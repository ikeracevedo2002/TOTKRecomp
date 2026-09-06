#include "switchrecomp/analysis/cfg_analyzer.hpp"
#include "switchrecomp/analysis/coverage.hpp"
#include "switchrecomp/analysis/control_flow_graph.hpp"
#include "switchrecomp/common/error.hpp"
#include "switchrecomp/format/nso.hpp"
#include "switchrecomp/memory/guest_memory.hpp"
#include "switchrecomp/version.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
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
    AnalysisFailure = 3,
};

void print_help(std::ostream& output)
{
    output << "Usage: aarch64-analyze [options] file\n\n"
              "Analyze a raw, little-endian AArch64 code image in GuestMemory.\n\n"
              "Options:\n"
              "  --help                 Show this help text.\n"
              "  --version              Show the project version.\n"
              "  --base ADDRESS        Guest base address (default: 0x1000).\n"
              "  --entry ADDRESS       Guest entry address (default: base).\n"
              "  --max-instructions N  Maximum decoded instructions.\n"
              "  --max-blocks N        Maximum basic blocks.\n"
              "  --coverage             Scan every instruction in the input range.\n"
              "  --json                 Emit deterministic JSON coverage output.\n"
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
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, value, base);
    return result.ec == std::errc{} && result.ptr == last;
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
    std::cerr << error_code_name(error.code) << ": " << error.message << '\n';
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
        switchrecomp::analysis::AnalysisOptions options;
        bool coverage = false;
        bool json = false;
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view argument(argv[index]);
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
            if (argument == "--coverage")
            {
                coverage = true;
                continue;
            }
            if (argument == "--json")
            {
                json = true;
                coverage = true;
                continue;
            }
            const auto require_value = [&](std::string_view option_name) -> std::string_view {
                if (index + 1 >= argc)
                {
                    throw std::runtime_error("missing value for " + std::string(option_name));
                }
                ++index;
                return argv[index];
            };
            if (argument == "--base")
            {
                const auto value = require_value(argument);
                if (!parse_u64(value, base))
                {
                    throw std::runtime_error("invalid base address: " + std::string(value));
                }
                continue;
            }
            if (argument == "--entry")
            {
                const auto value = require_value(argument);
                std::uint64_t parsed = 0U;
                if (!parse_u64(value, parsed))
                {
                    throw std::runtime_error("invalid entry address: " + std::string(value));
                }
                entry = parsed;
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
                {
                    options.max_instructions = parsed;
                }
                else
                {
                    options.max_basic_blocks = parsed;
                }
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
        if ((base & 0x3U) != 0U || (entry && (*entry & 0x3U) != 0U))
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
        switchrecomp::memory::GuestAddress analysis_base = base;
        switchrecomp::memory::GuestSize analysis_size = static_cast<switchrecomp::memory::GuestSize>(bytes.size());
        switchrecomp::Result<void> mapped = switchrecomp::Result<void>::success();
        const bool is_nso = bytes.size() >= 4U && std::to_integer<unsigned char>(bytes[0]) == 'N' &&
                            std::to_integer<unsigned char>(bytes[1]) == 'S' &&
                            std::to_integer<unsigned char>(bytes[2]) == 'O' &&
                            std::to_integer<unsigned char>(bytes[3]) == '0';
        if (is_nso)
        {
            const auto header = switchrecomp::format::parse_nso_header(bytes);
            if (!header)
            {
                print_error(header.error());
                return static_cast<int>(ExitCode::AnalysisFailure);
            }
            const auto image = switchrecomp::format::materialize_nso(bytes, header.value());
            if (!image)
            {
                print_error(image.error());
                return static_cast<int>(ExitCode::AnalysisFailure);
            }
            analysis_base = image.value().text.memory_offset;
            analysis_size = static_cast<switchrecomp::memory::GuestSize>(image.value().text.bytes.size());
            mapped = memory.map(analysis_base, image.value().text.bytes,
                                switchrecomp::memory::GuestMemoryPermissions::Read |
                                    switchrecomp::memory::GuestMemoryPermissions::Execute,
                                ".text", switchrecomp::memory::GuestRegionKind::Text);
        }
        else
        {
            mapped = memory.map(base, bytes,
                                switchrecomp::memory::GuestMemoryPermissions::Read |
                                    switchrecomp::memory::GuestMemoryPermissions::Execute,
                                "synthetic-code", switchrecomp::memory::GuestRegionKind::Text);
        }
        if (!mapped)
        {
            print_error(mapped.error());
            return static_cast<int>(ExitCode::AnalysisFailure);
        }
        if (coverage)
        {
            const auto report = switchrecomp::analysis::scan_coverage(
                memory, analysis_base, analysis_size, std::filesystem::path(input_path).filename().string(),
                switchrecomp::analysis::CoverageOptions{options.max_instructions});
            if (!report)
            {
                print_error(report.error());
                return static_cast<int>(ExitCode::AnalysisFailure);
            }
            std::cout << (json ? switchrecomp::analysis::render_coverage_json(report.value())
                               : switchrecomp::analysis::render_coverage(report.value()));
            if (json)
            {
                std::cout << '\n';
            }
            return static_cast<int>(ExitCode::Success);
        }
        const auto actual_entry = entry.value_or(analysis_base);
        const auto graph = switchrecomp::analysis::analyze_control_flow(memory, actual_entry, options);
        if (!graph)
        {
            print_error(graph.error());
            return static_cast<int>(ExitCode::AnalysisFailure);
        }
        std::cout << switchrecomp::analysis::render_control_flow_graph(graph.value());
        return static_cast<int>(ExitCode::Success);
    }
    catch (const std::exception& error)
    {
        std::cerr << "invalid arguments: " << error.what() << '\n';
        return static_cast<int>(ExitCode::InvalidArguments);
    }
}
