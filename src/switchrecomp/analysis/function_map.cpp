#include "switchrecomp/analysis/function_map.hpp"

#include "switchrecomp/common/checked_arithmetic.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>

namespace switchrecomp::analysis
{

std::string_view analysis_strategy_name(AnalysisStrategy strategy) noexcept
{
    switch (strategy)
    {
    case AnalysisStrategy::WholeModule: return "whole_module";
    case AnalysisStrategy::ExecutionClosure: return "execution_closure";
    }
    return "unknown";
}

std::string_view analysis_budget_dimension_name(AnalysisBudgetDimension dimension) noexcept
{
    switch (dimension)
    {
    case AnalysisBudgetDimension::None: return "none";
    case AnalysisBudgetDimension::Functions: return "functions";
    case AnalysisBudgetDimension::Instructions: return "instructions";
    case AnalysisBudgetDimension::Blocks: return "blocks";
    case AnalysisBudgetDimension::Edges: return "edges";
    case AnalysisBudgetDimension::Seeds: return "seeds";
    case AnalysisBudgetDimension::BytesAnalyzed: return "bytes_analyzed";
    case AnalysisBudgetDimension::BoundaryFinalizationPasses:
        return "boundary_finalization_passes";
    }
    return "unknown";
}

std::string_view analysis_budget_provenance_kind_name(
    AnalysisBudgetProvenanceKind kind) noexcept
{
    switch (kind)
    {
    case AnalysisBudgetProvenanceKind::LibraryDefault: return "library_default";
    case AnalysisBudgetProvenanceKind::ExecutionToolProfile: return "execution_tool_profile";
    case AnalysisBudgetProvenanceKind::ExplicitCliOverride: return "explicit_cli_override";
    case AnalysisBudgetProvenanceKind::LocalConfigurationOverride:
        return "local_configuration_override";
    case AnalysisBudgetProvenanceKind::DerivedStructuralBound:
        return "derived_structural_bound";
    }
    return "unknown";
}

AnalysisBudgets make_execution_closure_analysis_budgets()
{
    AnalysisBudgets budgets;
    budgets.max_functions = 5'000U;
    budgets.max_instructions = 200'000U;
    budgets.max_blocks = 50'000U;
    budgets.max_edges = 100'000U;
    budgets.max_seeds = 10'000U;
    budgets.max_bytes_analyzed = memory::GuestSize{16U} * 1024U * 1024U;
    budgets.max_boundary_finalization_passes = 8U;
    budgets.strategy = AnalysisStrategy::ExecutionClosure;
    mark_analysis_budget_override(
        budgets, AnalysisBudgetDimension::Functions,
        AnalysisBudgetProvenanceKind::ExecutionToolProfile, "run_entry_execution_closure");
    mark_analysis_budget_override(
        budgets, AnalysisBudgetDimension::Instructions,
        AnalysisBudgetProvenanceKind::ExecutionToolProfile, "run_entry_execution_closure");
    mark_analysis_budget_override(
        budgets, AnalysisBudgetDimension::Blocks,
        AnalysisBudgetProvenanceKind::ExecutionToolProfile, "run_entry_execution_closure");
    mark_analysis_budget_override(
        budgets, AnalysisBudgetDimension::Edges,
        AnalysisBudgetProvenanceKind::ExecutionToolProfile, "run_entry_execution_closure");
    mark_analysis_budget_override(
        budgets, AnalysisBudgetDimension::Seeds,
        AnalysisBudgetProvenanceKind::ExecutionToolProfile, "run_entry_execution_closure");
    mark_analysis_budget_override(
        budgets, AnalysisBudgetDimension::BytesAnalyzed,
        AnalysisBudgetProvenanceKind::ExecutionToolProfile, "run_entry_execution_closure");
    mark_analysis_budget_override(
        budgets, AnalysisBudgetDimension::BoundaryFinalizationPasses,
        AnalysisBudgetProvenanceKind::ExecutionToolProfile, "run_entry_execution_closure");
    return budgets;
}

void mark_analysis_budget_override(AnalysisBudgets& budgets,
                                   AnalysisBudgetDimension dimension,
                                   AnalysisBudgetProvenanceKind kind,
                                   std::string detail)
{
    AnalysisBudgetProvenance* provenance = nullptr;
    switch (dimension)
    {
    case AnalysisBudgetDimension::Functions: provenance = &budgets.provenance.max_functions; break;
    case AnalysisBudgetDimension::Instructions:
        provenance = &budgets.provenance.max_instructions;
        break;
    case AnalysisBudgetDimension::Blocks: provenance = &budgets.provenance.max_blocks; break;
    case AnalysisBudgetDimension::Edges: provenance = &budgets.provenance.max_edges; break;
    case AnalysisBudgetDimension::Seeds: provenance = &budgets.provenance.max_seeds; break;
    case AnalysisBudgetDimension::BytesAnalyzed:
        provenance = &budgets.provenance.max_bytes_analyzed;
        break;
    case AnalysisBudgetDimension::BoundaryFinalizationPasses:
        provenance = &budgets.provenance.max_boundary_finalization_passes;
        break;
    case AnalysisBudgetDimension::None: break;
    }
    if (provenance != nullptr)
    {
        provenance->kind = kind;
        provenance->detail = std::move(detail);
    }
}

std::string render_analysis_accounting_json(const AnalysisAccounting& accounting)
{
    using json = nlohmann::json;
    const auto address_json = [](memory::GuestAddress address) {
        std::ostringstream output;
        output << "0x" << std::hex << std::setw(16) << std::setfill('0') << address;
        return output.str();
    };
    const auto provenance_json = [](const AnalysisBudgetProvenance& provenance) {
        return json{{"kind", analysis_budget_provenance_kind_name(provenance.kind)},
                    {"detail", provenance.detail}};
    };
    const auto budgets_json = [&]() {
        return json{{"max_functions", accounting.budgets.max_functions},
                    {"max_instructions", accounting.budgets.max_instructions},
                    {"max_blocks", accounting.budgets.max_blocks},
                    {"max_edges", accounting.budgets.max_edges},
                    {"max_seeds", accounting.budgets.max_seeds},
                    {"max_bytes_analyzed", accounting.budgets.max_bytes_analyzed},
                    {"max_boundary_finalization_passes",
                     accounting.budgets.max_boundary_finalization_passes}};
    };
    const auto provenance = json{
        {"max_functions", provenance_json(accounting.budgets.provenance.max_functions)},
        {"max_instructions", provenance_json(accounting.budgets.provenance.max_instructions)},
        {"max_blocks", provenance_json(accounting.budgets.provenance.max_blocks)},
        {"max_edges", provenance_json(accounting.budgets.provenance.max_edges)},
        {"max_seeds", provenance_json(accounting.budgets.provenance.max_seeds)},
        {"max_bytes_analyzed", provenance_json(accounting.budgets.provenance.max_bytes_analyzed)},
        {"max_boundary_finalization_passes",
         provenance_json(accounting.budgets.provenance.max_boundary_finalization_passes)}};

    auto sources = accounting.seed_sources;
    std::sort(sources.begin(), sources.end(), [](const auto& left, const auto& right) {
        return static_cast<unsigned>(left.source) < static_cast<unsigned>(right.source);
    });
    json source_values = json::array();
    for (const auto& source : sources)
    {
        source_values.push_back(json{{"source", function_discovery_source_name(source.source)},
                                     {"observed", source.observed},
                                     {"included", source.included},
                                     {"excluded", source.excluded},
                                     {"new_canonical_entries", source.new_canonical_entries},
                                     {"coalesced", source.coalesced}});
    }

    json exhaustion = nullptr;
    if (accounting.exhaustion)
    {
        const auto& value = accounting.exhaustion.value();
        exhaustion = json{{"dimension", analysis_budget_dimension_name(value.dimension)},
                          {"consumed", value.consumed},
                          {"limit", value.limit},
                          {"module", value.module},
                          {"phase", value.phase},
                          {"pending_work", value.pending_work},
                          {"next_work", value.next_work
                                           ? json(address_json(value.next_work.value()))
                                           : json(nullptr)}};
    }

    return json{{"module", accounting.module},
                {"strategy", analysis_strategy_name(accounting.strategy)},
                {"effective_budgets", budgets_json()},
                {"budget_provenance", provenance},
                {"seeds", json{{"initial", accounting.initial_seed_count},
                                {"normalized_unique", accounting.normalized_unique_seed_count},
                                {"duplicate_coalesced", accounting.duplicate_coalesced_seed_count},
                                {"excluded_candidate", accounting.excluded_candidate_seed_count},
                                {"by_source", std::move(source_values)}}},
                {"functions", json{{"discovered_canonical", accounting.discovered_canonical_functions},
                                    {"candidate_entries", accounting.candidate_function_entries},
                                    {"trusted_entries", accounting.trusted_function_entries},
                                    {"cfg_analyzed", accounting.functions_cfg_analyzed},
                                    {"newly_analyzed", accounting.newly_analyzed_functions},
                                    {"reanalyzed", accounting.reanalyzed_functions},
                                    {"reused", accounting.reused_functions},
                                    {"invalidated_records", accounting.invalidated_records},
                                    {"with_cfg", accounting.canonical_functions_with_cfg},
                                    {"failed", accounting.failed_functions}}},
                {"discovery", json{{"direct_call_discoveries", accounting.direct_call_discoveries},
                                    {"new_seeds_generated", accounting.new_seeds_generated}}},
                {"consumption", json{{"blocks", accounting.blocks_consumed},
                                      {"instructions", accounting.instructions_consumed},
                                      {"edges", accounting.edges_consumed},
                                      {"bytes_analyzed", accounting.bytes_analyzed}}},
                {"boundary_finalization_passes", accounting.boundary_finalization_passes},
                {"refinement_transactions", accounting.refinement_transactions},
                {"function_boundary_conflicts", accounting.function_boundary_conflicts},
                {"work_remaining_at_exhaustion", accounting.work_remaining_at_exhaustion},
                {"exhaustion", std::move(exhaustion)},
                {"phases", json{{"initial_seeding", accounting.phases.initial_seeding},
                                 {"cfg_discovery", accounting.phases.cfg_discovery},
                                 {"direct_call_expansion", accounting.phases.direct_call_expansion},
                                 {"ownership_normalization", accounting.phases.ownership_normalization},
                                 {"conflict_processing", accounting.phases.conflict_processing},
                                 {"boundary_finalization", accounting.phases.boundary_finalization},
                                 {"immutable_map_reconstruction",
                                  accounting.phases.immutable_map_reconstruction}}}}
        .dump();
}

std::string_view module_base_provenance_name(ModuleBaseProvenance provenance) noexcept
{
    switch (provenance)
    {
    case ModuleBaseProvenance::ExplicitAnalysisBase: return "explicit_analysis_base";
    case ModuleBaseProvenance::DeterministicAnalysisLayout:
        return "deterministic_analysis_layout";
    case ModuleBaseProvenance::ExternallyObserved: return "externally_observed";
    case ModuleBaseProvenance::RuntimeVerified: return "runtime_verified";
    }
    return "unknown";
}

namespace
{

using GuestAddress = memory::GuestAddress;

[[nodiscard]] std::string hex_address(GuestAddress address)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::setw(16) << std::setfill('0') << address;
    return output.str();
}

[[nodiscard]] std::string synthetic_id(GuestAddress address)
{
    std::ostringstream output;
    output << "sub_" << std::hex << std::setw(16) << std::setfill('0') << address;
    return output.str();
}

[[nodiscard]] int confidence_rank(FunctionConfidence confidence) noexcept
{
    switch (confidence)
    {
    case FunctionConfidence::Conflict: return 6;
    case FunctionConfidence::Manual: return 5;
    case FunctionConfidence::Confirmed: return 4;
    case FunctionConfidence::High: return 3;
    case FunctionConfidence::Medium: return 2;
    case FunctionConfidence::Low: return 1;
    }
    return 0;
}

[[nodiscard]] bool contains(const GuestAddressRange& range, GuestAddress address,
                            memory::GuestSize size = 1U)
{
    const auto end = checked_add_u64(range.base, range.size);
    const auto requested_end = checked_add_u64(address, size);
    return end && requested_end && address >= range.base && requested_end.value() <= end.value();
}

[[nodiscard]] bool contains_any(const std::vector<GuestAddressRange>& ranges,
                                GuestAddress address, memory::GuestSize size = 1U)
{
    return std::any_of(ranges.begin(), ranges.end(), [address, size](const auto& range) {
        return contains(range, address, size);
    });
}

[[nodiscard]] FailureCategory category_for(ErrorCode code) noexcept
{
    switch (code)
    {
    case ErrorCode::DecodeFailed:
    case ErrorCode::InstructionFetchFailed: return FailureCategory::DecodeFailure;
    case ErrorCode::UnsupportedInstruction:
    case ErrorCode::UnsupportedOperandForm:
    case ErrorCode::Unsupported: return FailureCategory::UnsupportedInstruction;
    case ErrorCode::InvalidBranchTarget:
    case ErrorCode::MisalignedInstructionAddress:
    case ErrorCode::NonExecutableAddress:
    case ErrorCode::InvalidGuestAddress: return FailureCategory::InvalidGuestAddress;
    case ErrorCode::AnalysisBudgetExceeded:
    case ErrorCode::AnalysisInstructionLimitExceeded:
    case ErrorCode::AnalysisBlockLimitExceeded:
    case ErrorCode::AnalysisWorklistLimitExceeded: return FailureCategory::AnalysisBudgetExceeded;
    case ErrorCode::InvalidControlFlow:
    case ErrorCode::AnalysisScopeViolation: return FailureCategory::InvalidCFG;
    case ErrorCode::FunctionBoundaryConflict: return FailureCategory::FunctionBoundaryConflict;
    case ErrorCode::IrVerificationFailed: return FailureCategory::IRVerificationFailure;
    case ErrorCode::MissingImportBinding: return FailureCategory::MissingImportBinding;
    case ErrorCode::RuntimeBoundary: return FailureCategory::RuntimeBoundary;
    case ErrorCode::CodegenFailure:
    case ErrorCode::LlvmVerificationFailed:
    case ErrorCode::JitCompilationFailed: return FailureCategory::CodegenFailure;
    default: return FailureCategory::InvalidCFG;
    }
}

[[nodiscard]] Error budget_failure(AnalysisAccounting& accounting,
                                   AnalysisBudgetDimension dimension,
                                   std::size_t consumed, std::size_t limit,
                                   std::string phase, std::size_t pending_work,
                                   std::optional<GuestAddress> next_work,
                                   std::string message)
{
    accounting.exhaustion = AnalysisBudgetExhaustion{dimension, consumed, limit,
                                                     accounting.module, phase, pending_work,
                                                     next_work};
    accounting.work_remaining_at_exhaustion = pending_work;
    Error error = make_error(ErrorCode::AnalysisBudgetExceeded, std::move(message));
    error.budget_context = Error::BudgetContext{
        "function_map_analysis", std::string(analysis_budget_dimension_name(dimension)), consumed,
        limit, accounting.module, std::move(phase), pending_work,
        next_work ? std::optional<std::uint64_t>(next_work.value()) : std::nullopt};
    error.message += " (" + std::string(analysis_budget_dimension_name(dimension)) + " " +
                     std::to_string(consumed) + "/" + std::to_string(limit) + ", module " +
                     accounting.module + ")";
    return error;
}

[[nodiscard]] std::optional<AnalysisBudgetDimension> cfg_budget_dimension(ErrorCode code) noexcept
{
    switch (code)
    {
    case ErrorCode::AnalysisInstructionLimitExceeded:
        return AnalysisBudgetDimension::Instructions;
    case ErrorCode::AnalysisBlockLimitExceeded:
        return AnalysisBudgetDimension::Blocks;
    case ErrorCode::AnalysisWorklistLimitExceeded:
        return AnalysisBudgetDimension::Seeds;
    default: return std::nullopt;
    }
}

[[nodiscard]] bool checked_size_add(std::size_t left, std::size_t right,
                                    std::size_t& result) noexcept
{
    if (right > std::numeric_limits<std::size_t>::max() - left) return false;
    result = left + right;
    return true;
}

[[nodiscard]] Result<AnalysisBudgets> derive_structural_budgets(
    const ModuleAnalysisInput& input, const AnalysisBudgets& requested,
    AnalysisAccounting& accounting)
{
    memory::GuestSize executable_bytes = 0U;
    for (const auto& range : input.identity.executable_ranges)
    {
        if (range.size > std::numeric_limits<memory::GuestSize>::max() - executable_bytes)
        {
            return Result<AnalysisBudgets>::failure(make_error(
                ErrorCode::AnalysisBudgetExceeded,
                "executable range byte capacity overflows the guest size type"));
        }
        executable_bytes += range.size;
    }
    const auto instruction_capacity_guest = executable_bytes / 4U;
    const auto instruction_capacity = instruction_capacity_guest >
                                              static_cast<memory::GuestSize>(
                                                  std::numeric_limits<std::size_t>::max())
                                          ? std::numeric_limits<std::size_t>::max()
                                          : static_cast<std::size_t>(instruction_capacity_guest);
    if (instruction_capacity == 0U)
    {
        return Result<AnalysisBudgets>::failure(make_error(
            ErrorCode::AnalysisBudgetExceeded,
            "executable ranges contain no finite AArch64 instruction capacity"));
    }

    AnalysisBudgets effective = requested;
    const auto clamp = [&](AnalysisBudgetDimension dimension, std::size_t& value,
                           std::size_t ceiling, std::string detail) {
        if (value > ceiling)
        {
            value = ceiling;
            mark_analysis_budget_override(effective, dimension,
                                          AnalysisBudgetProvenanceKind::DerivedStructuralBound,
                                          std::move(detail));
        }
    };
    clamp(AnalysisBudgetDimension::Functions, effective.max_functions, instruction_capacity,
          "executable_instruction_capacity");
    clamp(AnalysisBudgetDimension::Seeds, effective.max_seeds, instruction_capacity,
          "executable_instruction_capacity");
    const auto analysis_passes = effective.max_boundary_finalization_passes ==
                                         std::numeric_limits<std::size_t>::max()
                                     ? std::numeric_limits<std::size_t>::max()
                                     : effective.max_boundary_finalization_passes + 1U;
    const auto repeated_capacity = instruction_capacity >
                                           std::numeric_limits<std::size_t>::max() /
                                               analysis_passes
                                       ? std::numeric_limits<std::size_t>::max()
                                       : instruction_capacity * analysis_passes;
    const auto edge_capacity = repeated_capacity > std::numeric_limits<std::size_t>::max() / 2U
                                   ? std::numeric_limits<std::size_t>::max()
                                   : repeated_capacity * 2U;
    clamp(AnalysisBudgetDimension::Instructions, effective.max_instructions, repeated_capacity,
          "executable_instruction_capacity_times_analysis_passes");
    clamp(AnalysisBudgetDimension::Blocks, effective.max_blocks, repeated_capacity,
          "executable_instruction_capacity_times_analysis_passes");
    clamp(AnalysisBudgetDimension::Edges, effective.max_edges, edge_capacity,
          "two_cfg_edges_per_instruction_capacity");
    const auto repeated_bytes = executable_bytes >
                                       std::numeric_limits<memory::GuestSize>::max() /
                                           static_cast<memory::GuestSize>(analysis_passes)
                                   ? std::numeric_limits<memory::GuestSize>::max()
                                   : executable_bytes * static_cast<memory::GuestSize>(analysis_passes);
    if (effective.max_bytes_analyzed > repeated_bytes)
    {
        effective.max_bytes_analyzed = repeated_bytes;
        mark_analysis_budget_override(effective, AnalysisBudgetDimension::BytesAnalyzed,
                                      AnalysisBudgetProvenanceKind::DerivedStructuralBound,
                                      "executable_range_byte_capacity_times_analysis_passes");
    }
    accounting.budgets = effective;
    accounting.strategy = effective.strategy;
    return Result<AnalysisBudgets>::success(std::move(effective));
}

[[nodiscard]] AnalysisSeedSourceAccounting& seed_source_accounting(
    std::vector<AnalysisSeedSourceAccounting>& sources, FunctionDiscoverySource source)
{
    const auto found = std::find_if(sources.begin(), sources.end(), [source](const auto& item) {
        return item.source == source;
    });
    if (found != sources.end()) return *found;
    sources.push_back(AnalysisSeedSourceAccounting{source});
    return sources.back();
}

[[nodiscard]] Result<void> validate_input(const ModuleAnalysisInput& input,
                                           const FunctionMapOptions& options)
{
    if (input.memory == nullptr)
    {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidArgument, "function discovery requires guest memory"));
    }
    if (input.identity.module.empty())
    {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidArgument, "function discovery requires a logical module name"));
    }
    if (input.identity.executable_ranges.empty())
    {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidArgument, "function discovery requires executable ranges"));
    }
    const auto& budgets = options.budgets;
    if (budgets.max_functions == 0U || budgets.max_blocks == 0U || budgets.max_instructions == 0U ||
        budgets.max_edges == 0U || budgets.max_seeds == 0U || budgets.max_bytes_analyzed == 0U ||
        budgets.max_boundary_finalization_passes == 0U)
    {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidArgument, "function discovery budgets must be non-zero"));
    }

    GuestAddress previous_end = 0U;
    bool first = true;
    for (const auto& range : input.identity.executable_ranges)
    {
        if (range.size == 0U || (range.base & 0x3U) != 0U || (range.size & 0x3U) != 0U)
        {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidGuestAddress,
                "executable range must be non-empty and AArch64 aligned"));
        }
        const auto end = checked_add_u64(range.base, range.size);
        if (!end)
        {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidGuestAddress, "executable range overflows guest address space"));
        }
        if (!first && range.base < previous_end)
        {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidFormat, "executable ranges overlap or are not sorted"));
        }
        const auto executable = input.memory->is_executable(range.base, range.size);
        if (!executable)
        {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidGuestAddress,
                "executable range " + hex_address(range.base) + " cannot be validated: " +
                    executable.error().message));
        }
        if (!executable.value())
        {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidGuestAddress,
                "executable range " + hex_address(range.base) + " is not executable"));
        }
        previous_end = end.value();
        first = false;
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> validate_seed(const ModuleAnalysisInput& input,
                                          const FunctionSeed& seed)
{
    const auto canonical = seed.canonical_entry.value_or(seed.entry);
    for (const auto address : {seed.entry, canonical})
    {
        if ((address & 0x3U) != 0U)
        {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidGuestAddress,
                "function seed " + hex_address(address) + " is not AArch64 aligned"));
        }
        if (!contains_any(input.identity.executable_ranges, address, 4U))
        {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidGuestAddress,
                "function seed " + hex_address(address) + " is outside executable ranges"));
        }
        const auto executable = input.memory->is_executable(address, 4U);
        if (!executable || !executable.value())
        {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidGuestAddress,
                "function seed " + hex_address(address) + " is not executable"));
        }
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> add_evidence(FunctionRecord& record, const FunctionSeed& seed)
{
    if (std::find(record.entries.begin(), record.entries.end(), seed.entry) == record.entries.end())
    {
        record.entries.push_back(seed.entry);
    }
    const DiscoveryEvidence candidate{seed.source, seed.confidence, seed.entry, seed.note};
    const auto duplicate = std::find_if(record.evidence.begin(), record.evidence.end(),
                                        [&candidate](const auto& existing) {
                                            return existing.source == candidate.source &&
                                                   existing.confidence == candidate.confidence &&
                                                   existing.entry == candidate.entry &&
                                                   existing.note == candidate.note;
                                        });
    if (duplicate == record.evidence.end())
        record.evidence.push_back(candidate);
    if (seed.name)
    {
        if (!record.name || record.name->empty() || confidence_rank(seed.confidence) >
                                                    confidence_rank(record.confidence))
        {
            record.name = seed.name;
        }
    }
    if (confidence_rank(seed.confidence) > confidence_rank(record.confidence))
    {
        record.confidence = seed.confidence;
        record.primary_source = seed.source;
    }
    if (is_boundary_worthy_function_seed(seed))
    {
        record.entry_trust_status = FunctionEntryTrustStatus::Trusted;
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> add_seed(std::map<GuestAddress, FunctionRecord>& records,
                                    std::set<GuestAddress>& pending,
                                    const ModuleAnalysisInput& input, const FunctionSeed& seed,
                                    const AnalysisBudgets& budgets, std::size_t& seed_count,
                                    AnalysisAccounting& accounting, std::string_view phase)
{
    const auto valid = validate_seed(input, seed);
    if (!valid)
    {
        return valid;
    }
    const auto canonical = seed.canonical_entry.value_or(seed.entry);
    auto found = records.find(canonical);
    if (found == records.end())
    {
        if (records.size() >= budgets.max_functions)
        {
            return Result<void>::failure(budget_failure(
                accounting, AnalysisBudgetDimension::Functions, records.size(),
                budgets.max_functions, std::string(phase), pending.size(),
                pending.empty() ? std::nullopt
                                : std::optional<GuestAddress>(*pending.begin()),
                "function discovery exceeded the function budget"));
        }
        FunctionRecord record;
        record.module = input.identity.module;
        record.synthetic_id = synthetic_id(canonical);
        record.canonical_entry = canonical;
        record.primary_source = seed.source;
        record.confidence = seed.confidence;
        record.entry_trust_status = is_boundary_worthy_function_seed(seed)
                                        ? FunctionEntryTrustStatus::Trusted
                                        : FunctionEntryTrustStatus::Candidate;
        record.entries.push_back(canonical);
        if (seed.entry != canonical) record.entries.push_back(seed.entry);
        record.translation_status = TranslationStatus::Discovered;
        found = records.emplace(canonical, std::move(record)).first;
    }
    const auto evidence = add_evidence(found->second, seed);
    if (!evidence)
    {
        return evidence;
    }
    if (pending.insert(canonical).second)
    {
        if (seed_count >= budgets.max_seeds)
        {
            return Result<void>::failure(budget_failure(
                accounting, AnalysisBudgetDimension::Seeds, seed_count, budgets.max_seeds,
                std::string(phase), pending.size(),
                pending.empty() ? std::nullopt
                                : std::optional<GuestAddress>(*pending.begin()),
                "function discovery exceeded the seed budget"));
        }
        ++seed_count;
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> populate_ownership(
    FunctionRecord& record, const std::vector<GuestAddressRange>& executable_ranges)
{
    if (!record.cfg || record.cfg->blocks.empty())
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidControlFlow, "cannot derive a function range from an empty CFG"));
    }
    std::vector<GuestAddress> instruction_addresses;
    instruction_addresses.reserve(record.cfg->instruction_count);
    for (const auto& [address, block] : record.cfg->blocks)
    {
        (void)address;
        for (const auto& instruction : block.instructions)
        {
            instruction_addresses.push_back(instruction.address);
        }
    }
    const auto normalized = normalize_code_ranges(instruction_addresses);
    if (!normalized)
    {
        return Result<void>::failure(normalized.error());
    }
    if (normalized.value().empty())
    {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidControlFlow, "function CFG has no valid instruction range"));
    }
    for (const auto& range : normalized.value())
    {
        if (!contains_any(executable_ranges, range.base, range.size))
        {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidGuestAddress,
                "function ownership range " + hex_address(range.base) +
                    " is outside executable memory"));
        }
    }
    record.owned_code_ranges = normalized.value();
    record.range_begin = record.owned_code_ranges.front().base;
    const auto envelope_end = checked_add_u64(record.owned_code_ranges.back().base,
                                               record.owned_code_ranges.back().size);
    if (!envelope_end)
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidGuestAddress, "function ownership envelope overflows"));
    }
    record.range_end = envelope_end.value();
    record.direct_calls.clear();
    record.indirect_calls.clear();
    for (const auto& call : record.cfg->calls)
    {
        if (call.kind == CallKind::Direct && call.target)
        {
            record.direct_calls.push_back(call.target.value());
        }
        else if (call.kind == CallKind::Indirect)
        {
            record.indirect_calls.push_back(call);
        }
    }
    record.unresolved_control_flow = record.cfg->unresolved;
    std::sort(record.direct_calls.begin(), record.direct_calls.end());
    record.direct_calls.erase(std::unique(record.direct_calls.begin(), record.direct_calls.end()),
                              record.direct_calls.end());
    std::sort(record.indirect_calls.begin(), record.indirect_calls.end(),
              [](const CallSite& left, const CallSite& right) {
                  if (left.address != right.address) return left.address < right.address;
                  if (left.target.has_value() != right.target.has_value())
                  {
                      return left.target.has_value() < right.target.has_value();
                  }
                  if (left.target && right.target && left.target.value() != right.target.value())
                  {
                      return left.target.value() < right.target.value();
                  }
                  const auto left_register = left.register_target
                                                 ? static_cast<unsigned int>(left.register_target->index)
                                                 : 256U;
                  const auto right_register = right.register_target
                                                  ? static_cast<unsigned int>(right.register_target->index)
                                                  : 256U;
                  return left_register < right_register;
              });
    return Result<void>::success();
}

