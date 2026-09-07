#include "switchrecomp/execution/session.hpp"
#include "switchrecomp/version.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace switchrecomp;

enum class ExitCode : int
{
    Success = 0,
    InvalidArguments = 2,
    InfrastructureFailure = 3,
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

void help(std::ostream& output)
{
    output << "Usage: run-entry [options] prepared-main.nso\n\n"
              "Run one bounded prepared guest entry with the Semantic IR interpreter.\n\n"
              "Options:\n"
              "  --help                         Show this help text.\n"
              "  --version                      Show the project version.\n"
              "  --module-name NAME             Logical module name (default: main).\n"
              "  --module-base ADDR             Analysis-selected guest base.\n"
              "  --entry KIND                   dt-init, dt-fini, text-start, process.\n"
              "  --entry-address ADDR           Unverified analyst address.\n"
              "  --backend interpreter           M11 reference backend.\n"
              "  --stack-size N                 Synthetic guest stack size.\n"
              "  --report PATH                  Write deterministic JSON report.\n"
              "  --json                         Print deterministic JSON report.\n"
              "  --analysis-max-functions N     Analysis function budget.\n"
              "  --analysis-max-instructions N  Analysis instruction budget.\n"
              "  --analysis-max-blocks N        Analysis block budget.\n"
              "  --analysis-max-edges N         Analysis edge budget.\n"
              "  --analysis-max-seeds N         Analysis seed budget.\n"
              "  --analysis-max-bytes N         Analysis byte budget.\n"
              "  --max-ir-operations N          Global execution IR budget.\n"
              "  --max-function-transitions N   Global guest transition budget.\n"
              "  --max-call-depth N             Guest call-depth budget.\n"
              "  --max-events N                 Trace event budget.\n"
              "  --max-guest-blocks N           Guest block budget.\n";
}

void print_error(const Error& error)
{
    std::cerr << error_code_name(error.code) << ": " << error.message << '\n';
}

[[nodiscard]] bool take_value(int& index, int argc, char** argv, std::string_view option,
                              std::string& value)
{
    if (std::string_view(argv[index]) != option || index + 1 >= argc) return false;
    value = argv[++index];
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    std::string module_name = "main";
    std::uint64_t module_base = 0U;
    std::string entry_name = "dt-init";
    std::optional<std::uint64_t> analyst_address;
    std::string report_path;
    bool emit_json = false;
    std::filesystem::path input_path;
    analysis::PreparedModuleOptions load_options;
    execution::ExecutionSessionOptions execution_options;
    analysis::FunctionMapOptions function_options;
    function_options.budgets.max_functions = 5'000U;
    function_options.budgets.max_instructions = 200'000U;
    function_options.budgets.max_blocks = 50'000U;
    function_options.budgets.max_edges = 100'000U;
    function_options.budgets.max_seeds = 10'000U;
    function_options.budgets.max_bytes_analyzed = 16U * 1024U * 1024U;

    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument(argv[index]);
        if (argument == "--help")
        {
            help(std::cout);
            return static_cast<int>(ExitCode::Success);
        }
        if (argument == "--version")
        {
            std::cout << version << '\n';
            return static_cast<int>(ExitCode::Success);
        }
        std::string value;
        if (take_value(index, argc, argv, "--module-name", value))
        {
            module_name = value;
            continue;
        }
        if (take_value(index, argc, argv, "--module-base", value))
        {
            if (!parse_u64(value, module_base))
            {
                std::cerr << "invalid --module-base\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
            continue;
        }
        if (take_value(index, argc, argv, "--entry", value))
        {
            entry_name = value;
            continue;
        }
        if (take_value(index, argc, argv, "--entry-address", value))
        {
            std::uint64_t address = 0U;
            if (!parse_u64(value, address))
            {
                std::cerr << "invalid --entry-address\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
            analyst_address = address;
            continue;
        }
        if (take_value(index, argc, argv, "--backend", value))
        {
            if (value != "interpreter")
            {
                std::cerr << "only --backend interpreter is implemented for M11\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
            continue;
        }
        if (take_value(index, argc, argv, "--stack-size", value))
        {
            if (!parse_u64(value, execution_options.synthetic_stack_size))
            {
                std::cerr << "invalid --stack-size\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
            continue;
        }
        if (take_value(index, argc, argv, "--report", report_path)) continue;
        if (argument == "--json")
        {
            emit_json = true;
            continue;
        }

        bool invalid_number = false;
        const auto parse_number = [&]<typename T>(std::string_view name, T& destination) {
            if (argument != name) return false;
            if (index + 1 >= argc)
            {
                invalid_number = true;
                return true;
            }
            value = argv[++index];
            std::uint64_t parsed = 0U;
            if (!parse_u64(value, parsed) ||
                parsed > static_cast<std::uint64_t>(std::numeric_limits<T>::max()))
            {
                invalid_number = true;
                return true;
            }
            destination = static_cast<T>(parsed);
            return true;
        };
        if (parse_number("--analysis-max-functions", function_options.budgets.max_functions) ||
            parse_number("--analysis-max-instructions", function_options.budgets.max_instructions) ||
            parse_number("--analysis-max-blocks", function_options.budgets.max_blocks) ||
            parse_number("--analysis-max-edges", function_options.budgets.max_edges) ||
            parse_number("--analysis-max-seeds", function_options.budgets.max_seeds) ||
            parse_number("--analysis-max-bytes", function_options.budgets.max_bytes_analyzed) ||
            parse_number("--max-ir-operations", execution_options.budgets.max_ir_operations) ||
            parse_number("--max-function-transitions", execution_options.budgets.max_function_transitions) ||
            parse_number("--max-call-depth", execution_options.budgets.max_call_depth) ||
            parse_number("--max-events", execution_options.budgets.max_events) ||
            parse_number("--max-guest-blocks", execution_options.budgets.max_guest_blocks))
        {
            if (invalid_number)
            {
                std::cerr << "invalid numeric option\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
            continue;
        }

        if (!argument.empty() && argument[0] != '-' && input_path.empty())
        {
            input_path = argument;
            continue;
        }
        std::cerr << "unknown or incomplete argument: " << argument << '\n';
        return static_cast<int>(ExitCode::InvalidArguments);
    }

    // Keep the CLI's analysis controls separate from the metadata parser and
    // from the global execution budgets.
    load_options.module_name = module_name;
    load_options.module_base = module_base;

    if (input_path.empty())
    {
        help(std::cerr);
        return static_cast<int>(ExitCode::InvalidArguments);
    }
    if (entry_name == "process" && analyst_address)
    {
        std::cerr << "--entry process cannot be combined with --entry-address\n";
        return static_cast<int>(ExitCode::InvalidArguments);
    }
    if (analyst_address && entry_name == "dt-init") entry_name = "analyst";
    std::vector<std::byte> bytes;
    if (!read_file(input_path, bytes))
    {
        std::cerr << "unable to read input module\n";
        return static_cast<int>(ExitCode::InfrastructureFailure);
    }
    auto loaded = analysis::load_prepared_nso(bytes, load_options);
    if (!loaded)
    {
        print_error(loaded.error());
        return static_cast<int>(ExitCode::InfrastructureFailure);
    }
    const auto map = analysis::FunctionMapBuilder::build(
        analysis::ModuleAnalysisInput{loaded.value().identity, &loaded.value().memory,
                                      loaded.value().seeds},
        function_options);
    if (!map)
    {
        print_error(map.error());
        return static_cast<int>(ExitCode::InfrastructureFailure);
    }

    execution::EntrySelectionKind selection_kind;
    if (entry_name == "dt-init") selection_kind = execution::EntrySelectionKind::DynamicInit;
    else if (entry_name == "dt-fini") selection_kind = execution::EntrySelectionKind::DynamicFini;
    else if (entry_name == "text-start") selection_kind = execution::EntrySelectionKind::TextStartCandidate;
    else if (entry_name == "process") selection_kind = execution::EntrySelectionKind::VerifiedProcessEntry;
    else if (entry_name == "analyst") selection_kind = execution::EntrySelectionKind::AnalystAddress;
    else
    {
        std::cerr << "unknown --entry kind\n";
        return static_cast<int>(ExitCode::InvalidArguments);
    }
    const auto selected = execution::select_entry(map.value().identity(), selection_kind, analyst_address);
    if (!selected)
    {
        print_error(selected.error());
        return static_cast<int>(ExitCode::InfrastructureFailure);
    }
    execution::ExecutionSession session(loaded.value().memory, map.value(),
                                        loaded.value().unresolved_relocations, execution_options,
                                        execution::ExecutionLoadSummary{
                                            loaded.value().relocations.size(),
                                            loaded.value().applied_relocations,
                                            loaded.value().unresolved_relocations.size()});
    const auto run = session.run(selected.value());
    if (!run)
    {
        print_error(run.error());
        return static_cast<int>(ExitCode::InfrastructureFailure);
    }
    const auto report = execution::render_execution_report_json(run.value());
    if (!report_path.empty())
    {
        std::ofstream output(report_path, std::ios::binary);
        if (!output)
        {
            std::cerr << "unable to write report\n";
            return static_cast<int>(ExitCode::InfrastructureFailure);
        }
        output << report;
        if (!output)
        {
            std::cerr << "unable to finish report\n";
            return static_cast<int>(ExitCode::InfrastructureFailure);
        }
    }
    if (emit_json)
    {
        std::cout << report;
    }
    else
    {
        std::cout << "STOPPED: " << execution::execution_stop_reason_name(run.value().stop_reason)
                  << " pc=" << std::hex << run.value().stop_pc << std::dec << '\n';
    }
    return static_cast<int>(ExitCode::Success);
}
