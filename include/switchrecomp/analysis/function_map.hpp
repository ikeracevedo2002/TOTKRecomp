#pragma once

#include "switchrecomp/analysis/cfg_analyzer.hpp"
#include "switchrecomp/common/result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace switchrecomp::analysis
{

enum class FunctionDiscoverySource : std::uint8_t
{
    ModuleEntry,
    TextStartCandidate,
    DynamicSymbol,
    Export,
    DirectCall,
    RelocationReference,
    AnalystSeed,
    ManualOverride,
    JumpTable,
    ObservedIndirectTarget,
    Heuristic,
};

enum class AnalysisStrategy : std::uint8_t
{
    WholeModule,
    ExecutionClosure,
};

[[nodiscard]] std::string_view analysis_strategy_name(AnalysisStrategy strategy) noexcept;

enum class AnalysisBudgetDimension : std::uint8_t
{
    None,
    Functions,
    Instructions,
    Blocks,
    Edges,
    Seeds,
    BytesAnalyzed,
    BoundaryFinalizationPasses,
};

[[nodiscard]] std::string_view analysis_budget_dimension_name(
    AnalysisBudgetDimension dimension) noexcept;

enum class AnalysisBudgetProvenanceKind : std::uint8_t
{
    LibraryDefault,
    ExecutionToolProfile,
    ExplicitCliOverride,
    LocalConfigurationOverride,
    DerivedStructuralBound,
};

[[nodiscard]] std::string_view analysis_budget_provenance_kind_name(
    AnalysisBudgetProvenanceKind kind) noexcept;

struct AnalysisBudgetProvenance
{
    AnalysisBudgetProvenanceKind kind = AnalysisBudgetProvenanceKind::LibraryDefault;
    std::string detail = "library_default";
};

struct AnalysisBudgetProvenanceSet
{
    AnalysisBudgetProvenance max_functions;
    AnalysisBudgetProvenance max_instructions;
    AnalysisBudgetProvenance max_blocks;
    AnalysisBudgetProvenance max_edges;
    AnalysisBudgetProvenance max_seeds;
    AnalysisBudgetProvenance max_bytes_analyzed;
    AnalysisBudgetProvenance max_boundary_finalization_passes;
};

enum class ModuleBaseProvenance : std::uint8_t
{
    ExplicitAnalysisBase,
    DeterministicAnalysisLayout,
    ExternallyObserved,
    RuntimeVerified,
};

[[nodiscard]] std::string_view module_base_provenance_name(
    ModuleBaseProvenance provenance) noexcept;

enum class EntryPointKind : std::uint8_t
{
    TextStartCandidate,
    DynamicInit,
    DynamicFini,
    VerifiedProcessEntry,
    AnalystOverride,
};

[[nodiscard]] std::string_view entry_point_kind_name(EntryPointKind kind) noexcept;

enum class FunctionConfidence : std::uint8_t
{
    Confirmed,
    High,
    Medium,
    Low,
    Manual,
    Conflict,
};

struct EntryPointEvidence
{
    memory::GuestAddress address = 0U;
    EntryPointKind kind = EntryPointKind::TextStartCandidate;
    std::string provenance;
    FunctionConfidence confidence = FunctionConfidence::Low;
    bool verified_runtime_entry = false;
    std::string note;
};

enum class TranslationStatus : std::uint8_t
{
    Discovered,
    Analyzed,
    Lifted,
    Verified,
    Lowerable,
    Translated,
    Unsupported,
    Excluded,
    Failed,
    Conflict,
};

// A discovered record can exist in the map before it is allowed to serve as
// an indirect-call ownership boundary.  Keeping this separate from
// FunctionConfidence prevents executable-looking candidates from becoming
// trusted merely because they were decoded.
enum class FunctionEntryTrustStatus : std::uint8_t
{
    Candidate,
    Trusted,
    Conflict,
};

[[nodiscard]] std::string_view function_entry_trust_status_name(
    FunctionEntryTrustStatus status) noexcept;

enum class FailureCategory : std::uint8_t
{
    DecodeFailure,
    InvalidInstruction,
    InvalidCFG,
    UnsupportedInstruction,
    UnsupportedSemantic,
    UnresolvedIndirectFlow,
    FunctionBoundaryConflict,
    InvalidGuestAddress,
    AnalysisBudgetExceeded,
    IRVerificationFailure,
    MissingImportBinding,
    RuntimeBoundary,
    CodegenFailure,
};

[[nodiscard]] std::string_view function_discovery_source_name(
    FunctionDiscoverySource source) noexcept;
[[nodiscard]] std::string_view function_confidence_name(FunctionConfidence confidence) noexcept;
[[nodiscard]] std::string_view translation_status_name(TranslationStatus status) noexcept;
[[nodiscard]] std::string_view failure_category_name(FailureCategory category) noexcept;

struct ModuleIdentity
{
    std::string module;
    std::string build_id;
    std::string input_sha256;
    memory::GuestAddress guest_base = 0U;
    bool guest_base_verified = false;
    ModuleBaseProvenance guest_base_provenance = ModuleBaseProvenance::ExplicitAnalysisBase;
    std::vector<GuestAddressRange> executable_ranges;
    std::string translator_version;
    std::uint32_t metadata_schema_version = 2U;
    std::string llvm_version;
    std::vector<std::string> feature_flags;
    std::vector<EntryPointEvidence> entry_points;
};

struct FunctionSeed
{
    memory::GuestAddress entry = 0U;
    FunctionDiscoverySource source = FunctionDiscoverySource::Heuristic;
    FunctionConfidence confidence = FunctionConfidence::Low;
    std::optional<memory::GuestAddress> canonical_entry;
    std::optional<std::string> name;
    std::string note;
};

struct DiscoveryEvidence
{
    FunctionDiscoverySource source = FunctionDiscoverySource::Heuristic;
    FunctionConfidence confidence = FunctionConfidence::Low;
    memory::GuestAddress entry = 0U;
    std::string note;
};

struct FunctionDiagnostic
{
    FailureCategory category = FailureCategory::InvalidCFG;
    ErrorCode code = ErrorCode::InvalidControlFlow;
    memory::GuestAddress pc = 0U;
    std::optional<std::uint32_t> opcode;
    std::string message;
};

struct UnsupportedRecord
{
    std::string module;
    std::string function;
    memory::GuestAddress pc = 0U;
    std::uint32_t opcode = 0U;
    std::string instruction;
    std::string semantic_family;
    std::string reason;
    std::string required_action;
};

struct FunctionBoundaryConflict
{
    std::string module;
    memory::GuestAddress first_function = 0U;
    memory::GuestAddress second_function = 0U;
    GuestAddressRange first_range;
    GuestAddressRange second_range;
    // Exact normalized intersections of the two functions' owned instruction
    // ranges. first_range/second_range are retained as convex display/search
    // envelopes and are not ownership evidence.
    std::vector<GuestAddressRange> overlap_ranges;
    FunctionDiscoverySource first_source = FunctionDiscoverySource::Heuristic;
    FunctionDiscoverySource second_source = FunctionDiscoverySource::Heuristic;
    FunctionConfidence first_confidence = FunctionConfidence::Low;
    FunctionConfidence second_confidence = FunctionConfidence::Low;
    std::string resolution;
};

struct FunctionRecord
{
    std::string module;
    std::string synthetic_id;
    memory::GuestAddress canonical_entry = 0U;
    std::vector<memory::GuestAddress> entries;
    memory::GuestAddress range_begin = 0U;
    memory::GuestAddress range_end = 0U;
    // Exact normalized half-open spans made from decoded instruction PCs.
    // range_begin/range_end are only the encompassing display/search envelope.
    std::vector<GuestAddressRange> owned_code_ranges;
    FunctionDiscoverySource primary_source = FunctionDiscoverySource::Heuristic;
    FunctionConfidence confidence = FunctionConfidence::Low;
    std::optional<std::string> name;
    std::vector<DiscoveryEvidence> evidence;
    std::optional<ControlFlowGraph> cfg;
    std::vector<memory::GuestAddress> direct_calls;
    std::vector<CallSite> indirect_calls;
    std::vector<UnresolvedControlFlow> unresolved_control_flow;
    FunctionEntryTrustStatus entry_trust_status = FunctionEntryTrustStatus::Candidate;
    TranslationStatus translation_status = TranslationStatus::Discovered;
    std::vector<UnsupportedRecord> unsupported;
    std::vector<FunctionDiagnostic> diagnostics;
};

struct AnalysisBudgets
{
    std::size_t max_functions = 1'000'000U;
    std::size_t max_blocks = 16'384U;
    std::size_t max_instructions = 4'000'000U;
    std::size_t max_edges = 8'000'000U;
    std::size_t max_seeds = 2'000'000U;
    memory::GuestSize max_bytes_analyzed = memory::GuestSize{64U} * 1024U * 1024U;
    // Final boundary-aware re-analysis is bounded explicitly and consumes the
    // same aggregate instruction/block/edge/byte budgets as discovery.
    std::size_t max_boundary_finalization_passes = 8U;
    AnalysisStrategy strategy = AnalysisStrategy::WholeModule;
    AnalysisBudgetProvenanceSet provenance;
};

// The execution tool uses this generic, finite profile as its starting point.
// Callers may override individual dimensions, but the resulting provenance is
// retained in the effective AnalysisBudgets passed to the builder.
[[nodiscard]] AnalysisBudgets make_execution_closure_analysis_budgets();
void mark_analysis_budget_override(AnalysisBudgets& budgets,
                                   AnalysisBudgetDimension dimension,
                                   AnalysisBudgetProvenanceKind kind,
                                   std::string detail);

struct AnalysisBudgetExhaustion
{
    AnalysisBudgetDimension dimension = AnalysisBudgetDimension::None;
    std::size_t consumed = 0U;
    std::size_t limit = 0U;
    std::string module;
    std::string phase;
    std::size_t pending_work = 0U;
    std::optional<memory::GuestAddress> next_work;
};

struct AnalysisPhaseAccounting
{
    std::size_t initial_seeding = 0U;
    std::size_t cfg_discovery = 0U;
    std::size_t direct_call_expansion = 0U;
    std::size_t ownership_normalization = 0U;
    std::size_t conflict_processing = 0U;
    std::size_t boundary_finalization = 0U;
    std::size_t immutable_map_reconstruction = 0U;
};

struct AnalysisSeedSourceAccounting
{
    FunctionDiscoverySource source = FunctionDiscoverySource::Heuristic;
    std::size_t observed = 0U;
    std::size_t included = 0U;
    std::size_t excluded = 0U;
    std::size_t new_canonical_entries = 0U;
    std::size_t coalesced = 0U;
};

// Deterministic forensic accounting for one immutable module map. It is part
// of the map so a finalized map and the evidence that produced it cannot drift
// apart.
struct AnalysisAccounting
{
    std::string module;
    AnalysisStrategy strategy = AnalysisStrategy::WholeModule;
    AnalysisBudgets budgets;
    std::size_t initial_seed_count = 0U;
    std::size_t normalized_unique_seed_count = 0U;
    std::size_t duplicate_coalesced_seed_count = 0U;
    std::size_t excluded_candidate_seed_count = 0U;
    std::vector<AnalysisSeedSourceAccounting> seed_sources;
    std::size_t discovered_canonical_functions = 0U;
    std::size_t candidate_function_entries = 0U;
    std::size_t trusted_function_entries = 0U;
    std::size_t functions_cfg_analyzed = 0U;
    std::size_t canonical_functions_with_cfg = 0U;
    std::size_t direct_call_discoveries = 0U;
    std::size_t new_seeds_generated = 0U;
    std::size_t blocks_consumed = 0U;
    std::size_t instructions_consumed = 0U;
    std::size_t edges_consumed = 0U;
    memory::GuestSize bytes_analyzed = 0U;
    std::size_t boundary_finalization_passes = 0U;
    std::size_t failed_functions = 0U;
    std::size_t function_boundary_conflicts = 0U;
    std::size_t work_remaining_at_exhaustion = 0U;
    std::optional<AnalysisBudgetExhaustion> exhaustion;
    AnalysisPhaseAccounting phases;
};

// Stable, path-free JSON for deterministic reports and diagnostics.  The
// serializer deliberately contains only analysis evidence and finite budget
// provenance; host timings and process-local details do not enter it.
[[nodiscard]] std::string render_analysis_accounting_json(const AnalysisAccounting& accounting);

struct ModuleAnalysisInput
{
    ModuleIdentity identity;
    const memory::GuestMemory* memory = nullptr;
    std::vector<FunctionSeed> seeds;
};

struct FunctionMapOptions
{
    AnalysisBudgets budgets;
    AnalysisOptions cfg;
    // In ExecutionClosure mode only seeds matching one of these roots are
    // initial analysis work. Direct-call targets discovered from those roots
    // are still expanded transitively and are never treated as speculative.
    std::set<memory::GuestAddress> execution_closure_roots;
    bool continue_after_function_failure = true;
};

// Normalize four-byte instruction spans into deterministic, half-open code
// ranges. Duplicate and adjacent instruction spans are merged; gaps remain.
[[nodiscard]] Result<std::vector<GuestAddressRange>> normalize_code_ranges(
    std::span<const memory::GuestAddress> instruction_addresses);
[[nodiscard]] Result<std::vector<GuestAddressRange>> normalize_code_ranges(
    const std::vector<memory::GuestAddress>& instruction_addresses);
[[nodiscard]] Result<std::vector<GuestAddressRange>> normalize_code_ranges(
    std::span<const GuestAddressRange> input_ranges);
[[nodiscard]] Result<std::vector<GuestAddressRange>> normalize_code_ranges(
    const std::vector<GuestAddressRange>& input_ranges);
[[nodiscard]] bool owned_ranges_overlap(const std::vector<GuestAddressRange>& left,
                                         const std::vector<GuestAddressRange>& right) noexcept;
[[nodiscard]] Result<std::vector<GuestAddressRange>> intersect_owned_ranges(
    const std::vector<GuestAddressRange>& left, const std::vector<GuestAddressRange>& right);
[[nodiscard]] Result<memory::GuestSize> precise_owned_byte_count(
    const std::vector<GuestAddressRange>& ranges);
[[nodiscard]] bool function_owns_address(const FunctionRecord& function,
                                          memory::GuestAddress address) noexcept;
[[nodiscard]] bool is_boundary_worthy_function_seed(const FunctionSeed& seed) noexcept;

class FinalizedFunctionMap
{
  public:
    FinalizedFunctionMap() = default;

    [[nodiscard]] const ModuleIdentity& identity() const noexcept { return identity_; }
    [[nodiscard]] const std::vector<FunctionRecord>& functions() const noexcept
    {
        return functions_;
    }
    [[nodiscard]] const std::vector<FunctionBoundaryConflict>& conflicts() const noexcept
    {
        return conflicts_;
    }
    [[nodiscard]] const AnalysisAccounting& accounting() const noexcept { return accounting_; }
    // Exact callable-entry lookup includes explicitly accepted secondary
    // entries, but never infers callability from an ownership range.
    [[nodiscard]] const FunctionRecord* find_exact_entry(memory::GuestAddress entry) const noexcept;
    [[nodiscard]] const FunctionRecord* find_canonical_entry(
        memory::GuestAddress entry) const noexcept;
    [[nodiscard]] const FunctionRecord* find_callable_entry(
        memory::GuestAddress entry) const noexcept;
    [[nodiscard]] std::vector<const FunctionRecord*> find_precise_owners(
        memory::GuestAddress address) const;
    [[nodiscard]] const FunctionRecord* find(memory::GuestAddress entry) const noexcept;
    [[nodiscard]] std::vector<const FunctionRecord*> find_owners(
        memory::GuestAddress address) const;
    [[nodiscard]] bool frozen() const noexcept { return frozen_; }

  private:
    friend class FunctionMapBuilder;
    friend Result<void> validate_finalized_function_map(const FinalizedFunctionMap& map);
    ModuleIdentity identity_;
    std::vector<FunctionRecord> functions_;
    std::vector<FunctionBoundaryConflict> conflicts_;
    AnalysisAccounting accounting_;
    bool frozen_ = false;
};

class FunctionMapBuilder
{
  public:
    [[nodiscard]] static Result<FinalizedFunctionMap> build(
        const ModuleAnalysisInput& input, const FunctionMapOptions& options = {});
};

[[nodiscard]] Result<void> validate_finalized_function_map(const FinalizedFunctionMap& map);

} // namespace switchrecomp::analysis