void populate_boundary_dependencies(FunctionRecord& record,
                                    const std::set<GuestAddress>& known_function_entries)
{
    record.boundary_dependencies.clear();
    for (const auto entry : known_function_entries)
    {
        if (contains_any(record.owned_code_ranges, entry, 4U))
            record.boundary_dependencies.push_back(entry);
    }
}

[[nodiscard]] Result<FunctionBoundaryConflict> make_conflict(const FunctionRecord& first,
                                                             const FunctionRecord& second,
                                                             std::string module)
{
    const FunctionRecord* normalized_first = &first;
    const FunctionRecord* normalized_second = &second;
    if (normalized_second->canonical_entry < normalized_first->canonical_entry)
    {
        std::swap(normalized_first, normalized_second);
    }
    const auto overlap = intersect_owned_ranges(normalized_first->owned_code_ranges,
                                                 normalized_second->owned_code_ranges);
    if (!overlap)
    {
        return Result<FunctionBoundaryConflict>::failure(overlap.error());
    }
    if (overlap.value().empty())
    {
        return Result<FunctionBoundaryConflict>::failure(make_error(
            ErrorCode::FunctionBoundaryConflict,
            "cannot create a function conflict without precise instruction overlap"));
    }
    return Result<FunctionBoundaryConflict>::success(FunctionBoundaryConflict{
        std::move(module),
        normalized_first->canonical_entry,
        normalized_second->canonical_entry,
        GuestAddressRange{normalized_first->range_begin,
                          normalized_first->range_end - normalized_first->range_begin},
        GuestAddressRange{normalized_second->range_begin,
                          normalized_second->range_end - normalized_second->range_begin},
        overlap.value(),
        normalized_first->primary_source,
        normalized_second->primary_source,
        normalized_first->confidence,
        normalized_second->confidence,
        "unresolved; manual review required"});
}

