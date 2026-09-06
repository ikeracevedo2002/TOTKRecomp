#include "switchrecomp/analysis/cfg_analyzer.hpp"
#include "switchrecomp/analysis/control_flow_graph.hpp"
#include "switchrecomp/common/error.hpp"
#include "switchrecomp/interpreter/interpreter.hpp"
#include "switchrecomp/ir/printer.hpp"
#include "switchrecomp/lifter/lifter.hpp"
#include "switchrecomp/memory/guest_memory.hpp"
#ifdef TOTKRECOMP_HAS_LLVM
#include "switchrecomp/codegen/llvm_backend.hpp"
#endif

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using switchrecomp::Error;
using switchrecomp::Result;

void print_help()
{
    std::cout << "Usage: aarch64-lift --hex HEX [options]\n"
                 "\nOptions:\n"
                 "  --address HEX       Guest entry address (default 0x100000)\n"
                 "  --x0 VALUE          Initial X0 value\n"
                 "  --x1 VALUE          Initial X1 value\n"
                 "  --show-disassembly  Print decoded instructions\n"
                 "  --show-ir           Print Semantic IR\n"
                 "  --show-llvm         Print lowered LLVM IR (LLVM build only)\n"
                 "  --execute-ir        Execute through the Semantic IR interpreter\n"
                 "  --execute-native    Execute through the LLVM native JIT\n"
                 "  --json              Print deterministic summary JSON\n"
                 "  --help              Show this help\n"
                 "  --version           Show version\n";
}

[[nodiscard]] Result<std::uint64_t> parse_integer(std::string_view text)
{
    int base = 10;
    if (text.size() > 2U && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    {
        text.remove_prefix(2U);
        base = 16;
    }
    std::uint64_t value = 0U;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value, base);
    if (parsed.ec != std::errc{} || parsed.ptr != end)
    {
        return Result<std::uint64_t>::failure(
            switchrecomp::make_error(switchrecomp::ErrorCode::InvalidArgument,
                                     "invalid integer: " + std::string(text)));
    }
    return Result<std::uint64_t>::success(value);
}

[[nodiscard]] Result<std::vector<std::byte>> parse_hex(std::string_view text)
{
    std::string compact;
    for (const char character : text)
    {
        if (character != ' ' && character != '\t' && character != ':' && character != '_')
        {
            compact.push_back(character);
        }
    }
    if (compact.empty() || compact.size() % 2U != 0U)
    {
        return Result<std::vector<std::byte>>::failure(switchrecomp::make_error(
            switchrecomp::ErrorCode::InvalidArgument, "--hex requires a non-empty even number of digits"));
    }
    std::vector<std::byte> bytes;
    bytes.reserve(compact.size() / 2U);
    for (std::size_t index = 0U; index < compact.size(); index += 2U)
    {
        unsigned int value = 0U;
        const auto parsed = std::from_chars(compact.data() + static_cast<std::ptrdiff_t>(index),
                                            compact.data() + static_cast<std::ptrdiff_t>(index + 2U),
                                            value, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != compact.data() +
                                                          static_cast<std::ptrdiff_t>(index + 2U))
        {
            return Result<std::vector<std::byte>>::failure(switchrecomp::make_error(
                switchrecomp::ErrorCode::InvalidArgument, "--hex contains a non-hexadecimal byte"));
        }
        bytes.push_back(static_cast<std::byte>(value));
    }
    return Result<std::vector<std::byte>>::success(std::move(bytes));
}

[[nodiscard]] std::string json_escape(std::string_view text)
{
    std::string escaped;
    for (const char character : text)
    {
        if (character == '\\' || character == '"')
        {
            escaped += '\\';
        }
        if (character == '\n')
        {
            escaped += "\\n";
        }
        else
        {
            escaped += character;
        }
    }
    return escaped;
}

