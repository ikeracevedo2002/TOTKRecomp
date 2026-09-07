#include "switchrecomp/analysis/whole_module.hpp"
#include "switchrecomp/common/error.hpp"
#include "switchrecomp/version.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

enum class ExitCode : int
{
    Success = 0,
    InvalidArguments = 2,
    TranslationBlocked = 3,
};

[[nodiscard]] bool parse_u64(std::string_view text, std::uint64_t& value)
{
    int base = 10;
    if (text.size() > 2U && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    {
        text.remove_prefix(2U);
        base = 16;
    }
    if (text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, base);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] bool read_file(const std::filesystem::path& path, std::vector<std::byte>& bytes)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0) return false;
    input.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(end));
    if (!bytes.empty())
    {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return input.good() || input.eof();
    }
    return true;
}

void print_help(std::ostream& output)
{
    output << "Usage: translate-module [options] prepared-main.nso\n\n"
              "Analyze and translate a complete prepared NSO module through Semantic IR.\n\n"
              "Options:\n"
              "  --help                 Show this help text.\n"
              "  --version              Show the project version.\n"
              "  --module-name NAME     Logical module name (default: main).\n"
              "  --module-base ADDR     Guest module base (default: 0).\n"
              "  --strict               Stop after the first translation blocker (default).\n"
              "  --diagnostic           Continue and report all translation blockers.\n"
              "  --llvm                 Attempt optional LLVM 18 lowering.\n"
              "  --no-relocations       Load metadata without applying relocations.\n"
              "  --emit-ir              Include verified Semantic IR in the report.\n"
              "  --json                 Emit deterministic machine-readable JSON.\n"
              "  --report PATH          Write the same report to PATH.\n"
              "  --emit-function-map PATH  Write the canonical function map JSON.\n"
              "  --max-functions N      Function discovery budget.\n"
              "  --max-instructions N   Instruction discovery budget.\n"
              "  --max-blocks N         Basic-block discovery budget.\n"
              "  --max-edges N          CFG edge discovery budget.\n"
              "  --max-seeds N          Function seed budget.\n"
              "  --max-bytes N          Executable bytes analyzed budget.\n"
              "\nThe input must already be legally prepared executable data; this tool does not\n"
              "extract, decrypt, or distribute Nintendo content.\n";
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
        std::filesystem::path report_path;
        std::filesystem::path function_map_path;
        switchrecomp::analysis::PreparedModuleOptions load_options;
        switchrecomp::analysis::TranslationOptions translation_options;
        bool json = false;
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view argument(argv[index]);
            const auto require_value = [&](std::string_view option) -> std::string_view {
                if (index + 1 >= argc) throw std::runtime_error(std::string(option) + " requires a value");
                ++index;
                return argv[index];
            };
            if (argument == "--help" || argument == "-h")
            {
                print_help(std::cout);
                return 0;
            }
            if (argument == "--version")
            {
                std::cout << switchrecomp::version << '\n';
                return 0;
            }
            if (argument == "--strict")
            {
                translation_options.mode = switchrecomp::analysis::TranslationMode::Strict;
                continue;
            }
            if (argument == "--diagnostic")
            {
                translation_options.mode = switchrecomp::analysis::TranslationMode::Diagnostic;
                continue;
            }
            if (argument == "--llvm")
            {
                translation_options.lower_llvm = true;
                continue;
            }
            if (argument == "--no-relocations")
            {
                load_options.apply_relocations = false;
                continue;
            }
            if (argument == "--emit-ir")
            {
                translation_options.include_ir = true;
                continue;
            }
            if (argument == "--json")
            {
                json = true;
                continue;
            }
            if (argument == "--module-name" || argument == "--report" ||
                argument == "--emit-function-map")
            {
                const auto value = require_value(argument);
                if (argument == "--module-name") load_options.module_name = value;
                else if (argument == "--report") report_path = value;
                else function_map_path = value;
                continue;
            }
            if (argument == "--module-base" || argument == "--max-functions" ||
                argument == "--max-instructions" || argument == "--max-blocks" ||
                argument == "--max-edges" || argument == "--max-seeds" || argument == "--max-bytes")
            {
                const auto value = require_value(argument);
                std::uint64_t parsed = 0U;
                if (!parse_u64(value, parsed) ||
                    (parsed == 0U && argument != "--module-base") ||
                    parsed > std::numeric_limits<std::size_t>::max())
                {
                    throw std::runtime_error("invalid positive numeric value: " + std::string(value));
                }
                if (argument == "--module-base") load_options.module_base = parsed;
                else if (argument == "--max-functions") translation_options.function_map.budgets.max_functions = static_cast<std::size_t>(parsed);
                else if (argument == "--max-instructions") translation_options.function_map.budgets.max_instructions = static_cast<std::size_t>(parsed);
                else if (argument == "--max-blocks") translation_options.function_map.budgets.max_blocks = static_cast<std::size_t>(parsed);
                else if (argument == "--max-edges") translation_options.function_map.budgets.max_edges = static_cast<std::size_t>(parsed);
                else if (argument == "--max-seeds") translation_options.function_map.budgets.max_seeds = static_cast<std::size_t>(parsed);
                else translation_options.function_map.budgets.max_bytes_analyzed = parsed;
                continue;
            }
            if (!input_path.empty()) throw std::runtime_error("unexpected argument: " + std::string(argument));
            input_path = argument;
        }
        if (input_path.empty())
        {
            print_help(std::cerr);
            return static_cast<int>(ExitCode::InvalidArguments);
        }

        std::vector<std::byte> bytes;
        if (!read_file(input_path, bytes))
        {
            std::cerr << "cannot read input: " << input_path << '\n';
            return static_cast<int>(ExitCode::InvalidArguments);
        }
        const auto module = switchrecomp::analysis::load_prepared_nso(bytes, load_options);
        if (!module)
        {
            print_error(module.error());
            return static_cast<int>(ExitCode::TranslationBlocked);
        }
        const auto translated = switchrecomp::analysis::translate_module(module.value(), translation_options);
        if (!translated)
        {
            print_error(translated.error());
            return static_cast<int>(ExitCode::TranslationBlocked);
        }
        const auto report = json ? switchrecomp::analysis::render_translation_report_json(translated.value())
                                 : switchrecomp::analysis::render_translation_report(translated.value());
        std::cout << report;
        if (json || report.empty() || report.back() != '\n') std::cout << '\n';
        if (!report_path.empty())
        {
            std::ofstream output(report_path, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                std::cerr << "cannot write report: " << report_path << '\n';
                return static_cast<int>(ExitCode::TranslationBlocked);
            }
            output << report << '\n';
        }
        if (!function_map_path.empty())
        {
            std::ofstream output(function_map_path, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                std::cerr << "cannot write function map: " << function_map_path << '\n';
                return static_cast<int>(ExitCode::TranslationBlocked);
            }
            output << switchrecomp::analysis::render_function_map_json(
                         translated.value().function_map)
                   << '\n';
        }
        return translated.value().succeeded() ? static_cast<int>(ExitCode::Success)
                                              : static_cast<int>(ExitCode::TranslationBlocked);
    }
    catch (const std::exception& error)
    {
        std::cerr << "invalid arguments: " << error.what() << '\n';
        return static_cast<int>(ExitCode::InvalidArguments);
    }
}