[[nodiscard]] bool ranges_overlap(const FunctionRecord& left,
                                  const FunctionRecord& right) noexcept
{
    return owned_ranges_overlap(left.owned_code_ranges, right.owned_code_ranges);
}

} // namespace

Result<std::vector<GuestAddressRange>> normalize_code_ranges(
    std::span<const GuestAddress> instruction_addresses)
{
    std::vector<GuestAddress> addresses(instruction_addresses.begin(), instruction_addresses.end());
    std::sort(addresses.begin(), addresses.end());
    addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());

    std::vector<GuestAddressRange> ranges;
    ranges.reserve(addresses.size());
    for (const auto address : addresses)
    {
        if ((address & 0x3U) != 0U)
        {
            return Result<std::vector<GuestAddressRange>>::failure(make_error(
                ErrorCode::InvalidGuestAddress,
                "owned instruction address " + hex_address(address) +
                    " is not AArch64 instruction aligned"));
        }
        const auto end = checked_add_u64(address, 4U);
        if (!end)
        {
            return Result<std::vector<GuestAddressRange>>::failure(make_error(
                ErrorCode::InvalidGuestAddress,
                "owned instruction address " + hex_address(address) + " overflows"));
        }
        if (ranges.empty())
        {
            ranges.push_back(GuestAddressRange{address, 4U});
            continue;
        }

        auto& previous = ranges.back();
        const auto previous_end = checked_add_u64(previous.base, previous.size);
        if (!previous_end)
        {
            return Result<std::vector<GuestAddressRange>>::failure(make_error(
                ErrorCode::InvalidGuestAddress, "owned code range overflows"));
        }
        if (address <= previous_end.value())
        {
            if (end.value() > previous_end.value())
            {
                previous.size = end.value() - previous.base;
            }
        }
        else
        {
            ranges.push_back(GuestAddressRange{address, 4U});
        }
    }
    return Result<std::vector<GuestAddressRange>>::success(std::move(ranges));
}

