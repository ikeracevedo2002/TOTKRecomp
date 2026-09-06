#include "switchrecomp/codegen/llvm_lowerer.hpp"
#include "switchrecomp/codegen/llvm_object_emitter.hpp"
#include "switchrecomp/analysis/cfg_analyzer.hpp"
#include "switchrecomp/analysis/control_flow_graph.hpp"
#include "switchrecomp/lift/aarch64_lifter.hpp"
#include "switchrecomp/memory/guest_memory.hpp"
#include "switchrecomp/ir/printer.hpp"
#include "switchrecomp/version.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

enum class ExitCode : int
{
    Success = 0,
    GeneralError = 1,
    InvalidArguments = 2,
    LiftFailure = 3,
};

void print_help(std::ostream& output)
{
    output << "Usage: aarch64-lift [options] file\n\n"
              "Lift a raw, little-endian AArch64 code image into SwitchRecomp semantic IR.\n\n"
              "Options:\n"
              "  --help                 Show this help text.\n"
              "  --version              Show the project version.\n"
              "  --base ADDRESS        Guest base address (default: 0x1000).\n"
              "  --entry ADDRESS       Guest entry address (default: base).\n"
              "  --print-llvm           Print verified LLVM IR after semantic lifting.\n"
              "  --emit-object PATH     Emit a native object file for the host.\n"
              "  --max-instructions N   Maximum decoded instructions.\n"
              "  --max-blocks N         Maximum basic blocks.\n"
              "\nThe input is project-owned synthetic data or a locally supplied prepared image;\n"
              "this tool does not extract or decrypt game content.\n";
}

[[nodiscard]] bool parse_u64(std::string_view text, std::uint64_t& value)
{
    int base = 10;
    if (text.size() > 2U && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    {
        text.remove_prefix(2U);
        base = 16;
    }
    if (text.empty())
    {
        return false;
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

[[nodiscard]] bool parse_size(std::string_view text, std::size_t& value)
{
    std::uint64_t parsed = 0U;
    if (!parse_u64(text, parsed) || parsed > std::numeric_limits<std::size_t>::max())
    {
        return false;
    }
    value = static_cast<std::size_t>(parsed);
    return true;
}

[[nodiscard]] bool read_file(const std::filesystem::path& path, std::vector<std::byte>& bytes)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return false;
    }
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0)
    {
        return false;
    }
    input.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(end));
    if (!bytes.empty())
    {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return input.good() || input.eof();
    }
    return true;
}

void print_error(const switchrecomp::Error& error)
{
    std::cerr << switchrecomp::error_code_name(error.code) << ": " << error.message << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc == 1)
        {
            print_help(std::cerr);
            return static_cast<int>(ExitCode::InvalidArguments);
        }
        std::filesystem::path input_path;
        std::uint64_t base = 0x1000U;
        std::optional<std::uint64_t> entry;
        std::optional<std::filesystem::path> object_path;
        bool print_llvm = false;
        switchrecomp::analysis::AnalysisOptions options;
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view argument(argv[index]);
            const auto require_value = [&](std::string_view option) -> std::string_view {
                if (index + 1 >= argc)
                {
                    throw std::runtime_error("missing value for " + std::string(option));
                }
                ++index;
                return argv[index];
            };
            if (argument == "--help" || argument == "-h")
            {
                print_help(std::cout);
                return static_cast<int>(ExitCode::Success);
            }
            if (argument == "--version")
            {
                std::cout << switchrecomp::version << '\n';
                return static_cast<int>(ExitCode::Success);
            }
            if (argument == "--print-llvm")
            {
                print_llvm = true;
                continue;
            }
            if (argument == "--base" || argument == "--entry")
            {
                const auto value = require_value(argument);
                std::uint64_t parsed = 0U;
                if (!parse_u64(value, parsed))
                {
                    throw std::runtime_error("invalid address: " + std::string(value));
                }
                if (argument == "--base")
                    base = parsed;
                else
                    entry = parsed;
                continue;
            }
            if (argument == "--emit-object")
            {
                object_path = require_value(argument);
                continue;
            }
            if (argument == "--max-instructions" || argument == "--max-blocks")
            {
                const auto value = require_value(argument);
                std::size_t parsed = 0U;
                if (!parse_size(value, parsed) || parsed == 0U)
                {
                    throw std::runtime_error("invalid positive limit: " + std::string(value));
                }
                if (argument == "--max-instructions")
                    options.max_instructions = parsed;
                else
                    options.max_basic_blocks = parsed;
                continue;
            }
            if (!input_path.empty())
            {
                throw std::runtime_error("unexpected argument: " + std::string(argument));
            }
            input_path = argument;
        }
        if (input_path.empty())
        {
            print_help(std::cerr);
            return static_cast<int>(ExitCode::InvalidArguments);
        }
        const auto actual_entry = entry.value_or(base);
        if ((base & 0x3U) != 0U || (actual_entry & 0x3U) != 0U)
        {
            throw std::runtime_error("base and entry addresses must be 4-byte aligned");
        }
        std::vector<std::byte> bytes;
        if (!read_file(input_path, bytes))
        {
            std::cerr << "cannot read input: " << input_path << '\n';
            return static_cast<int>(ExitCode::GeneralError);
        }
        switchrecomp::memory::GuestMemory memory;
        const auto mapped = memory.map(
            base, bytes, switchrecomp::memory::GuestMemoryPermissions::Read |
                         switchrecomp::memory::GuestMemoryPermissions::Execute,
            "synthetic-code", switchrecomp::memory::GuestRegionKind::Text);
        if (!mapped)
        {
            print_error(mapped.error());
            return static_cast<int>(ExitCode::LiftFailure);
        }
        const auto graph = switchrecomp::analysis::analyze_control_flow(memory, actual_entry, options);
        if (!graph)
        {
            print_error(graph.error());
            return static_cast<int>(ExitCode::LiftFailure);
        }
        const auto function = switchrecomp::lift::lift_aarch64(graph.value());
        if (!function)
        {
            print_error(function.error());
            return static_cast<int>(ExitCode::LiftFailure);
        }
        std::cout << "entry: 0x" << std::hex << actual_entry << std::dec << '\n'
                  << "blocks: " << function.value().blocks.size() << '\n'
                  << "instructions: " << graph.value().instruction_count << '\n'
                  << "status: liftable\n\nsemantic-ir:\n"
                  << switchrecomp::ir::print_function(function.value());
        if (print_llvm || object_path)
        {
            const auto llvm = switchrecomp::codegen::lower_to_llvm(function.value());
            if (!llvm)
            {
                print_error(llvm.error());
                return static_cast<int>(ExitCode::LiftFailure);
            }
            if (print_llvm)
            {
                std::cout << "\nllvm:\n" << llvm.value().textual_ir;
            }
            if (object_path)
            {
                const auto emitted = switchrecomp::codegen::emit_native_object(
                    function.value(), *object_path);
                if (!emitted)
                {
                    print_error(emitted.error());
                    return static_cast<int>(ExitCode::LiftFailure);
                }
                std::cout << "object: " << *object_path << '\n';
            }
        }
        return static_cast<int>(ExitCode::Success);
    }
    catch (const std::exception& error)
    {
        std::cerr << "invalid arguments: " << error.what() << '\n';
        return static_cast<int>(ExitCode::InvalidArguments);
    }
}
