#include "switchrecomp/execution/session.hpp"
#include "switchrecomp/analysis/indirect_target.hpp"
#include "switchrecomp/analysis/module_set.hpp"
#include "switchrecomp/analysis/process_image.hpp"
#include "switchrecomp/target/manifest.hpp"
#include "switchrecomp/version.hpp"

#include <algorithm>
#include <chrono>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
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

[[nodiscard]] bool read_file(const std::filesystem::path& path, std::size_t max_size,
                             std::vector<std::byte>& bytes)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > max_size) return false;
    input.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(end));
    if (!bytes.empty())
    {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return input.good() || input.eof();
    }
    return true;
}

[[nodiscard]] Result<void> apply_target_manifest(
    const nlohmann::json& root, analysis::ModuleSetIngestionOptions& options)
{
    if (!root.contains("target_manifest") || !root.at("target_manifest").is_string())
        return Result<void>::success();
    std::vector<std::byte> bytes;
    if (!read_file(root.at("target_manifest").get<std::string>(), 1024U * 1024U, bytes))
    {
        return Result<void>::failure(make_error(
            ErrorCode::ModuleManifestMismatch, "target manifest could not be read"));
    }
    const auto manifest = target::parse_manifest(std::string_view(
        reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    if (!manifest) return Result<void>::failure(manifest.error());
    if (manifest.value().support_status == target::SupportStatus::Template)
    {
        if (options.completeness == analysis::ModuleSetCompleteness::ManifestVerifiedComplete)
            return Result<void>::failure(make_error(
                ErrorCode::ModuleManifestMismatch,
                "a template target manifest cannot establish manifest-verified completeness"));
        return Result<void>::success();
    }
    if (manifest.value().support_status == target::SupportStatus::Unsupported)
    {
        return Result<void>::failure(make_error(
            ErrorCode::ModuleManifestMismatch, "target manifest marks this target unsupported"));
    }
    options.expected_logical_names.clear();
    options.expected_modules.clear();
    for (const auto& module : manifest.value().modules)
    {
        options.expected_logical_names.push_back(module.name);
        options.expected_modules.push_back(analysis::ModuleSetIngestionOptions::ExpectedModule{
            module.name, module.sha256, module.build_id, module.expected_size});
    }
    options.completeness = analysis::ModuleSetCompleteness::ManifestVerifiedComplete;
    options.completeness_basis = analysis::ModuleSetCompletenessBasis::TargetManifestMatch;
    options.coherence = analysis::ModuleSetCoherence::Verified;
    options.coherence_basis = "target_manifest_identity_match";
    return Result<void>::success();
}

void help(std::ostream& output)
{
    output << "Usage: run-entry [options] prepared-main.nso\n"
              "       run-entry --local-config <file> [options]\n"
              "       run-entry --module NAME=PATH [--module NAME=PATH ...] [options]\n\n"
              "Run one bounded prepared guest entry with the Semantic IR interpreter.\n\n"
              "Options:\n"
              "  --help                         Show this help text.\n"
              "  --version                      Show the project version.\n"
              "  --module-name NAME             Logical module name (default: main).\n"
              "  --module-base ADDR             Analysis-selected guest base.\n"
              "  --module NAME=PATH             Add a process module (repeatable).\n"
              "  --local-config PATH             Read a local-only multi-module config.\n"
              "  --module-directory PATH        Scan a prepared non-recursive module directory.\n"
              "  --module-base-for NAME=ADDR    Set one process module analysis base.\n"
              "  --entry KIND                   dt-init, dt-fini, text-start, process.\n"
              "  --entry-address ADDR           Unverified analyst address.\n"
              "  --analysis-focus-symbol NAME  Analyze only the bounded startup/provider closure for NAME.\n"
              "  --analysis-workers N          Bounded independent refinement-analysis workers.\n"
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
              "  --analysis-max-boundary-passes N Boundary-finalization budget.\n"
              "  --analysis-profile NAME        whole_module or execution_closure.\n"
              "  --refinement-max-stagnant-rounds N No-progress refinement retry budget.\n"
              "  --refinement-max-rounds N      Deprecated alias for stagnant-round budget.\n"
              "  --refinement-max-candidates N  Explicit legacy unique-candidate ceiling.\n"
              "  --refinement-max-assessments N Explicit legacy candidate-assessment ceiling.\n"
              "  --refinement-max-promotions N  Deprecated legacy event guard.\n"
              "  --refinement-max-rebuilds N    Deprecated legacy event guard.\n"
              "  --refinement-max-analysis-functions N       Cumulative refinement CFG-function work.\n"
              "  --refinement-max-reanalysis-functions N     Cumulative refinement reanalysis work.\n"
              "  --refinement-max-analysis-instructions N    Cumulative refinement instruction work.\n"
              "  --refinement-max-analysis-blocks N          Cumulative refinement block work.\n"
              "  --refinement-max-analysis-edges N           Cumulative refinement edge work.\n"
              "  --refinement-max-analysis-bytes N           Cumulative refinement byte work.\n"
              "  --refinement-max-analysis-boundary-passes N Cumulative boundary-finalization work.\n"
              "  --refinement-max-invalidated-records N      Cumulative invalidation work.\n"
              "  --refinement-max-analysis-transactions N    Explicit finite transaction compatibility ceiling.\n"
              "  --max-ir-operations N          Explicit global execution IR hard limit.\n"
              "  --max-function-transitions N   Global guest transition budget.\n"
              "  --max-call-depth N             Guest call-depth budget.\n"
              "  --max-events N                 Explicit compatibility execution-event guard.\n"
              "  --event-history-limit N        Bounded diagnostic event-history capacity.\n"
              "  --max-guest-blocks N           Guest block budget.\n";
}

void print_error(const Error& error)
{
    std::cerr << error_code_name(error.code) << ": " << error.message << '\n';
    if (error.budget_context)
    {
        const auto& context = error.budget_context.value();
        std::cerr << "analysis_budget_context: domain=" << context.domain
                  << " dimension=" << context.dimension << " consumed=" << context.consumed
                  << " limit=" << context.limit << " module=" << context.module
                  << " phase=" << context.phase << " pending=" << context.pending_work;
        if (context.next_work)
            std::cerr << " next=0x" << std::hex << context.next_work.value() << std::dec;
        std::cerr << '\n';
    }
}

[[nodiscard]] std::string refinement_exhaustion_diagnostic(
    const analysis::IndirectTargetRefinementSummary& summary)
{
    const auto& exhaustion = summary.exhaustion;
    std::string result =
        "indirect target refinement " +
        std::string(analysis::indirect_target_refinement_budget_dimension_name(
            exhaustion.dimension)) +
        " budget exhausted (" + std::to_string(exhaustion.consumed) + "/" +
        std::to_string(exhaustion.limit) + ") module=" + exhaustion.module +
        " generation=" + std::to_string(exhaustion.generation) +
        " pending=" + std::to_string(summary.pending_candidate_count);
    if (exhaustion.next_work)
    {
        std::ostringstream next;
        next << " next_candidate=" << exhaustion.next_work->target_module << ":0x" << std::hex
             << exhaustion.next_work->target << std::dec;
        result += next.str();
    }
    return result;
}

[[nodiscard]] bool take_value(int& index, int argc, char** argv, std::string_view option,
                              std::string& value)
{
    if (std::string_view(argv[index]) != option || index + 1 >= argc) return false;
    value = argv[++index];
    return true;
}

[[nodiscard]] bool parse_assignment(std::string_view text, std::string& left,
                                    std::string& right)
{
    const auto separator = text.find('=');
    if (separator == std::string_view::npos || separator == 0U || separator + 1U >= text.size())
    {
        return false;
    }
    left = text.substr(0U, separator);
    right = text.substr(separator + 1U);
    return !left.empty() && !right.empty();
}

[[nodiscard]] std::optional<analysis::AnalysisBudgetDimension> analysis_dimension_for_option(
    std::string_view argument)
{
    if (argument == "--analysis-max-functions")
        return analysis::AnalysisBudgetDimension::Functions;
    if (argument == "--analysis-max-instructions")
        return analysis::AnalysisBudgetDimension::Instructions;
    if (argument == "--analysis-max-blocks") return analysis::AnalysisBudgetDimension::Blocks;
    if (argument == "--analysis-max-edges") return analysis::AnalysisBudgetDimension::Edges;
    if (argument == "--analysis-max-seeds") return analysis::AnalysisBudgetDimension::Seeds;
    if (argument == "--analysis-max-bytes")
        return analysis::AnalysisBudgetDimension::BytesAnalyzed;
    if (argument == "--analysis-max-boundary-passes")
        return analysis::AnalysisBudgetDimension::BoundaryFinalizationPasses;
    return std::nullopt;
}

[[nodiscard]] std::optional<analysis::IndirectTargetRefinementAnalysisDimension>
refinement_analysis_dimension_for_option(std::string_view argument)
{
    if (argument == "--refinement-max-analysis-functions")
        return analysis::IndirectTargetRefinementAnalysisDimension::FunctionsAnalyzed;
    if (argument == "--refinement-max-reanalysis-functions")
        return analysis::IndirectTargetRefinementAnalysisDimension::FunctionsReanalyzed;
    if (argument == "--refinement-max-analysis-instructions")
        return analysis::IndirectTargetRefinementAnalysisDimension::Instructions;
    if (argument == "--refinement-max-analysis-blocks")
        return analysis::IndirectTargetRefinementAnalysisDimension::Blocks;
    if (argument == "--refinement-max-analysis-edges")
        return analysis::IndirectTargetRefinementAnalysisDimension::Edges;
    if (argument == "--refinement-max-analysis-bytes")
        return analysis::IndirectTargetRefinementAnalysisDimension::BytesAnalyzed;
    if (argument == "--refinement-max-analysis-boundary-passes")
        return analysis::IndirectTargetRefinementAnalysisDimension::BoundaryFinalizationPasses;
    if (argument == "--refinement-max-invalidated-records")
        return analysis::IndirectTargetRefinementAnalysisDimension::InvalidatedRecords;
    if (argument == "--refinement-max-analysis-transactions")
        return analysis::IndirectTargetRefinementAnalysisDimension::Transactions;
    return std::nullopt;
}

[[nodiscard]] std::optional<execution::EntrySelectionKind> entry_kind_for_name(
    std::string_view name)
{
    if (name == "dt-init") return execution::EntrySelectionKind::DynamicInit;
    if (name == "dt-fini") return execution::EntrySelectionKind::DynamicFini;
    if (name == "text-start") return execution::EntrySelectionKind::TextStartCandidate;
    if (name == "process") return execution::EntrySelectionKind::VerifiedProcessEntry;
    if (name == "analyst") return execution::EntrySelectionKind::AnalystAddress;
    return std::nullopt;
}

[[nodiscard]] std::size_t default_analysis_workers() noexcept
{
    const auto hardware = std::thread::hardware_concurrency();
    if (hardware == 0U) return 1U;
    return std::min<std::size_t>(4U, hardware);
}

[[nodiscard]] bool ranges_overlap(
    const std::vector<analysis::GuestAddressRange>& left,
    const std::vector<analysis::GuestAddressRange>& right) noexcept
{
    const auto end = [](const analysis::GuestAddressRange& range) noexcept {
        return range.size > std::numeric_limits<memory::GuestAddress>::max() - range.base
                   ? std::numeric_limits<memory::GuestAddress>::max()
                   : range.base + range.size;
    };
    for (const auto& left_range : left)
    {
        for (const auto& right_range : right)
        {
            if (left_range.base < end(right_range) && right_range.base < end(left_range))
                return true;
        }
    }
    return false;
}

[[nodiscard]] bool independent_candidates(
    const analysis::IndirectTargetAssessment& left,
    const analysis::IndirectTargetAssessment& right) noexcept
{
    return left.observed.target_module != right.observed.target_module ||
           !ranges_overlap(left.validation.candidate_owned_code_ranges,
                           right.validation.candidate_owned_code_ranges);
}

} // namespace

int main(int argc, char** argv)
{
    const bool profiling = []() noexcept {
        const auto* value = std::getenv("SWITCHRECOMP_PROFILE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    const auto wall_start = std::chrono::steady_clock::now();
    struct WallProfileExit
    {
        bool enabled;
        std::chrono::steady_clock::time_point start;
        ~WallProfileExit() noexcept
        {
            if (!enabled) return;
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count();
            std::cerr << "[switchrecomp profile] phase=total_wall_time elapsed_us=" << elapsed
                      << "\n";
        }
    } wall_profile_exit{profiling, wall_start};
    std::string module_name = "main";
    std::uint64_t module_base = 0U;
    std::string entry_name = "dt-init";
    std::optional<std::uint64_t> analyst_address;
    std::string report_path;
    bool emit_json = false;
    std::filesystem::path input_path;
    std::filesystem::path local_config_path;
    std::vector<std::pair<std::string, std::filesystem::path>> configured_modules;
    std::map<std::string, std::uint64_t> configured_bases;
    std::string configured_primary = "main";
    std::string analysis_focus_symbol;
    std::size_t analysis_workers = default_analysis_workers();
    bool provider_search_complete = false;
    analysis::ModuleSetCompleteness module_set_completeness =
        analysis::ModuleSetCompleteness::Incomplete;
    analysis::ModuleSetCompletenessBasis module_set_completeness_basis =
        analysis::ModuleSetCompletenessBasis::LegacyConfigFalse;
    analysis::ModuleSetCoherence module_set_coherence = analysis::ModuleSetCoherence::Unverified;
    std::string module_set_coherence_basis = "unknown";
    std::string module_set_source = "explicit";
    std::vector<std::string> expected_module_names;
    std::vector<analysis::ModuleSetIngestionOptions::ExpectedModule> expected_modules;
    std::filesystem::path module_directory;
    analysis::PreparedModuleOptions load_options;
    execution::ExecutionSessionOptions execution_options;
    analysis::FunctionMapOptions function_options;
    analysis::IndirectTargetRefinementBudgets refinement_budgets;
    function_options.budgets = analysis::make_execution_closure_analysis_budgets();
    std::set<analysis::AnalysisBudgetDimension> analysis_cli_overrides;
    std::set<analysis::IndirectTargetRefinementAnalysisDimension>
        refinement_analysis_cli_overrides;
    bool refinement_candidate_limit_cli_override = false;
    bool refinement_assessment_limit_cli_override = false;
    bool refinement_transaction_limit_cli_override = false;
    bool analysis_profile_cli_override = false;
    bool event_limit_cli_override = false;
    bool event_history_limit_cli_override = false;

    const auto apply_analysis_profile = [&](analysis::AnalysisBudgets profile,
                                            bool mark_as_cli_profile) {
        function_options.budgets.strategy = profile.strategy;
        const auto apply_dimension = [&](analysis::AnalysisBudgetDimension dimension,
                                         auto& destination, const auto& source) {
            if (analysis_cli_overrides.contains(dimension)) return;
            destination = source;
            if (mark_as_cli_profile)
            {
                analysis::mark_analysis_budget_override(
                    function_options.budgets, dimension,
                    analysis::AnalysisBudgetProvenanceKind::ExecutionToolProfile,
                    "cli_selected_analysis_profile");
            }
        };
        apply_dimension(analysis::AnalysisBudgetDimension::Functions,
                        function_options.budgets.max_functions, profile.max_functions);
        apply_dimension(analysis::AnalysisBudgetDimension::Instructions,
                        function_options.budgets.max_instructions, profile.max_instructions);
        apply_dimension(analysis::AnalysisBudgetDimension::Blocks,
                        function_options.budgets.max_blocks, profile.max_blocks);
        apply_dimension(analysis::AnalysisBudgetDimension::Edges,
                        function_options.budgets.max_edges, profile.max_edges);
        apply_dimension(analysis::AnalysisBudgetDimension::Seeds,
                        function_options.budgets.max_seeds, profile.max_seeds);
        apply_dimension(analysis::AnalysisBudgetDimension::BytesAnalyzed,
                        function_options.budgets.max_bytes_analyzed, profile.max_bytes_analyzed);
        apply_dimension(analysis::AnalysisBudgetDimension::BoundaryFinalizationPasses,
                        function_options.budgets.max_boundary_finalization_passes,
                        profile.max_boundary_finalization_passes);
    };

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
        if (take_value(index, argc, argv, "--local-config", value))
        {
            local_config_path = value;
            continue;
        }
        if (take_value(index, argc, argv, "--module-directory", value))
        {
            module_directory = value;
            continue;
        }
        if (take_value(index, argc, argv, "--module", value))
        {
            std::string name;
            std::string path;
            if (!parse_assignment(value, name, path))
            {
                std::cerr << "invalid --module; expected NAME=PATH\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
            configured_modules.emplace_back(std::move(name), std::filesystem::path(path));
            continue;
        }
        if (take_value(index, argc, argv, "--module-base-for", value))
        {
            std::string name;
            std::string address;
            std::uint64_t parsed_base = 0U;
            if (!parse_assignment(value, name, address) || !parse_u64(address, parsed_base))
            {
                std::cerr << "invalid --module-base-for; expected NAME=ADDR\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
            configured_bases[name] = parsed_base;
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
        if (take_value(index, argc, argv, "--analysis-focus-symbol", analysis_focus_symbol))
        {
            continue;
        }
        if (take_value(index, argc, argv, "--analysis-profile", value))
        {
            if (value == "execution_closure")
                apply_analysis_profile(analysis::make_execution_closure_analysis_budgets(), true);
            else if (value == "whole_module")
                apply_analysis_profile(analysis::AnalysisBudgets{}, true);
            else
            {
                std::cerr << "invalid --analysis-profile\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
            analysis_profile_cli_override = true;
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

        if (take_value(index, argc, argv, "--refinement-max-candidates", value))
        {
            std::uint64_t parsed = 0U;
            if (!parse_u64(value, parsed) || parsed == 0U ||
                parsed > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
            {
                std::cerr << "invalid numeric option\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
            refinement_budgets.max_unique_candidates = static_cast<std::size_t>(parsed);
            refinement_budgets.candidate_limit_provenance = analysis::AnalysisBudgetProvenance{
                analysis::AnalysisBudgetProvenanceKind::ExplicitCliOverride,
                "run_entry_cli"};
            refinement_candidate_limit_cli_override = true;
            continue;
        }

        if (take_value(index, argc, argv, "--max-ir-operations", value))
        {
            std::uint64_t parsed = 0U;
            if (!parse_u64(value, parsed) || parsed == 0U ||
                parsed > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
            {
                std::cerr << "invalid numeric option\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
            execution_options.budgets.max_ir_operations = static_cast<std::size_t>(parsed);
            execution_options.budgets.ir_operation_limit_provenance =
                execution::IrOperationLimitProvenance::ExplicitCli;
            continue;
        }

        if (take_value(index, argc, argv, "--max-events", value))
        {
            std::uint64_t parsed = 0U;
            if (!parse_u64(value, parsed) || parsed == 0U ||
                parsed > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
            {
                std::cerr << "invalid numeric option\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
            execution_options.budgets.max_events = static_cast<std::size_t>(parsed);
            execution_options.budgets.event_execution_limit_provenance =
                execution::EventExecutionLimitProvenance::ExplicitCli;
            event_limit_cli_override = true;
            continue;
        }

        if (take_value(index, argc, argv, "--event-history-limit", value))
        {
            std::uint64_t parsed = 0U;
            if (!parse_u64(value, parsed) || parsed == 0U ||
                parsed > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
            {
                std::cerr << "invalid numeric option\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
            execution_options.budgets.event_history_limit = static_cast<std::size_t>(parsed);
            event_history_limit_cli_override = true;
            continue;
        }

        if (take_value(index, argc, argv, "--refinement-max-assessments", value))
        {
            std::uint64_t parsed = 0U;
            if (!parse_u64(value, parsed) || parsed == 0U ||
                parsed > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
            {
                std::cerr << "invalid numeric option\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
            refinement_budgets.max_candidate_assessments = static_cast<std::size_t>(parsed);
            refinement_budgets.candidate_assessment_limit_provenance =
                analysis::AnalysisBudgetProvenance{
                    analysis::AnalysisBudgetProvenanceKind::ExplicitCliOverride,
                    "run_entry_cli"};
            refinement_assessment_limit_cli_override = true;
            continue;
        }

        if (take_value(index, argc, argv, "--refinement-max-analysis-transactions", value))
        {
            std::uint64_t parsed = 0U;
            if (!parse_u64(value, parsed) || parsed == 0U ||
                parsed > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
            {
                std::cerr << "invalid numeric option\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
            refinement_budgets.analysis.max_transactions = static_cast<std::size_t>(parsed);
            refinement_budgets.analysis.transactions_provenance =
                analysis::AnalysisBudgetProvenance{
                    analysis::AnalysisBudgetProvenanceKind::ExplicitCliOverride,
                    "run_entry_cli"};
            refinement_analysis_cli_overrides.insert(
                analysis::IndirectTargetRefinementAnalysisDimension::Transactions);
            refinement_transaction_limit_cli_override = true;
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
            if ((refinement_analysis_dimension_for_option(argument) ||
                 argument == "--analysis-workers") && parsed == 0U)
                invalid_number = true;
            return true;
        };
        if (parse_number("--analysis-max-functions", function_options.budgets.max_functions) ||
            parse_number("--analysis-max-instructions", function_options.budgets.max_instructions) ||
            parse_number("--analysis-max-blocks", function_options.budgets.max_blocks) ||
            parse_number("--analysis-max-edges", function_options.budgets.max_edges) ||
            parse_number("--analysis-max-seeds", function_options.budgets.max_seeds) ||
            parse_number("--analysis-max-bytes", function_options.budgets.max_bytes_analyzed) ||
            parse_number("--analysis-max-boundary-passes",
                         function_options.budgets.max_boundary_finalization_passes) ||
            parse_number("--analysis-workers", analysis_workers) ||
            parse_number("--refinement-max-stagnant-rounds",
                         refinement_budgets.max_stagnant_rounds) ||
            parse_number("--refinement-max-rounds", refinement_budgets.max_stagnant_rounds) ||
            parse_number("--refinement-max-promotions", refinement_budgets.max_promotions) ||
            parse_number("--refinement-max-rebuilds", refinement_budgets.max_map_rebuilds) ||
            parse_number("--refinement-max-analysis-functions",
                         refinement_budgets.analysis.max_functions_analyzed) ||
            parse_number("--refinement-max-reanalysis-functions",
                         refinement_budgets.analysis.max_functions_reanalyzed) ||
            parse_number("--refinement-max-analysis-instructions",
                         refinement_budgets.analysis.max_instructions) ||
            parse_number("--refinement-max-analysis-blocks",
                         refinement_budgets.analysis.max_blocks) ||
            parse_number("--refinement-max-analysis-edges",
                         refinement_budgets.analysis.max_edges) ||
            parse_number("--refinement-max-analysis-bytes",
                         refinement_budgets.analysis.max_bytes_analyzed) ||
            parse_number("--refinement-max-analysis-boundary-passes",
                         refinement_budgets.analysis.max_boundary_finalization_passes) ||
            parse_number("--refinement-max-invalidated-records",
                         refinement_budgets.analysis.max_invalidated_records) ||
            parse_number("--max-function-transitions", execution_options.budgets.max_function_transitions) ||
            parse_number("--max-call-depth", execution_options.budgets.max_call_depth) ||
            parse_number("--max-guest-blocks", execution_options.budgets.max_guest_blocks))
        {
            if (invalid_number)
            {
                std::cerr << "invalid numeric option\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
            if (const auto dimension = analysis_dimension_for_option(argument))
            {
                analysis_cli_overrides.insert(dimension.value());
                analysis::mark_analysis_budget_override(
                    function_options.budgets, dimension.value(),
                    analysis::AnalysisBudgetProvenanceKind::ExplicitCliOverride,
                    "run_entry_cli");
            }
            if (refinement_analysis_dimension_for_option(argument))
            {
                refinement_analysis_cli_overrides.insert(
                    refinement_analysis_dimension_for_option(argument).value());
                const auto provenance = analysis::AnalysisBudgetProvenance{
                    analysis::AnalysisBudgetProvenanceKind::ExplicitCliOverride,
                    "run_entry_cli"};
                switch (refinement_analysis_dimension_for_option(argument).value())
                {
                case analysis::IndirectTargetRefinementAnalysisDimension::FunctionsAnalyzed:
                    refinement_budgets.analysis.functions_analyzed_provenance = provenance;
                    break;
                case analysis::IndirectTargetRefinementAnalysisDimension::FunctionsReanalyzed:
                    refinement_budgets.analysis.functions_reanalyzed_provenance = provenance;
                    break;
                case analysis::IndirectTargetRefinementAnalysisDimension::Instructions:
                    refinement_budgets.analysis.instructions_provenance = provenance;
                    break;
                case analysis::IndirectTargetRefinementAnalysisDimension::Blocks:
                    refinement_budgets.analysis.blocks_provenance = provenance;
                    break;
                case analysis::IndirectTargetRefinementAnalysisDimension::Edges:
                    refinement_budgets.analysis.edges_provenance = provenance;
                    break;
                case analysis::IndirectTargetRefinementAnalysisDimension::BytesAnalyzed:
                    refinement_budgets.analysis.bytes_provenance = provenance;
                    break;
                case analysis::IndirectTargetRefinementAnalysisDimension::BoundaryFinalizationPasses:
                    refinement_budgets.analysis.boundary_finalization_provenance = provenance;
                    break;
                case analysis::IndirectTargetRefinementAnalysisDimension::InvalidatedRecords:
                    refinement_budgets.analysis.invalidated_records_provenance = provenance;
                    break;
                case analysis::IndirectTargetRefinementAnalysisDimension::Transactions:
                    refinement_budgets.analysis.transactions_provenance = provenance;
                    break;
                case analysis::IndirectTargetRefinementAnalysisDimension::None: break;
                }
            }
            if (argument == "--refinement-max-promotions" ||
                argument == "--refinement-max-rebuilds")
            {
                refinement_budgets.legacy_event_limits = true;
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

    if (!local_config_path.empty())
    {
        std::vector<std::byte> config_bytes;
        if (!read_file(local_config_path, 4U * 1024U * 1024U, config_bytes))
        {
            std::cerr << "unable to read local config\n";
            return static_cast<int>(ExitCode::InfrastructureFailure);
        }
        try
        {
            const auto root = nlohmann::json::parse(
                std::string(reinterpret_cast<const char*>(config_bytes.data()), config_bytes.size()));
            if (root.contains("analysis") && !root.at("analysis").is_object())
                throw std::runtime_error("analysis configuration must be an object");
            const auto analysis_object = root.contains("analysis")
                                             ? root.at("analysis")
                                             : nlohmann::json::object();
            const auto budget_object = analysis_object.contains("budgets")
                                           ? analysis_object.at("budgets")
                                           : analysis_object;
            if (!budget_object.is_object())
                throw std::runtime_error("analysis budgets configuration must be an object");
            std::optional<std::string> configured_profile;
            if (analysis_object.contains("profile"))
            {
                if (!analysis_object.at("profile").is_string())
                    throw std::runtime_error("analysis profile must be a string");
                configured_profile = analysis_object.at("profile").get<std::string>();
            }
            if (analysis_object.contains("strategy"))
            {
                if (!analysis_object.at("strategy").is_string())
                    throw std::runtime_error("analysis strategy must be a string");
                configured_profile = analysis_object.at("strategy").get<std::string>();
            }
            if (configured_profile)
            {
                if (analysis_profile_cli_override)
                {
                    if (configured_profile.value() != "execution_closure" &&
                        configured_profile.value() != "whole_module")
                        throw std::runtime_error("invalid analysis profile");
                }
                else if (configured_profile.value() == "execution_closure")
                {
                    apply_analysis_profile(analysis::make_execution_closure_analysis_budgets(), false);
                    function_options.budgets.strategy = analysis::AnalysisStrategy::ExecutionClosure;
                }
                else if (configured_profile.value() == "whole_module")
                {
                    apply_analysis_profile(analysis::AnalysisBudgets{}, false);
                    function_options.budgets.strategy = analysis::AnalysisStrategy::WholeModule;
                }
                else
                {
                    throw std::runtime_error("invalid analysis profile");
                }
                if (!analysis_profile_cli_override)
                {
                    for (const auto dimension : {
                             analysis::AnalysisBudgetDimension::Functions,
                             analysis::AnalysisBudgetDimension::Instructions,
                             analysis::AnalysisBudgetDimension::Blocks,
                             analysis::AnalysisBudgetDimension::Edges,
                             analysis::AnalysisBudgetDimension::Seeds,
                             analysis::AnalysisBudgetDimension::BytesAnalyzed,
                             analysis::AnalysisBudgetDimension::BoundaryFinalizationPasses})
                    {
                        if (!analysis_cli_overrides.contains(dimension))
                            analysis::mark_analysis_budget_override(
                                function_options.budgets, dimension,
                                analysis::AnalysisBudgetProvenanceKind::LocalConfigurationOverride,
                                "run_entry_local_config_profile");
                    }
                }
            }
            const auto parse_local_budget = [&](std::string_view key,
                                                analysis::AnalysisBudgetDimension dimension,
                                                auto& destination, std::uint64_t maximum) {
                if (!budget_object.contains(key)) return;
                const auto& value = budget_object.at(key);
                if (!value.is_number_unsigned())
                    throw std::runtime_error("analysis budget must be an unsigned integer");
                const auto parsed = value.get<std::uint64_t>();
                if (parsed == 0U || parsed > maximum)
                    throw std::runtime_error("analysis budget is zero or overflows its type");
                if (!analysis_cli_overrides.contains(dimension))
                {
                    destination = static_cast<std::remove_reference_t<decltype(destination)>>(parsed);
                    analysis::mark_analysis_budget_override(
                        function_options.budgets, dimension,
                        analysis::AnalysisBudgetProvenanceKind::LocalConfigurationOverride,
                        "run_entry_local_config");
                }
            };
            parse_local_budget("max_functions", analysis::AnalysisBudgetDimension::Functions,
                              function_options.budgets.max_functions,
                              std::numeric_limits<std::size_t>::max());
            parse_local_budget("max_instructions", analysis::AnalysisBudgetDimension::Instructions,
                              function_options.budgets.max_instructions,
                              std::numeric_limits<std::size_t>::max());
            parse_local_budget("max_blocks", analysis::AnalysisBudgetDimension::Blocks,
                              function_options.budgets.max_blocks,
                              std::numeric_limits<std::size_t>::max());
            parse_local_budget("max_edges", analysis::AnalysisBudgetDimension::Edges,
                              function_options.budgets.max_edges,
                              std::numeric_limits<std::size_t>::max());
            parse_local_budget("max_seeds", analysis::AnalysisBudgetDimension::Seeds,
                              function_options.budgets.max_seeds,
                              std::numeric_limits<std::size_t>::max());
            parse_local_budget("max_bytes_analyzed", analysis::AnalysisBudgetDimension::BytesAnalyzed,
                              function_options.budgets.max_bytes_analyzed,
                              std::numeric_limits<memory::GuestSize>::max());
            parse_local_budget("max_boundary_finalization_passes",
                              analysis::AnalysisBudgetDimension::BoundaryFinalizationPasses,
                              function_options.budgets.max_boundary_finalization_passes,
                              std::numeric_limits<std::size_t>::max());

            if (root.contains("execution") && !root.at("execution").is_object())
                throw std::runtime_error("execution configuration must be an object");
            const auto execution_object = root.contains("execution")
                                              ? root.at("execution")
                                              : nlohmann::json::object();
            const auto execution_budget_object = execution_object.contains("budgets")
                                                     ? execution_object.at("budgets")
                                                     : execution_object;
            if (!execution_budget_object.is_object())
                throw std::runtime_error("execution budgets configuration must be an object");
            if (!event_limit_cli_override && execution_budget_object.contains("max_events"))
            {
                const auto& value = execution_budget_object.at("max_events");
                if (!value.is_number_unsigned())
                    throw std::runtime_error("execution event limit must be an unsigned integer");
                const auto parsed = value.get<std::uint64_t>();
                if (parsed == 0U ||
                    parsed > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
                    throw std::runtime_error(
                        "execution event limit is zero or overflows its type");
                execution_options.budgets.max_events = static_cast<std::size_t>(parsed);
                execution_options.budgets.event_execution_limit_provenance =
                    execution::EventExecutionLimitProvenance::ExplicitLocalConfiguration;
            }
            if (!event_history_limit_cli_override &&
                execution_budget_object.contains("event_history_limit"))
            {
                const auto& value = execution_budget_object.at("event_history_limit");
                if (!value.is_number_unsigned())
                    throw std::runtime_error(
                        "event history limit must be an unsigned integer");
                const auto parsed = value.get<std::uint64_t>();
                if (parsed == 0U ||
                    parsed > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
                    throw std::runtime_error(
                        "event history limit is zero or overflows its type");
                execution_options.budgets.event_history_limit = static_cast<std::size_t>(parsed);
            }

            if (root.contains("refinement_analysis") &&
                !root.at("refinement_analysis").is_object())
                throw std::runtime_error("refinement_analysis configuration must be an object");
            const auto refinement_analysis_object =
                root.contains("refinement_analysis")
                    ? root.at("refinement_analysis")
                    : nlohmann::json::object();
            const auto refinement_budget_object =
                refinement_analysis_object.contains("budgets")
                    ? refinement_analysis_object.at("budgets")
                    : refinement_analysis_object;
            if (!refinement_budget_object.is_object())
                throw std::runtime_error(
                    "refinement_analysis budgets configuration must be an object");
            if (!refinement_candidate_limit_cli_override &&
                refinement_budget_object.contains("max_unique_candidates"))
            {
                const auto& value = refinement_budget_object.at("max_unique_candidates");
                if (!value.is_number_unsigned())
                    throw std::runtime_error(
                        "legacy candidate limit must be an unsigned integer");
                const auto parsed = value.get<std::uint64_t>();
                if (parsed == 0U ||
                    parsed > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
                    throw std::runtime_error(
                        "legacy candidate limit is zero or overflows its type");
                refinement_budgets.max_unique_candidates = static_cast<std::size_t>(parsed);
                refinement_budgets.candidate_limit_provenance =
                    analysis::AnalysisBudgetProvenance{
                        analysis::AnalysisBudgetProvenanceKind::LocalConfigurationOverride,
                    "run_entry_local_config"};
            }
            if (!refinement_assessment_limit_cli_override &&
                refinement_budget_object.contains("max_candidate_assessments"))
            {
                const auto& value = refinement_budget_object.at("max_candidate_assessments");
                if (!value.is_number_unsigned())
                    throw std::runtime_error(
                        "legacy candidate-assessment limit must be an unsigned integer");
                const auto parsed = value.get<std::uint64_t>();
                if (parsed == 0U ||
                    parsed > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
                    throw std::runtime_error(
                        "legacy candidate-assessment limit is zero or overflows its type");
                refinement_budgets.max_candidate_assessments =
                    static_cast<std::size_t>(parsed);
                refinement_budgets.candidate_assessment_limit_provenance =
                    analysis::AnalysisBudgetProvenance{
                        analysis::AnalysisBudgetProvenanceKind::LocalConfigurationOverride,
                        "run_entry_local_config"};
            }
            const auto parse_local_refinement_budget =
                [&](std::string_view key, auto& destination,
                    analysis::AnalysisBudgetProvenance& provenance,
                    std::uint64_t maximum,
                    analysis::IndirectTargetRefinementAnalysisDimension dimension) {
                    if (!refinement_budget_object.contains(key) ||
                        refinement_analysis_cli_overrides.contains(dimension))
                        return;
                    const auto& value = refinement_budget_object.at(key);
                    if (!value.is_number_unsigned())
                        throw std::runtime_error(
                            "refinement analysis budget must be an unsigned integer");
                    const auto parsed = value.get<std::uint64_t>();
                    if (parsed == 0U || parsed > maximum)
                        throw std::runtime_error(
                            "refinement analysis budget is zero or overflows its type");
                    destination = static_cast<std::remove_reference_t<decltype(destination)>>(parsed);
                    provenance = analysis::AnalysisBudgetProvenance{
                        analysis::AnalysisBudgetProvenanceKind::LocalConfigurationOverride,
                        "run_entry_local_config"};
                };
            parse_local_refinement_budget(
                "max_functions_analyzed", refinement_budgets.analysis.max_functions_analyzed,
                refinement_budgets.analysis.functions_analyzed_provenance,
                std::numeric_limits<std::size_t>::max(),
                analysis::IndirectTargetRefinementAnalysisDimension::FunctionsAnalyzed);
            parse_local_refinement_budget(
                "max_functions_reanalyzed", refinement_budgets.analysis.max_functions_reanalyzed,
                refinement_budgets.analysis.functions_reanalyzed_provenance,
                std::numeric_limits<std::size_t>::max(),
                analysis::IndirectTargetRefinementAnalysisDimension::FunctionsReanalyzed);
            parse_local_refinement_budget(
                "max_instructions", refinement_budgets.analysis.max_instructions,
                refinement_budgets.analysis.instructions_provenance,
                std::numeric_limits<std::size_t>::max(),
                analysis::IndirectTargetRefinementAnalysisDimension::Instructions);
            parse_local_refinement_budget(
                "max_blocks", refinement_budgets.analysis.max_blocks,
                refinement_budgets.analysis.blocks_provenance,
                std::numeric_limits<std::size_t>::max(),
                analysis::IndirectTargetRefinementAnalysisDimension::Blocks);
            parse_local_refinement_budget(
                "max_edges", refinement_budgets.analysis.max_edges,
                refinement_budgets.analysis.edges_provenance,
                std::numeric_limits<std::size_t>::max(),
                analysis::IndirectTargetRefinementAnalysisDimension::Edges);
            parse_local_refinement_budget(
                "max_bytes_analyzed", refinement_budgets.analysis.max_bytes_analyzed,
                refinement_budgets.analysis.bytes_provenance,
                std::numeric_limits<memory::GuestSize>::max(),
                analysis::IndirectTargetRefinementAnalysisDimension::BytesAnalyzed);
            parse_local_refinement_budget(
                "max_boundary_finalization_passes",
                refinement_budgets.analysis.max_boundary_finalization_passes,
                refinement_budgets.analysis.boundary_finalization_provenance,
                std::numeric_limits<std::size_t>::max(),
                analysis::IndirectTargetRefinementAnalysisDimension::BoundaryFinalizationPasses);
            parse_local_refinement_budget(
                "max_invalidated_records", refinement_budgets.analysis.max_invalidated_records,
                refinement_budgets.analysis.invalidated_records_provenance,
                std::numeric_limits<std::size_t>::max(),
                analysis::IndirectTargetRefinementAnalysisDimension::InvalidatedRecords);
            if (!refinement_transaction_limit_cli_override &&
                refinement_budget_object.contains("max_transactions"))
            {
                const auto& value = refinement_budget_object.at("max_transactions");
                if (!value.is_number_unsigned())
                    throw std::runtime_error(
                        "refinement transaction limit must be an unsigned integer");
                const auto parsed = value.get<std::uint64_t>();
                if (parsed == 0U ||
                    parsed > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
                    throw std::runtime_error(
                        "refinement transaction limit is zero or overflows its type");
                refinement_budgets.analysis.max_transactions =
                    static_cast<std::size_t>(parsed);
                refinement_budgets.analysis.transactions_provenance =
                    analysis::AnalysisBudgetProvenance{
                        analysis::AnalysisBudgetProvenanceKind::LocalConfigurationOverride,
                        "run_entry_local_config"};
            }
            const auto module_set = root.contains("module_set") && root.at("module_set").is_object()
                                        ? root.at("module_set") : nlohmann::json::object();
            const bool directory_source =
                (module_set.contains("source") && module_set.at("source").is_string() &&
                 module_set.at("source").get<std::string>() == "directory") ||
                (module_set.contains("directory") && module_set.at("directory").is_string());
            if (module_set.contains("directory") && module_set.at("directory").is_string())
                module_directory = module_set.at("directory").get<std::string>();
            const auto& module_object = module_set.contains("modules") &&
                                                module_set.at("modules").is_object()
                                            ? module_set.at("modules")
                                            : root.value("modules", nlohmann::json::object());
            if (!directory_source && !module_object.is_object())
            {
                std::cerr << "local config modules must be an object\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
            if (!directory_source)
            for (const auto& [name, path] : module_object.items())
            {
                if (!path.is_string())
                {
                    std::cerr << "local config module paths must be strings\n";
                    return static_cast<int>(ExitCode::InvalidArguments);
                }
                configured_modules.emplace_back(name, path.get<std::string>());
            }
            if (root.contains("primary_module") && root.at("primary_module").is_string())
            {
                configured_primary = root.at("primary_module").get<std::string>();
            }
            if (module_set.contains("primary_module") && module_set.at("primary_module").is_string())
                configured_primary = module_set.at("primary_module").get<std::string>();
            if (root.contains("provider_search_complete") &&
                root.at("provider_search_complete").is_boolean())
            {
                provider_search_complete = root.at("provider_search_complete").get<bool>();
                if (provider_search_complete)
                {
                    module_set_completeness = analysis::ModuleSetCompleteness::DeclaredComplete;
                    module_set_completeness_basis =
                        analysis::ModuleSetCompletenessBasis::ExplicitLocalAssertion;
                }
            }
            const auto& base_object = module_set.contains("module_bases") &&
                                              module_set.at("module_bases").is_object()
                                          ? module_set.at("module_bases")
                                          : root.value("module_bases", nlohmann::json::object());
            if (base_object.is_object())
            {
                for (const auto& [name, base] : base_object.items())
                {
                    std::uint64_t parsed = 0U;
                    if (base.is_number_unsigned()) parsed = base.get<std::uint64_t>();
                    else if (base.is_string() && !parse_u64(base.get<std::string>(), parsed))
                    {
                        std::cerr << "invalid local config module base\n";
                        return static_cast<int>(ExitCode::InvalidArguments);
                    }
                    else if (!base.is_number_unsigned())
                    {
                        std::cerr << "invalid local config module base\n";
                        return static_cast<int>(ExitCode::InvalidArguments);
                    }
                    configured_bases[name] = parsed;
                }
            }
            if (module_set.is_object())
            {
                if (module_set.contains("source") && module_set.at("source").is_string())
                {
                    module_set_source = module_set.at("source").get<std::string>();
                }
                // The directory path is captured before module entries are parsed.
                if (module_set.contains("completeness") && module_set.at("completeness").is_string())
                {
                    const auto completeness = module_set.at("completeness").get<std::string>();
                    if (completeness == "declared_complete")
                        module_set_completeness = analysis::ModuleSetCompleteness::DeclaredComplete;
                    else if (completeness == "manifest_verified_complete")
                        module_set_completeness = analysis::ModuleSetCompleteness::ManifestVerifiedComplete;
                    else if (completeness == "incomplete")
                        module_set_completeness = analysis::ModuleSetCompleteness::Incomplete;
                }
                if (module_set.contains("basis") && module_set.at("basis").is_string())
                {
                    const auto basis = module_set.at("basis").get<std::string>();
                    if (basis == "explicit_inventory")
                        module_set_completeness_basis = analysis::ModuleSetCompletenessBasis::ExplicitInventory;
                    else if (basis == "explicit_local_assertion")
                        module_set_completeness_basis = analysis::ModuleSetCompletenessBasis::ExplicitLocalAssertion;
                    else if (basis == "local_manifest_match")
                        module_set_completeness_basis = analysis::ModuleSetCompletenessBasis::LocalManifestMatch;
                    else if (basis == "target_manifest_match")
                        module_set_completeness_basis = analysis::ModuleSetCompletenessBasis::TargetManifestMatch;
                    else if (basis == "directory_scan_only")
                        module_set_completeness_basis = analysis::ModuleSetCompletenessBasis::DirectoryScanOnly;
                }
                if (module_set_completeness != analysis::ModuleSetCompleteness::Incomplete &&
                    module_set_completeness_basis == analysis::ModuleSetCompletenessBasis::LegacyConfigFalse)
                    module_set_completeness_basis = analysis::ModuleSetCompletenessBasis::ExplicitInventory;
                if (module_set.contains("expected_modules") && module_set.at("expected_modules").is_array())
                {
                    for (const auto& expected : module_set.at("expected_modules"))
                    {
                        if (expected.is_string())
                            expected_module_names.push_back(expected.get<std::string>());
                        else if (expected.is_object() && expected.contains("name") &&
                                 expected.contains("sha256") && expected.contains("build_id") &&
                                 expected.at("name").is_string() && expected.at("sha256").is_string() &&
                                 expected.at("build_id").is_string())
                        {
                            std::optional<std::uint64_t> expected_size;
                            if (expected.contains("expected_size"))
                            {
                                if (!expected.at("expected_size").is_number_unsigned())
                                {
                                    std::cerr << "invalid expected module size\n";
                                    return static_cast<int>(ExitCode::InvalidArguments);
                                }
                                expected_size = expected.at("expected_size").get<std::uint64_t>();
                            }
                            expected_modules.push_back(
                                analysis::ModuleSetIngestionOptions::ExpectedModule{
                                    expected.at("name").get<std::string>(),
                                    expected.at("sha256").get<std::string>(),
                                    expected.at("build_id").get<std::string>(), expected_size});
                            expected_module_names.push_back(expected.at("name").get<std::string>());
                        }
                        else
                        {
                            std::cerr << "invalid expected module identity\n";
                            return static_cast<int>(ExitCode::InvalidArguments);
                        }
                    }
                }
                if (module_set.contains("coherence") && module_set.at("coherence").is_string())
                {
                    const auto coherence = module_set.at("coherence").get<std::string>();
                    if (coherence == "verified") module_set_coherence = analysis::ModuleSetCoherence::Verified;
                    else if (coherence == "partially_verified") module_set_coherence = analysis::ModuleSetCoherence::PartiallyVerified;
                    else if (coherence == "conflicting") module_set_coherence = analysis::ModuleSetCoherence::Conflicting;
                }
                if (module_set.contains("coherence_basis") && module_set.at("coherence_basis").is_string())
                    module_set_coherence_basis = module_set.at("coherence_basis").get<std::string>();
            }
            analysis::ModuleSetIngestionOptions manifest_options;
            manifest_options.completeness = module_set_completeness;
            const auto manifest = apply_target_manifest(root, manifest_options);
            if (!manifest)
            {
                print_error(manifest.error());
                return static_cast<int>(ExitCode::InfrastructureFailure);
            }
            if (root.contains("target_manifest") && root.at("target_manifest").is_string())
            {
                module_set_completeness = manifest_options.completeness;
                module_set_completeness_basis = manifest_options.completeness_basis;
                module_set_coherence = manifest_options.coherence;
                module_set_coherence_basis = manifest_options.coherence_basis;
                expected_module_names = manifest_options.expected_logical_names;
                expected_modules = manifest_options.expected_modules;
            }
            if (directory_source && module_directory.empty())
            {
                std::cerr << "module_set directory source requires a directory\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
        }
        catch (const std::exception& error)
        {
            std::cerr << "invalid local config: " << error.what() << '\n';
            return static_cast<int>(ExitCode::InvalidArguments);
        }
    }

    const bool process_mode = !configured_modules.empty() || !module_directory.empty();
    if (process_mode)
    {
        if (local_config_path.empty() && configured_primary == "main" && module_name != "main" &&
            module_directory.empty())
        {
            configured_primary = module_name;
        }
        if (!input_path.empty() && module_directory.empty())
        {
            bool has_main = false;
            for (const auto& module : configured_modules) has_main |= module.first == configured_primary;
            if (!has_main) configured_modules.emplace_back(configured_primary, input_path);
        }
        std::sort(configured_modules.begin(), configured_modules.end(),
                  [](const auto& left, const auto& right) { return left.first < right.first; });
        for (std::size_t index = 1U; index < configured_modules.size(); ++index)
        {
            if (configured_modules[index - 1U].first == configured_modules[index].first)
            {
                std::cerr << "duplicate process module name\n";
                return static_cast<int>(ExitCode::InvalidArguments);
            }
        }
        if (configured_primary.empty()) configured_primary = "main";
        bool has_primary = false;
        for (const auto& module : configured_modules) has_primary |= module.first == configured_primary;
        if (!has_primary && module_directory.empty())
        {
            std::cerr << "primary process module is not configured\n";
            return static_cast<int>(ExitCode::InvalidArguments);
        }

        analysis::ModuleSetInventory inventory;
        if (!module_directory.empty())
        {
            analysis::DirectoryInventoryOptions inventory_options;
            inventory_options.source = "directory";
            inventory_options.completeness = module_set_completeness;
            inventory_options.completeness_basis = module_set_completeness_basis;
            inventory_options.coherence = module_set_coherence;
            inventory_options.coherence_basis = module_set_coherence_basis;
            inventory_options.expected_logical_names = expected_module_names;
            inventory_options.expected_modules = expected_modules;
            inventory_options.explicit_bases = configured_bases;
            if (module_set_completeness == analysis::ModuleSetCompleteness::Incomplete &&
                module_set_completeness_basis == analysis::ModuleSetCompletenessBasis::LegacyConfigFalse)
                inventory_options.completeness_basis = analysis::ModuleSetCompletenessBasis::DirectoryScanOnly;
            const auto scanned = analysis::scan_prepared_module_directory(module_directory, inventory_options);
            if (!scanned) { print_error(scanned.error()); return static_cast<int>(ExitCode::InfrastructureFailure); }
            inventory = std::move(scanned).value();
            configured_modules.clear();
            for (const auto& module : inventory.modules)
                configured_modules.emplace_back(module.logical_name, std::filesystem::path{});
        }
        else
        {
            std::vector<analysis::ModuleSetFileInput> file_inputs;
            file_inputs.reserve(configured_modules.size());
            for (const auto& [name, path] : configured_modules)
            {
                const auto found = configured_bases.find(name);
                file_inputs.push_back(analysis::ModuleSetFileInput{
                    name, path, found == configured_bases.end() ? std::nullopt
                                                                 : std::optional<memory::GuestAddress>(found->second)});
            }
            analysis::ModuleSetIngestionOptions inventory_options;
            inventory_options.source = module_set_source;
            inventory_options.completeness = module_set_completeness;
            inventory_options.completeness_basis = module_set_completeness_basis;
            inventory_options.coherence = module_set_coherence;
            inventory_options.coherence_basis = module_set_coherence_basis;
            inventory_options.expected_logical_names = expected_module_names;
            inventory_options.expected_modules = expected_modules;
            const auto loaded = analysis::ingest_module_files(file_inputs, inventory_options);
            if (!loaded) { print_error(loaded.error()); return static_cast<int>(ExitCode::InfrastructureFailure); }
            inventory = std::move(loaded).value();
        }
        std::vector<analysis::ProcessModuleInput> module_inputs = inventory.process_inputs();
        bool inventory_has_primary = false;
        for (const auto& module : inventory.modules) inventory_has_primary |= module.logical_name == configured_primary;
        if (!inventory_has_primary)
        {
            std::cerr << "primary process module is not present in the ingested module set\n";
            return static_cast<int>(ExitCode::InvalidArguments);
        }
        if (analyst_address && entry_name == "dt-init") entry_name = "analyst";
        const auto selection_kind = entry_kind_for_name(entry_name);
        if (!selection_kind)
        {
            std::cerr << "unknown --entry kind\n";
            return static_cast<int>(ExitCode::InvalidArguments);
        }
        analysis::ProcessImageOptions process_options;
        process_options.primary_module = configured_primary;
        process_options.provider_search_complete = provider_search_complete;
        process_options.module_set_completeness = inventory.completeness;
        process_options.module_set_completeness_basis = inventory.completeness_basis;
        process_options.module_set_coherence = inventory.coherence;
        process_options.module_set_coherence_basis = inventory.coherence_basis;
        process_options.module_set_source = inventory.source;
        process_options.ignored_module_entries = inventory.ignored_entries;
        process_options.module_order = analysis::ProcessModuleOrderEvidence{
            inventory.module_load_order, inventory.module_load_order_basis};
        process_options.module_options = load_options;
        process_options.module_options.module_base = 0U;
        process_options.module_options.module_name = "";
        auto process = analysis::load_process_image(module_inputs, process_options);
        if (!process)
        {
            print_error(process.error());
            return static_cast<int>(ExitCode::InfrastructureFailure);
        }
        const auto candidate_universe =
            analysis::derive_indirect_target_candidate_universe(process.value());
        if (!candidate_universe)
        {
            print_error(candidate_universe.error());
            return static_cast<int>(ExitCode::InfrastructureFailure);
        }
        refinement_budgets.candidate_universe = candidate_universe.value();
        const auto* primary_process_module = process.value().module(configured_primary);
        if (primary_process_module == nullptr)
        {
            std::cerr << "primary process module has no process-image identity\n";
            return static_cast<int>(ExitCode::InfrastructureFailure);
        }
        const auto selected = execution::select_entry(
            primary_process_module->identity, selection_kind.value(), analyst_address);
        if (!selected)
        {
            print_error(selected.error());
            return static_cast<int>(ExitCode::InfrastructureFailure);
        }
        std::vector<analysis::FinalizedFunctionMap> maps;
        maps.reserve(process.value().modules().size());
        const auto focus_provider = analysis_focus_symbol.empty()
                                         ? analysis::ProviderLookup{}
                                         : process.value().lookup_provider(analysis_focus_symbol);
        for (const auto& module : process.value().modules())
        {
            std::vector<analysis::FunctionSeed> seeds = module.seeds;
            analysis::FunctionMapOptions module_function_options = function_options;
            const bool execution_closure =
                module_function_options.budgets.strategy == analysis::AnalysisStrategy::ExecutionClosure ||
                !analysis_focus_symbol.empty();
            if (execution_closure)
            {
                seeds.clear();
                for (const auto& seed : module.seeds)
                {
                    const bool is_focus_symbol = seed.name &&
                                                 seed.name.value() == analysis_focus_symbol;
                    const bool is_primary_init = module.identity.module == configured_primary &&
                                                 seed.source == analysis::FunctionDiscoverySource::AnalystSeed &&
                                                 seed.note.find("DT_INIT") != std::string::npos;
                    const bool is_selected_entry = module.identity.module == configured_primary &&
                                                   (seed.entry == selected.value().address ||
                                                    seed.canonical_entry.value_or(seed.entry) ==
                                                        selected.value().address);
                    if ((!analysis_focus_symbol.empty() &&
                         (is_focus_symbol || is_primary_init || is_selected_entry)) ||
                        (analysis_focus_symbol.empty() && is_selected_entry))
                        seeds.push_back(seed);
                }
                if (focus_provider.selected_candidate &&
                    focus_provider.selected_candidate.value() < focus_provider.candidates.size())
                {
                    const auto& candidate = focus_provider.candidates[
                        focus_provider.selected_candidate.value()];
                    if (candidate.module == module.identity.module &&
                        std::none_of(seeds.begin(), seeds.end(), [&](const auto& seed) {
                            return seed.entry == candidate.address;
                        }))
                    {
                        seeds.push_back(analysis::FunctionSeed{
                            candidate.address, analysis::FunctionDiscoverySource::DynamicSymbol,
                            analysis::FunctionConfidence::Confirmed, std::nullopt,
                            std::optional<std::string>(analysis_focus_symbol),
                            "M15 selected provider closure seed"});
                    }
                }
                if (module.identity.module == configured_primary && seeds.empty())
                {
                    seeds.push_back(analysis::FunctionSeed{
                        selected.value().address, analysis::FunctionDiscoverySource::ManualOverride,
                        analysis::FunctionConfidence::Manual, std::nullopt, std::nullopt,
                        "selected execution entry closure root"});
                }
                if (seeds.empty()) continue;
                module_function_options.execution_closure_roots.clear();
                for (const auto& seed : seeds)
                {
                    module_function_options.execution_closure_roots.insert(seed.entry);
                    module_function_options.execution_closure_roots.insert(
                        seed.canonical_entry.value_or(seed.entry));
                }
                module_function_options.budgets.strategy = analysis::AnalysisStrategy::ExecutionClosure;
            }
            const auto map = analysis::FunctionMapBuilder::build(
                analysis::ModuleAnalysisInput{module.identity, &process.value().memory(), std::move(seeds)},
                module_function_options);
            if (!map)
            {
                print_error(map.error());
                return static_cast<int>(ExitCode::InfrastructureFailure);
            }
            maps.push_back(std::move(map).value());
        }
        const auto process_map_result = analysis::ProcessFunctionMap::build(std::move(maps));
        if (!process_map_result)
        {
            print_error(process_map_result.error());
            return static_cast<int>(ExitCode::InfrastructureFailure);
        }
        analysis::ProcessFunctionMap process_map = std::move(process_map_result).value();
        const auto* primary_map = [&]() -> const analysis::FinalizedFunctionMap* {
            for (const auto& map : process_map.maps())
                if (map.identity().module == configured_primary) return &map;
            return nullptr;
        }();
        if (primary_map == nullptr)
        {
            std::cerr << "primary process module has no finalized function map\n";
            return static_cast<int>(ExitCode::InfrastructureFailure);
        }
        runtime::RuntimeImportRegistry runtime_imports;
        const auto registered = runtime::register_m12_evidence_imports(runtime_imports);
        if (!registered)
        {
            print_error(registered.error());
            return static_cast<int>(ExitCode::InfrastructureFailure);
        }
        execution::ExecutionLoadSummary summary;
        for (const auto& module : process.value().modules())
        {
            summary.relocations_parsed += module.relocations.size();
            summary.relocations_applied += module.applied_relocations;
            summary.unresolved_relocations += module.unresolved_relocations.size();
        }
        analysis::IndirectTargetDiscoveryOptions discovery_options;
        discovery_options.budgets = function_options.budgets;
        discovery_options.cfg = function_options.cfg;
        analysis::IndirectTargetRefinementWorklist worklist(refinement_budgets);
        const auto refinement_start = std::chrono::steady_clock::now();
        std::int64_t assessment_elapsed_us = 0;
        std::int64_t refinement_publication_elapsed_us = 0;
        std::vector<analysis::IndirectTargetAssessment> promoted_targets;
        std::optional<execution::ExecutionSessionResult> last_run_result;
        execution::ExecutionSessionResult final_result;
        for (;;)
        {
            if (!worklist.begin_round())
            {
                if (last_run_result) final_result = std::move(last_run_result.value());
                final_result.indirect_target_refinement = worklist.summary();
                final_result.stop_reason =
                    execution::ExecutionStopReason::IndirectTargetRefinementBudgetExceeded;
                const auto summary = worklist.summary();
                final_result.diagnostic = refinement_exhaustion_diagnostic(summary);
                break;
            }
            execution::ExecutionSession session(process.value().memory(), process_map,
                                                process.value(), execution_options, summary,
                                                &runtime_imports);
            const auto run = session.run(selected.value());
            if (!run)
            {
                print_error(run.error());
                return static_cast<int>(ExitCode::InfrastructureFailure);
            }
            auto run_result = std::move(run).value();
            if (run_result.stop_reason == execution::ExecutionStopReason::GuestMemoryResourceLimitExceeded &&
                last_run_result)
            {
                // A new refinement generation owns a fresh controlled stack.
                // If the process-wide guest-memory resource prevents that
                // generation from starting, retain the last complete
                // execution evidence and attach the actual next blocker.
                auto blocked_result = std::move(last_run_result.value());
                blocked_result.stop_reason = run_result.stop_reason;
                blocked_result.diagnostic = run_result.diagnostic;
                blocked_result.indirect_target_refinement = worklist.summary();
                final_result = std::move(blocked_result);
                break;
            }
            if (run_result.stop_reason == execution::ExecutionStopReason::UnknownGuestFunction)
            {
                for (const auto& assessment : run_result.indirect_target_discovery)
                {
                    (void)worklist.observe(assessment);
                }
            }
            bool refined = false;
            const auto pending = worklist.pending_candidates();
            std::vector<analysis::ObservedIndirectTarget> assessment_candidates;
            assessment_candidates.reserve(pending.size());
            for (const auto& candidate : pending)
            {
                const auto identity = analysis::indirect_target_candidate_identity(candidate);
                if (!worklist.begin_candidate_assessment(identity) || !worklist.can_promote()) break;
                assessment_candidates.push_back(candidate);
            }
            const auto assessment_start = std::chrono::steady_clock::now();
            auto assessed = analysis::assess_indirect_targets(
                assessment_candidates, process.value().memory(), process_map, process.value(),
                discovery_options, analysis_workers);
            assessment_elapsed_us += std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - assessment_start).count();
            if (!assessed)
            {
                print_error(assessed.error());
                return static_cast<int>(ExitCode::InfrastructureFailure);
            }
            auto assessments = std::move(assessed).value();
            std::vector<std::size_t> selected_indices;
            for (std::size_t index = 0U; index < assessments.size(); ++index)
            {
                auto& assessment = assessments[index];
                const auto identity = analysis::indirect_target_candidate_identity(
                    assessment.observed);
                if (!assessment.decision.eligible_for_promotion ||
                    assessment.decision.kind != analysis::IndirectTargetDecisionKind::TrustedNewEntry)
                {
                    if (assessment.validation.analysis_error)
                        worklist.record_failed_refinement(identity);
                    else
                        worklist.record_terminal_candidate(identity);
                    continue;
                }
                bool independent = true;
                for (const auto selected_index : selected_indices)
                {
                    if (!independent_candidates(assessment, assessments[selected_index]))
                    {
                        independent = false;
                        break;
                    }
                }
                if (independent) selected_indices.push_back(index);
            }
            if (!selected_indices.empty())
            {
                const auto map_generation_before = worklist.summary().map_generation;
                std::vector<analysis::IndirectTargetAssessment> batch;
                batch.reserve(selected_indices.size());
                for (const auto index : selected_indices)
                    batch.push_back(std::move(assessments[index]));
                const auto publication_start = std::chrono::steady_clock::now();
                auto expansion = analysis::refine_process_function_map_batch(
                    process_map, process.value(), batch, discovery_options);
                refinement_publication_elapsed_us +=
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - publication_start).count();
                if (!expansion)
                {
                    print_error(expansion.error());
                    return static_cast<int>(ExitCode::InfrastructureFailure);
                }
                std::vector<analysis::IndirectTargetAssessment> fallback_failures;
                if (!expansion.value().all_promoted && batch.size() > 1U)
                {
                    // A combined analysis is an optimization, never a new
                    // source of semantic failure. Retry each selected
                    // candidate in stable order against the unchanged map
                    // until one publishes successfully. If every singleton
                    // fails, all attempted candidates are terminalized below
                    // so a failed optimization cannot strand the round with
                    // hidden same-generation work.
                    for (const auto& candidate : batch)
                    {
                        std::vector<analysis::IndirectTargetAssessment> singleton;
                        singleton.push_back(candidate);
                        const auto retry_start = std::chrono::steady_clock::now();
                        expansion = analysis::refine_process_function_map_batch(
                            process_map, process.value(), singleton, discovery_options);
                        refinement_publication_elapsed_us +=
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - retry_start).count();
                        if (!expansion)
                        {
                            print_error(expansion.error());
                            return static_cast<int>(ExitCode::InfrastructureFailure);
                        }
                        if (expansion.value().all_promoted) break;
                        fallback_failures.push_back(
                            std::move(expansion.value().assessments.front()));
                    }
                }
                if (!expansion)
                {
                    print_error(expansion.error());
                    return static_cast<int>(ExitCode::InfrastructureFailure);
                }
                if (!expansion.value().all_promoted)
                {
                    const auto& failed_assessments = fallback_failures.empty()
                                                         ? expansion.value().assessments
                                                         : fallback_failures;
                    for (const auto& assessment : failed_assessments)
                    {
                        const auto identity = analysis::indirect_target_candidate_identity(
                            assessment.observed);
                        if (assessment.validation.analysis_error)
                            worklist.record_failed_refinement(identity);
                        else
                            worklist.record_terminal_candidate(identity);
                    }
                }
                else
                {
                    std::vector<analysis::IndirectTargetCandidateIdentity> identities;
                    identities.reserve(expansion.value().assessments.size());
                    for (const auto& assessment : expansion.value().assessments)
                        identities.push_back(analysis::indirect_target_candidate_identity(
                            assessment.observed));
                    if (!worklist.can_commit_batch_refinement(
                            identities, expansion.value().analysis_work,
                            expansion.value().module_maps_rebuilt,
                            expansion.value().module_maps_reused))
                    {
                        for (const auto& identity : identities)
                            worklist.record_rollback_assessment(identity);
                    }
                    else
                    {
                        process_map = std::move(expansion.value().map);
                        for (auto& assessment : expansion.value().assessments)
                        {
                            assessment.map_generation_before = map_generation_before;
                            assessment.map_generation_after = map_generation_before + 1U;
                            promoted_targets.push_back(std::move(assessment));
                        }
                        worklist.record_batch_promotion(
                            identities, expansion.value().module_maps_rebuilt,
                            expansion.value().module_maps_reused,
                            expansion.value().analysis_work,
                            expansion.value().finalized_functions_reused,
                            expansion.value().cfgs_reused,
                            expansion.value().functions_rebuilt,
                            expansion.value().modules_touched,
                            expansion.value().incremental_updates,
                            expansion.value().full_rebuilds);
                        refined = true;
                    }
                }
            }
            worklist.end_round();
            if (refined)
            {
                last_run_result = std::move(run_result);
                continue;
            }

            if (worklist.exhausted())
            {
                run_result.stop_reason =
                    execution::ExecutionStopReason::IndirectTargetRefinementBudgetExceeded;
                const auto refinement = worklist.summary();
                run_result.diagnostic = refinement_exhaustion_diagnostic(refinement);
            }
            run_result.indirect_target_refinement = worklist.summary();
            final_result = std::move(run_result);
            break;
        }
        for (const auto& promoted : promoted_targets)
        {
            auto merged = promoted;
            const auto runtime_assessment = std::find_if(
                final_result.indirect_target_discovery.begin(),
                final_result.indirect_target_discovery.end(),
                [&](const auto& item) {
                    return item.observed.target == promoted.observed.target &&
                           item.observed.source_pc == promoted.observed.source_pc;
                });
            if (runtime_assessment != final_result.indirect_target_discovery.end())
            {
                merged.guest_code_entered = runtime_assessment->guest_code_entered;
                merged.first_guest_pc = runtime_assessment->first_guest_pc;
                merged.first_guest_opcode = runtime_assessment->first_guest_opcode;
                merged.next_guest_pc = runtime_assessment->next_guest_pc;
            }
            final_result.indirect_target_discovery.erase(
                std::remove_if(final_result.indirect_target_discovery.begin(),
                               final_result.indirect_target_discovery.end(),
                               [&](const auto& item) {
                                   return item.observed.target == promoted.observed.target &&
                                          item.observed.source_pc == promoted.observed.source_pc;
                }),
                final_result.indirect_target_discovery.end());
            final_result.indirect_target_discovery.push_back(std::move(merged));
        }
        if (profiling)
        {
            const auto refinement = worklist.summary();
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - refinement_start).count();
            std::cerr << "[switchrecomp profile] phase=refinement_total elapsed_us=" << elapsed
                      << " rounds=" << refinement.total_execution_attempts
                      << " productive_rounds=" << refinement.productive_rounds
                      << " stagnant_rounds=" << refinement.stagnant_rounds
                      << " transactions=" << refinement.analysis.transactions
                      << " rebuilds=" << refinement.map_rebuilds
                      << " functions_analyzed=" << refinement.analysis.functions_analyzed
                      << " functions_reanalyzed=" << refinement.analysis.functions_reanalyzed
                      << " candidate_assessments=" << refinement.candidate_assessments
                      << " assessment_elapsed_us=" << assessment_elapsed_us
                      << " publication_elapsed_us=" << refinement_publication_elapsed_us
                      << " worker_count=" << analysis_workers
                      << " batch_count=" << refinement.refinement_batches
                      << " batch_candidates=" << refinement.batch_candidates
                      << " singleton_batches=" << refinement.singleton_batches
                      << " rebuilds_avoided=" << refinement.rebuilds_avoided
                      << " average_batch_width="
                      << (refinement.refinement_batches == 0U
                              ? 0.0
                              : static_cast<double>(refinement.batch_candidates) /
                                    static_cast<double>(refinement.refinement_batches))
                      << " max_batch_width=" << refinement.max_batch_width
                      << " finalized_functions_reused="
                      << refinement.finalized_functions_reused
                      << " cfgs_reused=" << refinement.cfgs_reused
                      << " functions_rebuilt=" << refinement.functions_rebuilt
                      << " modules_touched=" << refinement.modules_touched
                      << " incremental_updates=" << refinement.incremental_updates
                      << " full_rebuilds=" << refinement.full_rebuilds << "\n";
        }
        // Every loop-local ExecutionSession has now been destroyed, including
        // the terminal or failed generation. Capture post-lifetime accounting
        // so the report distinguishes live storage from cumulative churn.
        final_result.guest_memory = process.value().memory().accounting();
        const auto report = execution::render_execution_report_json(final_result);
        if (!report_path.empty())
        {
            std::ofstream output(report_path, std::ios::binary);
            if (!output) { std::cerr << "unable to write report\n"; return static_cast<int>(ExitCode::InfrastructureFailure); }
            output << report;
            if (!output) { std::cerr << "unable to finish report\n"; return static_cast<int>(ExitCode::InfrastructureFailure); }
        }
        if (emit_json) std::cout << report;
        else std::cout << "STOPPED: " << execution::execution_stop_reason_name(final_result.stop_reason)
                        << " pc=" << std::hex << final_result.stop_pc << std::dec << '\n';
        return static_cast<int>(ExitCode::Success);
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
    const auto selection_kind = entry_kind_for_name(entry_name);
    if (!selection_kind)
    {
        std::cerr << "unknown --entry kind\n";
        return static_cast<int>(ExitCode::InvalidArguments);
    }
    std::vector<std::byte> bytes;
    if (!read_file(input_path, 512U * 1024U * 1024U, bytes))
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
    const auto selected = execution::select_entry(
        loaded.value().identity, selection_kind.value(), analyst_address);
    if (!selected)
    {
        print_error(selected.error());
        return static_cast<int>(ExitCode::InfrastructureFailure);
    }
    analysis::FunctionMapOptions module_function_options = function_options;
    std::vector<analysis::FunctionSeed> seeds = loaded.value().seeds;
    if (module_function_options.budgets.strategy == analysis::AnalysisStrategy::ExecutionClosure)
    {
        seeds.clear();
        for (const auto& seed : loaded.value().seeds)
        {
            if (seed.entry == selected.value().address ||
                seed.canonical_entry.value_or(seed.entry) == selected.value().address)
                seeds.push_back(seed);
        }
        if (seeds.empty())
        {
            seeds.push_back(analysis::FunctionSeed{
                selected.value().address, analysis::FunctionDiscoverySource::ManualOverride,
                analysis::FunctionConfidence::Manual, std::nullopt, std::nullopt,
                "selected execution entry closure root"});
        }
        module_function_options.execution_closure_roots.clear();
        for (const auto& seed : seeds)
        {
            module_function_options.execution_closure_roots.insert(seed.entry);
            module_function_options.execution_closure_roots.insert(
                seed.canonical_entry.value_or(seed.entry));
        }
    }
    const auto map = analysis::FunctionMapBuilder::build(
        analysis::ModuleAnalysisInput{loaded.value().identity, &loaded.value().memory,
                                      std::move(seeds)},
        module_function_options);
    if (!map)
    {
        print_error(map.error());
        return static_cast<int>(ExitCode::InfrastructureFailure);
    }

    runtime::RuntimeImportRegistry runtime_imports;
    const auto registered = runtime::register_m12_evidence_imports(runtime_imports);
    if (!registered)
    {
        print_error(registered.error());
        return static_cast<int>(ExitCode::InfrastructureFailure);
    }
    std::optional<execution::ExecutionSessionResult> single_run;
    {
        execution::ExecutionSession session(
            loaded.value().memory, map.value(), loaded.value().unresolved_relocations,
            execution_options,
            execution::ExecutionLoadSummary{loaded.value().relocations.size(),
                                             loaded.value().applied_relocations,
                                             loaded.value().unresolved_relocations.size()},
            &runtime_imports, &loaded.value().metadata,
            loaded.value().symbols ? &loaded.value().symbols.value() : nullptr);
        const auto run = session.run(selected.value());
        if (!run)
        {
            print_error(run.error());
            return static_cast<int>(ExitCode::InfrastructureFailure);
        }
        single_run = std::move(run).value();
    }
    single_run->guest_memory = loaded.value().memory.accounting();
    const auto report = execution::render_execution_report_json(single_run.value());
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
        std::cout << "STOPPED: "
                  << execution::execution_stop_reason_name(single_run->stop_reason)
                  << " pc=" << std::hex << single_run->stop_pc << std::dec << '\n';
    }
    return static_cast<int>(ExitCode::Success);
}