Result<std::vector<GuestAddressRange>> normalize_code_ranges(
    const std::vector<GuestAddress>& instruction_addresses)
{
    return normalize_code_ranges(
        std::span<const GuestAddress>(instruction_addresses.data(), instruction_addresses.size()));
}

Result<std::vector<GuestAddressRange>> normalize_code_ranges(
    std::span<const GuestAddressRange> input_ranges)
{
    std::vector<GuestAddressRange> ranges(input_ranges.begin(), input_ranges.end());
    std::sort(ranges.begin(), ranges.end(), [](const GuestAddressRange& left,
                                               const GuestAddressRange& right) {
        if (left.base != right.base) return left.base < right.base;
        return left.size < right.size;
    });

    std::vector<GuestAddressRange> normalized;
    normalized.reserve(ranges.size());
    for (const auto& range : ranges)
    {
        if (range.size == 0U || (range.base & 0x3U) != 0U || (range.size & 0x3U) != 0U)
        {
            return Result<std::vector<GuestAddressRange>>::failure(make_error(
                ErrorCode::InvalidGuestAddress,
                "code range must be non-empty and AArch64 aligned"));
        }
        const auto end = checked_add_u64(range.base, range.size);
        if (!end)
        {
            return Result<std::vector<GuestAddressRange>>::failure(make_error(
                ErrorCode::InvalidGuestAddress, "code range overflows the guest address space"));
        }
        if (normalized.empty())
        {
            normalized.push_back(range);
            continue;
        }
        auto& previous = normalized.back();
        const auto previous_end = checked_add_u64(previous.base, previous.size);
        if (!previous_end)
        {
            return Result<std::vector<GuestAddressRange>>::failure(make_error(
                ErrorCode::InvalidGuestAddress, "normalized code range overflows"));
        }
        if (range.base <= previous_end.value())
        {
            if (end.value() > previous_end.value())
            {
                previous.size = end.value() - previous.base;
            }
        }
        else
        {
            normalized.push_back(range);
        }
    }
    return Result<std::vector<GuestAddressRange>>::success(std::move(normalized));
}

Result<std::vector<GuestAddressRange>> normalize_code_ranges(
    const std::vector<GuestAddressRange>& input_ranges)
{
    return normalize_code_ranges(
        std::span<const GuestAddressRange>(input_ranges.data(), input_ranges.size()));
}

bool owned_ranges_overlap(const std::vector<GuestAddressRange>& left,
                          const std::vector<GuestAddressRange>& right) noexcept
{
    std::size_t left_index = 0U;
    std::size_t right_index = 0U;
    while (left_index < left.size() && right_index < right.size())
    {
        const auto left_end = checked_add_u64(left[left_index].base, left[left_index].size);
        const auto right_end = checked_add_u64(right[right_index].base, right[right_index].size);
        if (!left_end || !right_end)
        {
            return false;
        }
        if (left[left_index].base < right_end.value() &&
            right[right_index].base < left_end.value())
        {
            return true;
        }
        if (left_end.value() <= right_end.value())
        {
            ++left_index;
        }
        else
        {
            ++right_index;
        }
    }
    return false;
}

Result<std::vector<GuestAddressRange>> intersect_owned_ranges(
    const std::vector<GuestAddressRange>& left, const std::vector<GuestAddressRange>& right)
{
    const auto normalized_left = normalize_code_ranges(left);
    if (!normalized_left)
    {
        return Result<std::vector<GuestAddressRange>>::failure(normalized_left.error());
    }
    const auto normalized_right = normalize_code_ranges(right);
    if (!normalized_right)
    {
        return Result<std::vector<GuestAddressRange>>::failure(normalized_right.error());
    }

    std::vector<GuestAddressRange> intersections;
    std::size_t left_index = 0U;
    std::size_t right_index = 0U;
    while (left_index < normalized_left.value().size() &&
           right_index < normalized_right.value().size())
    {
        const auto& left_range = normalized_left.value()[left_index];
        const auto& right_range = normalized_right.value()[right_index];
        const auto left_end = checked_add_u64(left_range.base, left_range.size);
        const auto right_end = checked_add_u64(right_range.base, right_range.size);
        if (!left_end || !right_end)
        {
            return Result<std::vector<GuestAddressRange>>::failure(make_error(
                ErrorCode::InvalidGuestAddress, "owned range intersection overflows"));
        }
        const auto begin = std::max(left_range.base, right_range.base);
        const auto end = std::min(left_end.value(), right_end.value());
        if (begin < end)
        {
            intersections.push_back(GuestAddressRange{begin, end - begin});
        }
        if (left_end.value() <= right_end.value())
        {
            ++left_index;
        }
        else
        {
            ++right_index;
        }
    }
    return Result<std::vector<GuestAddressRange>>::success(std::move(intersections));
}

Result<memory::GuestSize> precise_owned_byte_count(
    const std::vector<GuestAddressRange>& ranges)
{
    memory::GuestSize total = 0U;
    const auto normalized = normalize_code_ranges(ranges);
    if (!normalized)
    {
        return Result<memory::GuestSize>::failure(normalized.error());
    }
    for (const auto& range : normalized.value())
    {
        if (range.size > std::numeric_limits<memory::GuestSize>::max() - total)
        {
            return Result<memory::GuestSize>::failure(make_error(
                ErrorCode::InvalidGuestAddress, "precise owned-byte count overflows"));
        }
        total += range.size;
    }
    return Result<memory::GuestSize>::success(total);
}

bool function_owns_address(const FunctionRecord& function, GuestAddress address) noexcept
{
    for (const auto& range : function.owned_code_ranges)
    {
        const auto end = checked_add_u64(range.base, range.size);
        if (end && address >= range.base && address < end.value() &&
            ((address - range.base) & 0x3U) == 0U)
        {
            return true;
        }
    }
    return false;
}

bool is_boundary_worthy_function_seed(const FunctionSeed& seed) noexcept
{
    if (seed.source == FunctionDiscoverySource::ObservedIndirectTarget)
    {
        return true;
    }
    if (seed.source == FunctionDiscoverySource::TextStartCandidate ||
        seed.source == FunctionDiscoverySource::Heuristic)
    {
        return false;
    }
    if (seed.source == FunctionDiscoverySource::ManualOverride ||
        seed.confidence == FunctionConfidence::Manual)
    {
        return true;
    }
    return seed.confidence == FunctionConfidence::Confirmed ||
           seed.confidence == FunctionConfidence::High;
}

std::string_view function_discovery_source_name(FunctionDiscoverySource source) noexcept
{
    switch (source)
    {
    case FunctionDiscoverySource::ModuleEntry: return "module_entry";
    case FunctionDiscoverySource::TextStartCandidate: return "text_start_candidate";
    case FunctionDiscoverySource::DynamicSymbol: return "dynamic_symbol";
    case FunctionDiscoverySource::Export: return "export";
    case FunctionDiscoverySource::DirectCall: return "direct_call";
    case FunctionDiscoverySource::RelocationReference: return "relocation_reference";
    case FunctionDiscoverySource::AnalystSeed: return "analyst_seed";
    case FunctionDiscoverySource::ManualOverride: return "manual_override";
    case FunctionDiscoverySource::JumpTable: return "jump_table";
    case FunctionDiscoverySource::ObservedIndirectTarget: return "observed_indirect_target";
    case FunctionDiscoverySource::Heuristic: return "heuristic";
    }
    return "unknown";
}

std::string_view entry_point_kind_name(EntryPointKind kind) noexcept
{
    switch (kind)
    {
    case EntryPointKind::TextStartCandidate: return "text_start_candidate";
    case EntryPointKind::DynamicInit: return "dynamic_init";
    case EntryPointKind::DynamicFini: return "dynamic_fini";
    case EntryPointKind::VerifiedProcessEntry: return "verified_process_entry";
    case EntryPointKind::AnalystOverride: return "analyst_override";
    }
    return "unknown";
}