void print_error(const Error& error)
{
    std::cerr << switchrecomp::error_code_name(error.code) << ": " << error.message << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    std::string hex;
    std::uint64_t address = 0x100000U;
    std::uint64_t x0 = 0U;
    std::uint64_t x1 = 0U;
    bool show_disassembly = false;
    bool show_ir = false;
    bool show_llvm = false;
    bool execute_ir = false;
    bool execute_native = false;
    bool json = false;

    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument(argv[index]);
        const auto next_value = [&](std::string_view option) -> Result<std::string_view> {
            if (index + 1 >= argc)
            {
                return Result<std::string_view>::failure(switchrecomp::make_error(
                    switchrecomp::ErrorCode::InvalidArgument, std::string(option) + " requires a value"));
            }
            ++index;
            return Result<std::string_view>::success(argv[index]);
        };
        if (argument == "--help")
        {
            print_help();
            return 0;
        }
        if (argument == "--version")
        {
            std::cout << SWITCHRECOMP_VERSION << '\n';
            return 0;
        }
        if (argument == "--show-disassembly") show_disassembly = true;
        else if (argument == "--show-ir") show_ir = true;
        else if (argument == "--show-llvm") show_llvm = true;
        else if (argument == "--execute-ir") execute_ir = true;
        else if (argument == "--execute-native") execute_native = true;
        else if (argument == "--json") json = true;
        else if (argument == "--hex" || argument == "--address" || argument == "--x0" || argument == "--x1")
        {
            const auto value = next_value(argument);
            if (!value)
            {
                print_error(value.error());
                return 2;
            }
            if (argument == "--hex")
            {
                hex = std::string(value.value());
            }
            else
            {
                const auto parsed = parse_integer(value.value());
                if (!parsed)
                {
                    print_error(parsed.error());
                    return 2;
                }
                if (argument == "--address") address = parsed.value();
                else if (argument == "--x0") x0 = parsed.value();
                else x1 = parsed.value();
            }
        }
        else
        {
            print_error(switchrecomp::make_error(switchrecomp::ErrorCode::InvalidArgument,
                                                 "unknown option: " + std::string(argument)));
            return 2;
        }
    }
    if (hex.empty())
    {
        print_help();
        return 2;
    }
    const auto bytes = parse_hex(hex);
    if (!bytes)
    {
        print_error(bytes.error());
        return 2;
    }
    if (bytes.value().size() % 4U != 0U)
    {
        print_error(switchrecomp::make_error(switchrecomp::ErrorCode::InvalidArgument,
                                             "AArch64 input must contain complete 4-byte instructions"));
        return 2;
    }

    switchrecomp::memory::GuestMemory memory;
    const auto mapped = memory.map(address, std::span<const std::byte>(bytes.value()),
                                   switchrecomp::memory::GuestMemoryPermissions::Read |
                                       switchrecomp::memory::GuestMemoryPermissions::Execute,
                                   "aarch64-lift.text", switchrecomp::memory::GuestRegionKind::Text);
    if (!mapped)
    {
        print_error(mapped.error());
        return 1;
    }
    switchrecomp::analysis::AnalysisOptions analysis_options;
    analysis_options.allowed_code_range = switchrecomp::analysis::GuestAddressRange{
        address, static_cast<switchrecomp::memory::GuestSize>(bytes.value().size())};
    const auto cfg = switchrecomp::analysis::analyze_control_flow(memory, address, analysis_options);
    if (!cfg)
    {
        print_error(cfg.error());
        return 1;
    }
    const auto function = switchrecomp::lifter::lift_function(cfg.value());
    if (!function)
    {
        print_error(function.error());
        return 1;
    }

    if (show_disassembly)
    {
        std::cout << "Guest function: 0x" << std::hex << std::setw(16) << std::setfill('0') << address
                  << std::dec << "\n\nAArch64:\n";
        for (const auto& [_, block] : cfg.value().blocks)
        {
            for (const auto& instruction : block.instructions)
            {
                std::cout << "0x" << std::hex << instruction.address << std::dec << "  "
                          << instruction.disassembly << '\n';
            }
        }
    }
    if (show_ir)
    {
        std::cout << "\nSemantic IR:\n" << switchrecomp::ir::print(function.value()) << '\n';
    }

    switchrecomp::runtime::CpuState initial_cpu;
    initial_cpu.x[0] = x0;
    initial_cpu.x[1] = x1;
    switchrecomp::runtime::RuntimeContext initial_runtime{&memory};
    if (execute_ir || execute_native || show_llvm)
    {
        if (execute_ir)
        {
            auto cpu = initial_cpu;
            auto runtime = initial_runtime;
            const auto execution = switchrecomp::interpreter::execute(function.value(), cpu, runtime);
            if (!execution)
            {
                print_error(execution.error());
                return 1;
            }
            std::cout << "Execution (IR): X0 = 0x" << std::hex << cpu.x[0] << std::dec
                      << ", NZCV = " << static_cast<unsigned int>(cpu.n) << static_cast<unsigned int>(cpu.z)
                      << static_cast<unsigned int>(cpu.c) << static_cast<unsigned int>(cpu.v) << '\n';
        }
#ifdef TOTKRECOMP_HAS_LLVM
        if (execute_native)
        {
            const auto backend = switchrecomp::codegen::LlvmBackend::create();
            if (!backend)
            {
                print_error(backend.error());
                return 1;
            }
            auto cpu = initial_cpu;
            auto runtime = initial_runtime;
            const auto execution = backend.value()->execute(function.value(), cpu, runtime);
            if (!execution)
            {
                print_error(execution.error());
                return 1;
            }
            std::cout << "Execution (LLVM/native): X0 = 0x" << std::hex << cpu.x[0] << std::dec << '\n';
        }
#else
        if (execute_native || show_llvm)
        {
            print_error(switchrecomp::make_error(
                switchrecomp::ErrorCode::Unsupported,
                "aarch64-lift was built without LLVM; configure with -DTOTKRECOMP_ENABLE_LLVM=ON"));
            return 1;
        }
#endif
    }
#ifdef TOTKRECOMP_HAS_LLVM
    if (show_llvm)
    {
        const auto backend = switchrecomp::codegen::LlvmBackend::create();
        if (!backend)
        {
            print_error(backend.error());
            return 1;
        }
        const auto llvm = backend.value()->lower_to_llvm_ir(function.value());
        if (!llvm)
        {
            print_error(llvm.error());
            return 1;
        }
        std::cout << "LLVM IR:\n" << llvm.value() << '\n';
    }
#endif
    if (json)
    {
        const auto ir_text = switchrecomp::ir::print(function.value());
        std::cout << "{\"entry\":\"0x" << std::hex << std::setw(16) << std::setfill('0') << address
                  << "\",\"blocks\":" << std::dec << function.value().blocks().size()
                  << ",\"values\":" << function.value().values().size()
                  << ",\"ir\":\"" << json_escape(ir_text) << "\"}\n";
    }
    return 0;
}