std::string_view function_confidence_name(FunctionConfidence confidence) noexcept
{
    switch (confidence)
    {
    case FunctionConfidence::Confirmed: return "confirmed";
    case FunctionConfidence::High: return "high";
    case FunctionConfidence::Medium: return "medium";
    case FunctionConfidence::Low: return "low";
    case FunctionConfidence::Manual: return "manual";
    case FunctionConfidence::Conflict: return "conflict";
    }
    return "unknown";
}

std::string_view translation_status_name(TranslationStatus status) noexcept
{
    switch (status)
    {
    case TranslationStatus::Discovered: return "discovered";
    case TranslationStatus::Analyzed: return "analyzed";
    case TranslationStatus::Lifted: return "lifted";
    case TranslationStatus::Verified: return "verified";
    case TranslationStatus::Lowerable: return "lowerable";
    case TranslationStatus::Translated: return "translated";
    case TranslationStatus::Unsupported: return "unsupported";
    case TranslationStatus::Excluded: return "excluded";
    case TranslationStatus::Failed: return "failed";
    case TranslationStatus::Conflict: return "conflict";
    }
    return "unknown";
}

std::string_view function_entry_trust_status_name(FunctionEntryTrustStatus status) noexcept
{
    switch (status)
    {
    case FunctionEntryTrustStatus::Candidate: return "candidate";
    case FunctionEntryTrustStatus::Trusted: return "trusted";
    case FunctionEntryTrustStatus::Conflict: return "conflict";
    }
    return "candidate";
}

std::string_view failure_category_name(FailureCategory category) noexcept
{
    switch (category)
    {
    case FailureCategory::DecodeFailure: return "decode_failure";
    case FailureCategory::InvalidInstruction: return "invalid_instruction";
    case FailureCategory::InvalidCFG: return "invalid_cfg";
    case FailureCategory::UnsupportedInstruction: return "unsupported_instruction";
    case FailureCategory::UnsupportedSemantic: return "unsupported_semantic";
    case FailureCategory::UnresolvedIndirectFlow: return "unresolved_indirect_flow";
    case FailureCategory::FunctionBoundaryConflict: return "function_boundary_conflict";
    case FailureCategory::InvalidGuestAddress: return "invalid_guest_address";
    case FailureCategory::AnalysisBudgetExceeded: return "analysis_budget_exceeded";
    case FailureCategory::IRVerificationFailure: return "ir_verification_failure";
    case FailureCategory::MissingImportBinding: return "missing_import_binding";
    case FailureCategory::RuntimeBoundary: return "runtime_boundary";
    case FailureCategory::CodegenFailure: return "codegen_failure";
    }
    return "unknown";
}

const FunctionRecord* FinalizedFunctionMap::find_canonical_entry(GuestAddress entry) const noexcept
{
    const auto found = std::lower_bound(
        functions_.begin(), functions_.end(), entry,
        [](const FunctionRecord& function, GuestAddress value) {
            return function.canonical_entry < value;
        });
    return found != functions_.end() && found->canonical_entry == entry ? &*found : nullptr;
}

const FunctionRecord* FinalizedFunctionMap::find_exact_entry(GuestAddress entry) const noexcept
{
    if (const auto* canonical = find_canonical_entry(entry)) return canonical;
    for (const auto& function : functions_)
    {
        if (std::binary_search(function.entries.begin(), function.entries.end(), entry))
        {
            return &function;
        }
    }
    return nullptr;
}

const FunctionRecord* FinalizedFunctionMap::find_callable_entry(GuestAddress entry) const noexcept
{
    return find_exact_entry(entry);
}

std::vector<const FunctionRecord*> FinalizedFunctionMap::find_precise_owners(
    GuestAddress address) const
{
    std::vector<const FunctionRecord*> owners;
    for (const auto& function : functions_)
    {
        if (function_owns_address(function, address))
        {
            owners.push_back(&function);
        }
    }
    return owners;
}

const FunctionRecord* FinalizedFunctionMap::find(GuestAddress entry) const noexcept
{
    return find_callable_entry(entry);
}

std::vector<const FunctionRecord*> FinalizedFunctionMap::find_owners(
    GuestAddress address) const
{
    return find_precise_owners(address);
}

Result<FinalizedFunctionMap> FunctionMapBuilder::build(const ModuleAnalysisInput& input,
                                                        const FunctionMapOptions& options)
{
    const auto valid_input = validate_input(input, options);
    if (!valid_input)
    {
            return Result<FinalizedFunctionMap>::failure(valid_input.error());
    }

    AnalysisAccounting accounting;
    accounting.module = input.identity.module;
    accounting.strategy = options.budgets.strategy;
    accounting.initial_seed_count = input.seeds.size();
    accounting.phases.initial_seeding = input.seeds.size();
    const auto effective_budget_result = derive_structural_budgets(
        input, options.budgets, accounting);
    if (!effective_budget_result)
    {
        return Result<FinalizedFunctionMap>::failure(effective_budget_result.error());
    }
    const auto& budgets = effective_budget_result.value();
    if (budgets.strategy == AnalysisStrategy::ExecutionClosure &&
        options.execution_closure_roots.empty())
    {
        return Result<FinalizedFunctionMap>::failure(make_error(
            ErrorCode::InvalidArgument,
            "execution-closure analysis requires at least one explicit closure root"));
    }

    std::map<GuestAddress, FunctionRecord> records;
    std::set<GuestAddress> pending;
    std::set<GuestAddress> processed;
    std::set<GuestAddress> known_function_entries = options.cfg.known_function_entries;
    const bool reuse_enabled = options.reuse_map != nullptr;
    std::set<GuestAddress> reused_originals;
    std::set<GuestAddress> previous_canonical_entries;
    std::set<GuestAddress> previous_boundary_entries;
    if (reuse_enabled)
    {
        const auto& previous = *options.reuse_map;
        if (!previous.frozen() || previous.identity().module != input.identity.module ||
            previous.identity().build_id != input.identity.build_id ||
            previous.identity().input_sha256 != input.identity.input_sha256 ||
            previous.identity().guest_base != input.identity.guest_base ||
            previous.identity().executable_ranges != input.identity.executable_ranges)
        {
            return Result<FinalizedFunctionMap>::failure(make_error(
                ErrorCode::InvalidArgument,
                "persistent function analysis reuse requires the same module identity and executable layout"));
        }
        for (const auto& function : previous.functions())
        {
            records.emplace(function.canonical_entry, function);
            reused_originals.insert(function.canonical_entry);
            previous_canonical_entries.insert(function.canonical_entry);
            processed.insert(function.canonical_entry);
            for (const auto entry : function.entries)
            {
                known_function_entries.insert(entry);
                previous_boundary_entries.insert(entry);
            }
            for (const auto target : function.direct_calls)
            {
                if (contains_any(input.identity.executable_ranges, target, 4U))
                {
                    known_function_entries.insert(target);
                    previous_boundary_entries.insert(target);
                }
            }
        }
        accounting.reused_functions = records.size();
        accounting.refinement_transactions = 1U;
    }
    if (budgets.strategy == AnalysisStrategy::ExecutionClosure)
    {
        for (auto entry = known_function_entries.begin(); entry != known_function_entries.end();)
        {
            if (!options.execution_closure_roots.contains(*entry))
                entry = known_function_entries.erase(entry);
            else
                ++entry;
        }
    }
    const auto invalidate_reused = [&](GuestAddress boundary) {
        if (!reuse_enabled || previous_boundary_entries.contains(boundary)) return;
        for (auto it = reused_originals.begin(); it != reused_originals.end();)
        {
            const auto record = records.find(*it);
            if (record == records.end() ||
                (!function_owns_address(record->second, boundary) &&
                 std::find(record->second.boundary_dependencies.begin(),
                           record->second.boundary_dependencies.end(),
                           boundary) == record->second.boundary_dependencies.end()))
            {
                ++it;
                continue;
            }
            record->second.cfg.reset();
            record->second.owned_code_ranges.clear();
            record->second.boundary_dependencies.clear();
            record->second.range_begin = 0U;
            record->second.range_end = 0U;
            record->second.direct_calls.clear();
            record->second.indirect_calls.clear();
            record->second.unresolved_control_flow.clear();
            record->second.translation_status = TranslationStatus::Discovered;
            processed.erase(record->second.canonical_entry);
            pending.insert(record->second.canonical_entry);
            ++accounting.invalidated_records;
            if (accounting.reused_functions != 0U) --accounting.reused_functions;
            it = reused_originals.erase(it);
        }
    };
    for (const auto boundary : options.newly_introduced_function_entries)
    {
        if (!previous_boundary_entries.contains(boundary))
        {
            known_function_entries.insert(boundary);
            invalidate_reused(boundary);
        }
    }
    std::size_t seed_count = 0U;
    for (const auto& seed : input.seeds)
    {
        auto& source = seed_source_accounting(accounting.seed_sources, seed.source);
        ++source.observed;
        const auto valid_seed = validate_seed(input, seed);
        if (!valid_seed)
        {
            return Result<FinalizedFunctionMap>::failure(valid_seed.error());
        }
        const auto canonical = seed.canonical_entry.value_or(seed.entry);
        const bool include = budgets.strategy != AnalysisStrategy::ExecutionClosure ||
                             options.execution_closure_roots.contains(seed.entry) ||
                             options.execution_closure_roots.contains(canonical);
        if (!include)
        {
            ++source.excluded;
            ++accounting.excluded_candidate_seed_count;
            continue;
        }
        ++source.included;
        const bool was_new = records.find(canonical) == records.end();
        bool reused_record = reuse_enabled && reused_originals.contains(canonical);
        if (reused_record && is_boundary_worthy_function_seed(seed) &&
            !previous_boundary_entries.contains(seed.entry))
        {
            known_function_entries.insert(seed.entry);
            invalidate_reused(seed.entry);
            reused_record = false;
        }
        if (reused_record)
        {
            const auto evidence = add_evidence(records.find(canonical)->second, seed);
            if (!evidence) return Result<FinalizedFunctionMap>::failure(evidence.error());
        }
        else
        {
            const auto added = add_seed(records, pending, input, seed, budgets, seed_count,
                                        accounting, "initial_seeding");
            if (!added)
            {
                return Result<FinalizedFunctionMap>::failure(added.error());
            }
        }
        if (was_new) ++source.new_canonical_entries;
        else
        {
            ++source.coalesced;
            ++accounting.duplicate_coalesced_seed_count;
        }
        if (is_boundary_worthy_function_seed(seed))
        {
            if (budgets.strategy != AnalysisStrategy::ExecutionClosure ||
                options.execution_closure_roots.contains(seed.entry) ||
                options.execution_closure_roots.contains(canonical))
            {
                known_function_entries.insert(canonical);
                known_function_entries.insert(seed.entry);
            }
        }
    }
    accounting.normalized_unique_seed_count = records.size();
    if (records.empty())
    {
        return Result<FinalizedFunctionMap>::failure(
            make_error(budgets.strategy == AnalysisStrategy::ExecutionClosure
                           ? ErrorCode::AnalysisScopeViolation
                           : ErrorCode::InvalidArgument,
                       budgets.strategy == AnalysisStrategy::ExecutionClosure
                           ? "execution-closure roots did not match any supplied function seed"
                           : "function discovery requires at least one seed"));
    }

    AnalysisOptions cfg_options = options.cfg;
    cfg_options.max_instructions = std::min(cfg_options.max_instructions,
                                            budgets.max_instructions);
    cfg_options.max_basic_blocks = std::min(cfg_options.max_basic_blocks, budgets.max_blocks);
    cfg_options.max_pending_targets = std::min(cfg_options.max_pending_targets,
                                               budgets.max_seeds);
    if (!cfg_options.allowed_code_range && input.identity.executable_ranges.size() == 1U)
    {
        cfg_options.allowed_code_range = input.identity.executable_ranges.front();
    }
    cfg_options.known_function_entries = known_function_entries;

    std::size_t analyzed_instructions = 0U;
    std::size_t analyzed_blocks = 0U;
    std::size_t analyzed_edges = 0U;
    memory::GuestSize analyzed_bytes = 0U;
    const auto account_graph = [&](const ControlFlowGraph& graph, std::string_view phase,
                                   std::size_t pending_work,
                                   std::optional<GuestAddress> next_work) -> Result<void> {
        std::size_t next_instructions = 0U;
        std::size_t next_blocks = 0U;
        if (!checked_size_add(analyzed_instructions, graph.instruction_count, next_instructions) ||
            !checked_size_add(analyzed_blocks, graph.blocks.size(), next_blocks))
        {
            return Result<void>::failure(budget_failure(
                accounting, AnalysisBudgetDimension::Instructions, analyzed_instructions,
                budgets.max_instructions, std::string(phase), pending_work, next_work,
                "function discovery host-size budget overflow"));
        }

        std::size_t graph_edges = graph.unresolved.size();
        for (const auto& [unused, block] : graph.blocks)
        {
            (void)unused;
            std::size_t block_edges = 0U;
            if (!checked_size_add(block.successors.size(), block.calls.size(), block_edges) ||
                !checked_size_add(graph_edges, block_edges, graph_edges))
            {
                return Result<void>::failure(budget_failure(
                    accounting, AnalysisBudgetDimension::Edges, analyzed_edges, budgets.max_edges,
                    std::string(phase), pending_work, next_work,
                    "function discovery edge budget overflow"));
            }
        }
        std::size_t next_edges = 0U;
        if (!checked_size_add(analyzed_edges, graph_edges, next_edges))
        {
            return Result<void>::failure(budget_failure(
                accounting, AnalysisBudgetDimension::Edges, analyzed_edges, budgets.max_edges,
                std::string(phase), pending_work, next_work,
                "function discovery edge budget overflow"));
        }
        if (graph.instruction_count >
            static_cast<std::size_t>(std::numeric_limits<memory::GuestSize>::max() / 4U))
        {
            return Result<void>::failure(budget_failure(
                accounting, AnalysisBudgetDimension::BytesAnalyzed, analyzed_bytes,
                budgets.max_bytes_analyzed, std::string(phase), pending_work, next_work,
                "function CFG byte count overflows the guest size type"));
        }
        const auto graph_bytes = static_cast<memory::GuestSize>(graph.instruction_count) * 4U;
        if (analyzed_bytes > std::numeric_limits<memory::GuestSize>::max() - graph_bytes)
        {
            return Result<void>::failure(budget_failure(
                accounting, AnalysisBudgetDimension::BytesAnalyzed, analyzed_bytes,
                budgets.max_bytes_analyzed, std::string(phase), pending_work, next_work,
                "function discovery byte budget overflow"));
        }
        const auto next_bytes = analyzed_bytes + graph_bytes;
        if (next_instructions > budgets.max_instructions)
        {
            return Result<void>::failure(budget_failure(
                accounting, AnalysisBudgetDimension::Instructions, analyzed_instructions,
                budgets.max_instructions, std::string(phase), pending_work, next_work,
                "function discovery exceeded the instruction budget"));
        }
        if (next_blocks > budgets.max_blocks)
        {
            return Result<void>::failure(budget_failure(
                accounting, AnalysisBudgetDimension::Blocks, analyzed_blocks, budgets.max_blocks,
                std::string(phase), pending_work, next_work,
                "function discovery exceeded the block budget"));
        }
        if (next_edges > budgets.max_edges)
        {
            return Result<void>::failure(budget_failure(
                accounting, AnalysisBudgetDimension::Edges, analyzed_edges, budgets.max_edges,
                std::string(phase), pending_work, next_work,
                "function discovery exceeded the edge budget"));
        }
        if (next_bytes > budgets.max_bytes_analyzed)
        {
            return Result<void>::failure(budget_failure(
                accounting, AnalysisBudgetDimension::BytesAnalyzed, analyzed_bytes,
                budgets.max_bytes_analyzed, std::string(phase), pending_work, next_work,
                "function discovery exceeded the byte budget"));
        }
        analyzed_instructions = next_instructions;
        analyzed_blocks = next_blocks;
        analyzed_edges = next_edges;
        analyzed_bytes = next_bytes;
        accounting.instructions_consumed = analyzed_instructions;
        accounting.blocks_consumed = analyzed_blocks;
        accounting.edges_consumed = analyzed_edges;
        accounting.bytes_analyzed = analyzed_bytes;
        return Result<void>::success();
    };
    while (!pending.empty())
    {
        const auto entry = *pending.begin();
        pending.erase(pending.begin());
        if (!processed.insert(entry).second)
        {
            continue;
        }
        auto record = records.find(entry);
        if (record == records.end())
        {
            return Result<FinalizedFunctionMap>::failure(
                make_error(ErrorCode::InvalidFormat, "function discovery lost a pending seed"));
        }

        const auto graph = analyze_control_flow(*input.memory, entry, cfg_options);
        if (!graph)
        {
            if (const auto dimension = cfg_budget_dimension(graph.error().code))
            {
                const auto consumed = [&]() {
                    switch (dimension.value())
                    {
                    case AnalysisBudgetDimension::Instructions: return analyzed_instructions;
                    case AnalysisBudgetDimension::Blocks: return analyzed_blocks;
                    case AnalysisBudgetDimension::Seeds: return pending.size();
                    default: return std::size_t{0U};
                    }
                }();
                return Result<FinalizedFunctionMap>::failure(budget_failure(
                    accounting, dimension.value(), consumed,
                    dimension.value() == AnalysisBudgetDimension::Instructions
                        ? budgets.max_instructions
                        : dimension.value() == AnalysisBudgetDimension::Blocks
                            ? budgets.max_blocks
                            : budgets.max_seeds,
                    "cfg_discovery", pending.size(), entry,
                    "function CFG analysis exceeded its effective budget: " +
                        graph.error().message));
            }
            ++accounting.failed_functions;
            record->second.translation_status = TranslationStatus::Failed;
            record->second.diagnostics.push_back(FunctionDiagnostic{
                category_for(graph.error().code), graph.error().code, entry, std::nullopt,
                graph.error().message});
            if (!options.continue_after_function_failure)
            {
                return Result<FinalizedFunctionMap>::failure(graph.error());
            }
            continue;
        }

        const auto& graph_value = graph.value();
        const auto accounted = account_graph(
            graph_value, "cfg_discovery", pending.size(),
            pending.empty() ? std::nullopt : std::optional<GuestAddress>(*pending.begin()));
        if (!accounted)
        {
            return Result<FinalizedFunctionMap>::failure(accounted.error());
        }
        ++accounting.functions_cfg_analyzed;
        if (reuse_enabled && previous_canonical_entries.contains(entry))
            ++accounting.reanalyzed_functions;
        else
            ++accounting.newly_analyzed_functions;
        ++accounting.phases.cfg_discovery;

        record->second.cfg = graph_value;
        ++accounting.phases.ownership_normalization;
        const auto range = populate_ownership(record->second, input.identity.executable_ranges);
        if (!range)
        {
            ++accounting.failed_functions;
            record->second.translation_status = TranslationStatus::Failed;
            record->second.diagnostics.push_back(FunctionDiagnostic{
                category_for(range.error().code), range.error().code, entry, std::nullopt,
                range.error().message});
            if (!options.continue_after_function_failure)
            {
                return Result<FinalizedFunctionMap>::failure(range.error());
            }
            continue;
        }
        populate_boundary_dependencies(record->second, known_function_entries);
        record->second.translation_status = TranslationStatus::Analyzed;

        for (const auto target : record->second.direct_calls)
        {
            ++accounting.direct_call_discoveries;
            ++accounting.phases.direct_call_expansion;
            if (contains_any(input.identity.executable_ranges, target, 4U))
            {
                const FunctionSeed seed{target, FunctionDiscoverySource::DirectCall,
                                        FunctionConfidence::High, std::nullopt, std::nullopt,
                                        "validated direct BL target"};
                auto& source = seed_source_accounting(accounting.seed_sources, seed.source);
                ++source.observed;
                ++source.included;
                const bool was_new = records.find(target) == records.end();
                const auto added = add_seed(records, pending, input, seed, budgets, seed_count,
                                            accounting, "direct_call_expansion");
                if (!added)
                {
                    return Result<FinalizedFunctionMap>::failure(added.error());
                }
                ++accounting.new_seeds_generated;
                if (was_new) ++source.new_canonical_entries;
                else
                {
                    ++source.coalesced;
                    ++accounting.duplicate_coalesced_seed_count;
                }
                if (known_function_entries.insert(target).second) invalidate_reused(target);
                cfg_options.known_function_entries = known_function_entries;
            }
        }
    }

    // Discovery deliberately permits enough ordinary CFG traversal to expose
    // direct BL evidence. Once that fixed point is known, re-analyze every
    // successful function with the complete strong-entry set so no stale CFG
    // can retain ownership across a later-discovered function boundary.
    for (std::size_t pass = 0U; pass < budgets.max_boundary_finalization_passes; ++pass)
    {
        ++accounting.boundary_finalization_passes;
        ++accounting.phases.boundary_finalization;
        bool boundary_set_changed = false;
        cfg_options.known_function_entries = known_function_entries;
        for (auto& [entry, record] : records)
        {
            if (reuse_enabled && reused_originals.contains(entry)) continue;
            if (!record.cfg && record.translation_status != TranslationStatus::Discovered)
            {
                continue;
            }
            const auto graph = analyze_control_flow(*input.memory, entry, cfg_options);
            if (!graph)
            {
                if (const auto dimension = cfg_budget_dimension(graph.error().code))
                {
                    const auto consumed = [&]() {
                        switch (dimension.value())
                        {
                        case AnalysisBudgetDimension::Instructions: return analyzed_instructions;
                        case AnalysisBudgetDimension::Blocks: return analyzed_blocks;
                        case AnalysisBudgetDimension::Seeds: return pending.size();
                        default: return std::size_t{0U};
                        }
                    }();
                    return Result<FinalizedFunctionMap>::failure(budget_failure(
                        accounting, dimension.value(), consumed,
                        dimension.value() == AnalysisBudgetDimension::Instructions
                            ? budgets.max_instructions
                            : dimension.value() == AnalysisBudgetDimension::Blocks
                                ? budgets.max_blocks
                                : budgets.max_seeds,
                        "boundary_finalization", pending.size(), entry,
                        "boundary-aware CFG analysis exceeded its effective budget: " +
                            graph.error().message));
                }
                ++accounting.failed_functions;
                record.cfg.reset();
                record.owned_code_ranges.clear();
                record.range_begin = 0U;
                record.range_end = 0U;
                record.direct_calls.clear();
                record.indirect_calls.clear();
                record.unresolved_control_flow.clear();
                record.translation_status = TranslationStatus::Failed;
                record.diagnostics.push_back(FunctionDiagnostic{
                    category_for(graph.error().code), graph.error().code, entry, std::nullopt,
                    "boundary-aware finalization: " + graph.error().message});
                continue;
            }
            const auto accounted = account_graph(
                graph.value(), "boundary_finalization", pending.size(),
                pending.empty() ? std::nullopt : std::optional<GuestAddress>(*pending.begin()));
            if (!accounted)
            {
                return Result<FinalizedFunctionMap>::failure(accounted.error());
            }
            ++accounting.functions_cfg_analyzed;
            if (reuse_enabled && previous_canonical_entries.contains(entry))
                ++accounting.reanalyzed_functions;
            else
                ++accounting.newly_analyzed_functions;
            record.cfg = graph.value();
            ++accounting.phases.ownership_normalization;
            const auto range = populate_ownership(record, input.identity.executable_ranges);
            if (!range)
            {
                ++accounting.failed_functions;
                record.translation_status = TranslationStatus::Failed;
                record.diagnostics.push_back(FunctionDiagnostic{
                    category_for(range.error().code), range.error().code, entry, std::nullopt,
                    range.error().message});
                continue;
            }
            populate_boundary_dependencies(record, known_function_entries);
            record.translation_status = TranslationStatus::Analyzed;
            for (const auto target : record.direct_calls)
            {
                ++accounting.direct_call_discoveries;
                ++accounting.phases.direct_call_expansion;
                if (!contains_any(input.identity.executable_ranges, target, 4U))
                {
                    continue;
                }
                const FunctionSeed seed{target, FunctionDiscoverySource::DirectCall,
                                        FunctionConfidence::High, std::nullopt, std::nullopt,
                                        "validated direct BL target"};
                auto& source = seed_source_accounting(accounting.seed_sources, seed.source);
                ++source.observed;
                ++source.included;
                const bool was_new = records.find(target) == records.end();
                const auto added = add_seed(records, pending, input, seed, budgets, seed_count,
                                            accounting, "boundary_finalization");
                if (!added)
                {
                    return Result<FinalizedFunctionMap>::failure(added.error());
                }
                ++accounting.new_seeds_generated;
                if (was_new) ++source.new_canonical_entries;
                else
                {
                    ++source.coalesced;
                    ++accounting.duplicate_coalesced_seed_count;
                }
                if (known_function_entries.insert(target).second)
                {
                    invalidate_reused(target);
                    boundary_set_changed = true;
                }
            }
        }
        pending.clear();
        if (!boundary_set_changed)
        {
            break;
        }
        if (pass + 1U == budgets.max_boundary_finalization_passes)
        {
            return Result<FinalizedFunctionMap>::failure(budget_failure(
                accounting, AnalysisBudgetDimension::BoundaryFinalizationPasses,
                pass + 1U, budgets.max_boundary_finalization_passes,
                "boundary_finalization", pending.size(),
                pending.empty() ? std::nullopt
                                : std::optional<GuestAddress>(*pending.begin()),
                "function boundary finalization did not reach a fixed point within its budget"));
        }
    }

    FinalizedFunctionMap result;
    result.identity_ = input.identity;
    for (const auto& seed : input.seeds)
    {
        if (seed.source != FunctionDiscoverySource::ModuleEntry ||
            seed.confidence != FunctionConfidence::Confirmed)
        {
            continue;
        }
        const auto address = seed.canonical_entry.value_or(seed.entry);
        const auto already_recorded = std::any_of(
            result.identity_.entry_points.begin(), result.identity_.entry_points.end(),
            [address](const EntryPointEvidence& evidence) {
                return evidence.kind == EntryPointKind::VerifiedProcessEntry &&
                       evidence.address == address;
            });
        if (!already_recorded)
        {
            result.identity_.entry_points.push_back(EntryPointEvidence{
                address, EntryPointKind::VerifiedProcessEntry, "confirmed module-entry seed",
                FunctionConfidence::Confirmed, true,
                "verified runtime entry supplied by the launch model"});
        }
    }
    std::sort(result.identity_.entry_points.begin(), result.identity_.entry_points.end(),
              [](const EntryPointEvidence& left, const EntryPointEvidence& right) {
                  if (left.address != right.address) return left.address < right.address;
                  if (left.kind != right.kind) return left.kind < right.kind;
                  if (left.provenance != right.provenance) return left.provenance < right.provenance;
                  if (left.confidence != right.confidence) return left.confidence < right.confidence;
                  if (left.verified_runtime_entry != right.verified_runtime_entry)
                  {
                      return left.verified_runtime_entry < right.verified_runtime_entry;
                  }
                  return left.note < right.note;
              });
    result.functions_.reserve(records.size());
    for (auto& [unused, record] : records)
    {
        (void)unused;
        std::sort(record.entries.begin(), record.entries.end());
        record.entries.erase(std::unique(record.entries.begin(), record.entries.end()),
                             record.entries.end());
        std::sort(record.evidence.begin(), record.evidence.end(),
                  [](const DiscoveryEvidence& left, const DiscoveryEvidence& right) {
                      return std::tie(left.entry, left.source, left.confidence, left.note) <
                             std::tie(right.entry, right.source, right.confidence, right.note);
                  });
        if (!record.name)
        {
            record.name = record.synthetic_id;
        }
        result.functions_.push_back(std::move(record));
    }
    std::sort(result.functions_.begin(), result.functions_.end(),
              [](const FunctionRecord& left, const FunctionRecord& right) {
                  return left.canonical_entry < right.canonical_entry;
              });

    // Compute the complete conflict set only after all fixed-point discovery
    // and range population has finished. The result functions are already in
    // canonical-entry order, so each pair is considered exactly once and its
    // identity is independent of seed/discovery order.
    for (std::size_t left = 0U; left < result.functions_.size(); ++left)
    {
        ++accounting.phases.conflict_processing;
        if (!result.functions_[left].cfg)
        {
            continue;
        }
        for (std::size_t right = left + 1U; right < result.functions_.size(); ++right)
        {
            ++accounting.phases.conflict_processing;
            if (!result.functions_[right].cfg ||
                !ranges_overlap(result.functions_[left], result.functions_[right]))
            {
                continue;
            }
            const auto conflict = make_conflict(result.functions_[left], result.functions_[right],
                                                input.identity.module);
            if (!conflict)
            {
                return Result<FinalizedFunctionMap>::failure(conflict.error());
            }
            result.conflicts_.push_back(conflict.value());
        }
    }
    // Mark records only after all conflict records have captured the original
    // discovery confidence. The evidence vector remains the provenance source
    // even after the finalized record receives Conflict status.
    for (const auto& conflict : result.conflicts_)
    {
        const auto first = std::lower_bound(
            result.functions_.begin(), result.functions_.end(), conflict.first_function,
            [](const FunctionRecord& function, GuestAddress entry) {
                return function.canonical_entry < entry;
            });
        const auto second = std::lower_bound(
            result.functions_.begin(), result.functions_.end(), conflict.second_function,
            [](const FunctionRecord& function, GuestAddress entry) {
                return function.canonical_entry < entry;
            });
        if (first != result.functions_.end() && first->canonical_entry == conflict.first_function)
        {
            first->confidence = FunctionConfidence::Conflict;
            first->entry_trust_status = FunctionEntryTrustStatus::Conflict;
            first->translation_status = TranslationStatus::Conflict;
        }
        if (second != result.functions_.end() &&
            second->canonical_entry == conflict.second_function)
        {
            second->confidence = FunctionConfidence::Conflict;
            second->entry_trust_status = FunctionEntryTrustStatus::Conflict;
            second->translation_status = TranslationStatus::Conflict;
        }
    }
    accounting.discovered_canonical_functions = result.functions_.size();
    accounting.normalized_unique_seed_count = result.functions_.size();
    std::set<std::pair<GuestAddress, FunctionDiscoverySource>> source_entries;
    for (const auto& function : result.functions_)
    {
        for (const auto& evidence : function.evidence)
        {
            source_entries.emplace(function.canonical_entry, evidence.source);
        }
    }
    std::size_t included_seed_count = 0U;
    for (auto& source : accounting.seed_sources)
    {
        source.new_canonical_entries = static_cast<std::size_t>(std::count_if(
            source_entries.begin(), source_entries.end(),
            [&source](const auto& entry) { return entry.second == source.source; }));
        source.coalesced = source.included >= source.new_canonical_entries
                               ? source.included - source.new_canonical_entries
                               : 0U;
        included_seed_count += source.included;
    }
    accounting.duplicate_coalesced_seed_count =
        included_seed_count >= accounting.normalized_unique_seed_count
            ? included_seed_count - accounting.normalized_unique_seed_count
            : 0U;
    accounting.canonical_functions_with_cfg = static_cast<std::size_t>(std::count_if(
        result.functions_.begin(), result.functions_.end(), [](const FunctionRecord& function) {
            return function.cfg.has_value();
        }));
    accounting.candidate_function_entries = static_cast<std::size_t>(std::count_if(
        result.functions_.begin(), result.functions_.end(), [](const FunctionRecord& function) {
            return function.entry_trust_status == FunctionEntryTrustStatus::Candidate;
        }));
    accounting.trusted_function_entries = static_cast<std::size_t>(std::count_if(
        result.functions_.begin(), result.functions_.end(), [](const FunctionRecord& function) {
            return function.entry_trust_status == FunctionEntryTrustStatus::Trusted;
        }));
    accounting.function_boundary_conflicts = result.conflicts_.size();
    accounting.phases.immutable_map_reconstruction = 1U;
    std::sort(accounting.seed_sources.begin(), accounting.seed_sources.end(),
              [](const auto& left, const auto& right) { return left.source < right.source; });
    result.accounting_ = std::move(accounting);
    result.frozen_ = true;
    const auto valid = validate_finalized_function_map(result);
    if (!valid)
    {
        return Result<FinalizedFunctionMap>::failure(valid.error());
    }
    return Result<FinalizedFunctionMap>::success(std::move(result));
}

Result<void> validate_finalized_function_map(const FinalizedFunctionMap& map)
{
    if (!map.frozen_)
    {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidFormat, "function map must be frozen before validation"));
    }
    GuestAddress previous = 0U;
    bool first = true;
    std::set<GuestAddress> all_entries;
    for (const auto& function : map.functions_)
    {
        if (function.module != map.identity_.module || function.entries.empty() ||
            function.synthetic_id.empty())
        {
            return Result<void>::failure(
                make_error(ErrorCode::InvalidFormat, "function map contains incomplete function identity"));
        }
        if ((function.translation_status == TranslationStatus::Conflict) !=
            (function.entry_trust_status == FunctionEntryTrustStatus::Conflict))
        {
            return Result<void>::failure(make_error(
                ErrorCode::FunctionBoundaryConflict,
                "function map trust status does not match its conflict status"));
        }
        if (!first && function.canonical_entry <= previous)
        {
            return Result<void>::failure(
                make_error(ErrorCode::InvalidFormat, "function map is not sorted by canonical entry"));
        }
        previous = function.canonical_entry;
        first = false;
        for (const auto entry : function.entries)
        {
            if ((entry & 0x3U) != 0U || !all_entries.insert(entry).second ||
                !contains_any(map.identity_.executable_ranges, entry, 4U))
            {
                return Result<void>::failure(
                    make_error(ErrorCode::InvalidGuestAddress,
                               "function map contains an invalid or duplicate entry"));
            }
        }
        if (function.cfg)
        {
            std::vector<GuestAddress> instruction_addresses;
            instruction_addresses.reserve(function.cfg->instruction_count);
            for (const auto& [unused, block] : function.cfg->blocks)
            {
                (void)unused;
                for (const auto& instruction : block.instructions)
                {
                    instruction_addresses.push_back(instruction.address);
                }
            }
            const auto expected_ranges = normalize_code_ranges(instruction_addresses);
            if (!expected_ranges || expected_ranges.value() != function.owned_code_ranges ||
                function.owned_code_ranges.empty() ||
                function.range_begin != function.owned_code_ranges.front().base)
            {
                return Result<void>::failure(make_error(
                    ErrorCode::InvalidGuestAddress,
                    "function precise ownership does not match its decoded CFG"));
            }
            if (!std::is_sorted(function.boundary_dependencies.begin(),
                                function.boundary_dependencies.end()) ||
                std::adjacent_find(function.boundary_dependencies.begin(),
                                   function.boundary_dependencies.end()) !=
                    function.boundary_dependencies.end() ||
                std::any_of(function.boundary_dependencies.begin(),
                            function.boundary_dependencies.end(),
                            [&function](const auto entry) {
                                return !function_owns_address(function, entry);
                            }))
            {
                return Result<void>::failure(make_error(
                    ErrorCode::FunctionBoundaryConflict,
                    "function boundary dependencies are not normalized or owned"));
            }
            GuestAddress previous_end = 0U;
            bool first_range = true;
            for (const auto& range : function.owned_code_ranges)
            {
                if (range.size == 0U || (range.base & 0x3U) != 0U ||
                    (range.size & 0x3U) != 0U)
                {
                    return Result<void>::failure(make_error(
                        ErrorCode::InvalidGuestAddress,
                        "function ownership contains an empty or misaligned range"));
                }
                const auto end = checked_add_u64(range.base, range.size);
                if (!end || !contains_any(map.identity_.executable_ranges, range.base, range.size) ||
                    (!first_range && range.base <= previous_end))
                {
                    return Result<void>::failure(make_error(
                        ErrorCode::InvalidGuestAddress,
                        "function ownership is outside executable memory or not normalized"));
                }
                previous_end = end.value();
                first_range = false;
            }
            if (function.range_end != previous_end || function.range_end <= function.range_begin)
            {
                return Result<void>::failure(make_error(
                    ErrorCode::InvalidGuestAddress,
                    "function envelope does not bound its precise ownership"));
            }
            const auto valid_cfg = validate_control_flow_graph(function.cfg.value());
            if (!valid_cfg)
            {
                return Result<void>::failure(valid_cfg.error());
            }
        }
    }

    std::set<std::pair<GuestAddress, GuestAddress>> recorded_conflicts;
    std::optional<std::pair<GuestAddress, GuestAddress>> previous_conflict;
    for (const auto& conflict : map.conflicts_)
    {
        if (conflict.module != map.identity_.module ||
            conflict.first_function >= conflict.second_function ||
            !recorded_conflicts.emplace(conflict.first_function, conflict.second_function).second ||
            (previous_conflict &&
             *previous_conflict >=
                 std::make_pair(conflict.first_function, conflict.second_function)))
        {
            return Result<void>::failure(make_error(
                ErrorCode::FunctionBoundaryConflict,
                "function map contains a duplicate or non-normalized conflict"));
        }
        previous_conflict =
            std::make_pair(conflict.first_function, conflict.second_function);

        const auto first = std::lower_bound(
            map.functions_.begin(), map.functions_.end(), conflict.first_function,
            [](const FunctionRecord& function, GuestAddress entry) {
                return function.canonical_entry < entry;
            });
        const auto second = std::lower_bound(
            map.functions_.begin(), map.functions_.end(), conflict.second_function,
            [](const FunctionRecord& function, GuestAddress entry) {
                return function.canonical_entry < entry;
            });
        if (first == map.functions_.end() || first->canonical_entry != conflict.first_function ||
            second == map.functions_.end() || second->canonical_entry != conflict.second_function ||
            !first->cfg || !second->cfg ||
            conflict.first_range.base != first->range_begin ||
            conflict.first_range.size != first->range_end - first->range_begin ||
            conflict.second_range.base != second->range_begin ||
            conflict.second_range.size != second->range_end - second->range_begin)
        {
            return Result<void>::failure(make_error(
                ErrorCode::FunctionBoundaryConflict,
                "function map contains a phantom or stale boundary conflict"));
        }
        const auto expected_overlap =
            intersect_owned_ranges(first->owned_code_ranges, second->owned_code_ranges);
        const auto normalized_overlap = normalize_code_ranges(conflict.overlap_ranges);
        if (!expected_overlap || !normalized_overlap || expected_overlap.value().empty() ||
            normalized_overlap.value() != conflict.overlap_ranges ||
            expected_overlap.value() != conflict.overlap_ranges)
        {
            return Result<void>::failure(make_error(
                ErrorCode::FunctionBoundaryConflict,
                "function conflict does not identify the exact normalized ownership overlap"));
        }
    }

    std::set<std::pair<GuestAddress, GuestAddress>> expected_conflicts;
    for (std::size_t left = 0U; left < map.functions_.size(); ++left)
    {
        if (!map.functions_[left].cfg)
        {
            continue;
        }
        for (std::size_t right = left + 1U; right < map.functions_.size(); ++right)
        {
            if (!map.functions_[right].cfg)
            {
                continue;
            }
            if (!ranges_overlap(map.functions_[left], map.functions_[right]))
            {
                continue;
            }
            const auto pair = std::make_pair(map.functions_[left].canonical_entry,
                                             map.functions_[right].canonical_entry);
            expected_conflicts.insert(pair);
            if (recorded_conflicts.find(pair) == recorded_conflicts.end())
            {
                return Result<void>::failure(make_error(
                    ErrorCode::FunctionBoundaryConflict,
                    "overlapping function ranges were not recorded as a conflict"));
            }
            if (map.functions_[left].confidence != FunctionConfidence::Conflict ||
                map.functions_[left].translation_status != TranslationStatus::Conflict ||
                map.functions_[right].confidence != FunctionConfidence::Conflict ||
                map.functions_[right].translation_status != TranslationStatus::Conflict)
            {
                return Result<void>::failure(make_error(
                    ErrorCode::FunctionBoundaryConflict,
                    "overlapping functions were not marked as conflicting"));
            }
        }
    }
    if (recorded_conflicts != expected_conflicts)
    {
        return Result<void>::failure(make_error(
            ErrorCode::FunctionBoundaryConflict,
            "finalized function map conflict set does not match precise ownership"));
    }
    return Result<void>::success();
}

} // namespace switchrecomp::analysis
