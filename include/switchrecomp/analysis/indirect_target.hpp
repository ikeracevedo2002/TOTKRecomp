#pragma once

#include "switchrecomp/analysis/function_map.hpp"
#include "switchrecomp/analysis/process_image.hpp"
#include "switchrecomp/format/elf_rela.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace switchrecomp::analysis
{

enum class IndirectControlFlowKind : std::uint8_t
{
    Call,
    Branch,
    Return,
};

[[nodiscard]] std::string_view indirect_control_flow_kind_name(
    IndirectControlFlowKind kind) noexcept;

enum class IndirectTargetPointerProvenanceKind : std::uint8_t
{
    Unknown,
    GuestLoad,
};

[[nodiscard]] std::string_view indirect_target_pointer_provenance_name(
    IndirectTargetPointerProvenanceKind kind) noexcept;

enum class IndirectTargetOwnership : std::uint8_t
{
    Unknown,
    TrustedExistingEntry,
    ExistingSecondaryEntry,
    InsideExistingFunction,
    ExistingDisplayEnvelopeOnly,
    CandidateOverlap,
    NewEntry,
    CrossModuleAmbiguity,
};

[[nodiscard]] std::string_view indirect_target_ownership_name(
    IndirectTargetOwnership ownership) noexcept;

enum class IndirectTargetCFGStatus : std::uint8_t
{
    NotAnalyzed,
    ExistingTrustedCFG,
    Validated,
    ValidatedWithUnresolvedFlow,
    AnalysisFailed,
    BudgetExceeded,
};

[[nodiscard]] std::string_view indirect_target_cfg_status_name(
    IndirectTargetCFGStatus status) noexcept;

enum class IndirectTargetDecisionKind : std::uint8_t
{
    TrustedExistingEntry,
    TrustedNewEntry,
    AliasOfExistingEntry,
    FunctionBoundaryConflict,
    InsideExistingFunction,
    NonExecutable,
    Unmapped,
    CrossModuleAmbiguity,
    InvalidAlignment,
    AnalysisFailed,
    InsufficientEvidence,
    BudgetExceeded,
    UnsupportedTarget,
};

[[nodiscard]] std::string_view indirect_target_decision_name(
    IndirectTargetDecisionKind decision) noexcept;

// A precise overlap is not automatically shareable. This relationship records
// the result of the boundary reconciliation proof that accompanies an
// indirect-target assessment.
enum class FunctionBoundaryReconciliationKind : std::uint8_t
{
    None,
    AnalyzerOverClaim,
    IncompatiblePreciseOverlap,
};

[[nodiscard]] std::string_view function_boundary_reconciliation_kind_name(
    FunctionBoundaryReconciliationKind kind) noexcept;

enum class FunctionBoundaryWitnessKind : std::uint8_t
{
    ExistingUnconditionalBranch,
    CandidateDirectCall,
    CandidateUnconditionalBranch,
    BoundaryCFG,
};

[[nodiscard]] std::string_view function_boundary_witness_kind_name(
    FunctionBoundaryWitnessKind kind) noexcept;

struct FunctionBoundaryWitness
{
    FunctionBoundaryWitnessKind kind = FunctionBoundaryWitnessKind::BoundaryCFG;
    memory::GuestAddress source = 0U;
    memory::GuestAddress target = 0U;

    friend bool operator==(const FunctionBoundaryWitness&, const FunctionBoundaryWitness&) =
        default;
};

struct FunctionBoundaryReconciliation
{
    FunctionBoundaryReconciliationKind kind = FunctionBoundaryReconciliationKind::None;
    std::optional<memory::GuestAddress> existing_canonical_entry;
    std::vector<GuestAddressRange> existing_owned_code_ranges_before;
    std::vector<GuestAddressRange> candidate_owned_code_ranges_before;
    std::vector<GuestAddressRange> precise_overlap_ranges;
    std::vector<GuestAddressRange> existing_owned_code_ranges_after;
    std::vector<GuestAddressRange> candidate_owned_code_ranges_after;
    std::vector<GuestAddressRange> boundary_owned_code_ranges;
    std::vector<memory::GuestAddress> boundary_entries;
    std::vector<FunctionBoundaryWitness> witnesses;
    std::optional<ControlFlowGraph> existing_cfg_before;
    std::optional<ControlFlowGraph> candidate_cfg_before;
    std::optional<ControlFlowGraph> boundary_cfg;
    std::string reason;
};

// Function-entry evidence is deliberately richer than the legacy discovery
// source/confidence pair.  It records both the semantic kind of proof and the
// exact guest-side object that supplied it; no host pointer is representable.
enum class FunctionEntryEvidenceKind : std::uint8_t
{
    SymbolFunction,
    DynamicSymbolFunction,
    DirectCallTarget,
    RelocationFunctionTarget,
    RelocatedFunctionPointer,
    ExportedFunction,
    KnownProviderEntry,
    BoundedCfgCandidate,
    ObservedIndirectCall,
    GuestLoadedPointer,
};

[[nodiscard]] std::string_view function_entry_evidence_kind_name(
    FunctionEntryEvidenceKind kind) noexcept;

enum class FunctionEntryEvidenceStrength : std::uint8_t
{
    Exact,
    Supporting,
};

[[nodiscard]] std::string_view function_entry_evidence_strength_name(
    FunctionEntryEvidenceStrength strength) noexcept;

struct FunctionEntryEvidence
{
    FunctionEntryEvidenceKind kind = FunctionEntryEvidenceKind::BoundedCfgCandidate;
    FunctionEntryEvidenceStrength strength = FunctionEntryEvidenceStrength::Supporting;
    std::string source_module;
    std::optional<memory::GuestAddress> source_address;
    std::string target_module;
    memory::GuestAddress target = 0U;
    std::optional<std::size_t> relocation_index;
    std::optional<format::AArch64RelocationType> relocation_type;
    std::optional<format::RelocationSource> relocation_source;
    std::optional<std::uint32_t> symbol_index;
    std::string symbol_name;
    bool target_declared_function = false;
    bool slot_value_verified = false;
    std::string detail;

    friend bool operator==(const FunctionEntryEvidence&, const FunctionEntryEvidence&) = default;
};

enum class FunctionEntryEvidenceRejectionKind : std::uint8_t
{
    NotFunctionMetadata,
    ProvenanceMismatch,
    InvalidRelocation,
    InsufficientEvidence,
};

[[nodiscard]] std::string_view function_entry_evidence_rejection_name(
    FunctionEntryEvidenceRejectionKind kind) noexcept;

struct FunctionEntryEvidenceRejection
{
    FunctionEntryEvidenceRejectionKind kind = FunctionEntryEvidenceRejectionKind::InsufficientEvidence;
    FunctionEntryEvidenceKind evidence_kind = FunctionEntryEvidenceKind::BoundedCfgCandidate;
    std::string source_module;
    std::optional<memory::GuestAddress> source_address;
    std::string detail;
};

struct RelocationFunctionPointerProvenance
{
    std::string source_module;
    memory::GuestAddress source_slot = 0U;
    std::size_t relocation_index = 0U;
    format::Relocation relocation{};
    std::optional<memory::GuestAddress> resolved_target;
    std::string target_module;
    std::optional<std::uint32_t> symbol_index;
    std::string symbol_name;
    bool target_declared_function = false;
    bool slot_value_verified = false;
    std::optional<std::string> resolution_error;
};

// Resolves only loader-visible guest relocation provenance for one exact
// storage slot. An empty result means the slot has no recorded relocation;
// it never means that an executable value is a function pointer.
[[nodiscard]] Result<std::vector<RelocationFunctionPointerProvenance>>
inspect_relocation_function_pointer_provenance(const ProcessImage& process_image,
                                               memory::GuestAddress source_slot,
                                               memory::GuestAddress expected_target);

enum class FunctionCertificationStatus : std::uint8_t
{
    Certified,
    InsufficientEvidence,
    NotExecutable,
    Misaligned,
    OutsideKnownModule,
    ConflictsWithExistingFunction,
    OverlapsExistingFunction,
    DecodeFailed,
    CFGIncomplete,
    AmbiguousOwnership,
    BoundsExceeded,
    InvalidProvenance,
};

[[nodiscard]] std::string_view function_certification_status_name(
    FunctionCertificationStatus status) noexcept;

struct FunctionCandidate
{
    std::string module;
    memory::GuestAddress entry = 0U;
    std::vector<GuestAddressRange> owned_code_ranges;
    std::optional<ControlFlowGraph> cfg;
    std::vector<FunctionEntryEvidence> evidence;
};

struct FunctionCertificationResult
{
    FunctionCertificationStatus status = FunctionCertificationStatus::InsufficientEvidence;
    bool certified = false;
    FunctionConfidence confidence = FunctionConfidence::Low;
    std::string reason;
    std::vector<FunctionEntryEvidence> accepted_evidence;
    std::vector<FunctionEntryEvidenceRejection> rejected_evidence;
};

struct IndirectTargetStaticEvidence
{
    FunctionDiscoverySource source = FunctionDiscoverySource::Heuristic;
    FunctionConfidence confidence = FunctionConfidence::Low;
    std::string detail;
    FunctionEntryEvidenceKind kind = FunctionEntryEvidenceKind::BoundedCfgCandidate;
    FunctionEntryEvidenceStrength strength = FunctionEntryEvidenceStrength::Supporting;

    friend bool operator==(const IndirectTargetStaticEvidence&,
                           const IndirectTargetStaticEvidence&) = default;
};

struct ObservedIndirectTarget
{
    std::string source_module;
    memory::GuestAddress source_function = 0U;
    memory::GuestAddress source_pc = 0U;
    IndirectControlFlowKind control_flow = IndirectControlFlowKind::Branch;
    std::string target_register;
    memory::GuestAddress target = 0U;
    std::string target_module;
    IndirectTargetPointerProvenanceKind pointer_provenance =
        IndirectTargetPointerProvenanceKind::Unknown;
    std::optional<memory::GuestAddress> guest_load_address;
    std::vector<IndirectTargetStaticEvidence> static_evidence;
    std::vector<FunctionEntryEvidence> entry_evidence;
    std::size_t observation_count = 1U;

    // The representative observation remains deterministic and is used for
    // certification.  This history retains the guest-side provenance of
    // coalesced observations without making host identity part of the model.
    struct Provenance
    {
        std::string source_module;
        memory::GuestAddress source_function = 0U;
        memory::GuestAddress source_pc = 0U;
        IndirectControlFlowKind control_flow = IndirectControlFlowKind::Branch;
        std::string target_register;
        IndirectTargetPointerProvenanceKind pointer_provenance =
            IndirectTargetPointerProvenanceKind::Unknown;
        std::optional<memory::GuestAddress> guest_load_address;

        friend bool operator==(const Provenance&, const Provenance&) = default;
    };
    std::vector<Provenance> observation_provenance;

    friend bool operator==(const ObservedIndirectTarget&, const ObservedIndirectTarget&) = default;
};

struct IndirectTargetCandidateIdentity
{
    std::string target_module;
    memory::GuestAddress target = 0U;
    std::string source_module;
    memory::GuestAddress source_function = 0U;
    memory::GuestAddress source_pc = 0U;
    IndirectControlFlowKind control_flow = IndirectControlFlowKind::Branch;
    std::string target_register;
    IndirectTargetPointerProvenanceKind pointer_provenance =
        IndirectTargetPointerProvenanceKind::Unknown;
    std::optional<memory::GuestAddress> guest_load_address;

    friend bool operator==(const IndirectTargetCandidateIdentity&,
                           const IndirectTargetCandidateIdentity&) = default;
    friend bool operator<(const IndirectTargetCandidateIdentity& left,
                          const IndirectTargetCandidateIdentity& right) noexcept
    {
        if (left.target_module != right.target_module)
            return left.target_module < right.target_module;
        if (left.target != right.target) return left.target < right.target;
        return std::tie(left.source_module, left.source_function, left.source_pc,
                        left.control_flow, left.target_register, left.pointer_provenance,
                        left.guest_load_address) <
               std::tie(right.source_module, right.source_function, right.source_pc,
                        right.control_flow, right.target_register, right.pointer_provenance,
                        right.guest_load_address);
    }
};

[[nodiscard]] IndirectTargetCandidateIdentity indirect_target_candidate_identity(
    const ObservedIndirectTarget& observed);

// The current outer worklist charges one persistent candidate record per
// target-module/address pair.  This universe is the finite set of aligned
// instruction slots in the immutable executable image.  The representation
// is a count, not an eagerly materialized address set.
struct IndirectTargetCandidateUniverse
{
    std::size_t executable_instruction_slots = std::numeric_limits<std::size_t>::max() / 4U;
    AnalysisBudgetProvenance provenance{
        AnalysisBudgetProvenanceKind::DerivedStructuralBound,
        "aarch64_guest_address_domain"};
};

[[nodiscard]] Result<IndirectTargetCandidateUniverse>
derive_indirect_target_candidate_universe(std::span<const ModuleIdentity> modules);

[[nodiscard]] Result<IndirectTargetCandidateUniverse>
derive_indirect_target_candidate_universe(const ProcessImage& process_image);

enum class IndirectTargetRefinementAnalysisDimension : std::uint8_t
{
    None,
    FunctionsAnalyzed,
    FunctionsReanalyzed,
    Instructions,
    Blocks,
    Edges,
    BytesAnalyzed,
    BoundaryFinalizationPasses,
    InvalidatedRecords,
    Transactions,
};

[[nodiscard]] std::string_view indirect_target_refinement_analysis_dimension_name(
    IndirectTargetRefinementAnalysisDimension dimension) noexcept;

struct IndirectTargetRefinementAnalysisBudgets
{
    // Cumulative immutable-generation work limits. These are not successful
    // promotion or map-event quotas.
    std::size_t max_functions_analyzed = 200'000U;
    std::size_t max_functions_reanalyzed = 100'000U;
    std::size_t max_instructions = 8'000'000U;
    std::size_t max_blocks = 2'000'000U;
    std::size_t max_edges = 4'000'000U;
    memory::GuestSize max_bytes_analyzed = memory::GuestSize{256U} * 1024U * 1024U;
    std::size_t max_boundary_finalization_passes = 2'048U;
    std::size_t max_invalidated_records = 100'000U;
    // Reusable-map transactions remain auditable accounting, but ordinary
    // termination is provided by the semantic aggregate resources below.
    // An engaged value is an explicit finite compatibility/debug ceiling.
    std::optional<std::size_t> max_transactions = std::nullopt;

    AnalysisBudgetProvenance functions_analyzed_provenance{
        AnalysisBudgetProvenanceKind::LibraryDefault, "refinement_analysis_default"};
    AnalysisBudgetProvenance functions_reanalyzed_provenance{
        AnalysisBudgetProvenanceKind::LibraryDefault, "refinement_analysis_default"};
    AnalysisBudgetProvenance instructions_provenance{
        AnalysisBudgetProvenanceKind::LibraryDefault, "refinement_analysis_default"};
    AnalysisBudgetProvenance blocks_provenance{
        AnalysisBudgetProvenanceKind::LibraryDefault, "refinement_analysis_default"};
    AnalysisBudgetProvenance edges_provenance{
        AnalysisBudgetProvenanceKind::LibraryDefault, "refinement_analysis_default"};
    AnalysisBudgetProvenance bytes_provenance{
        AnalysisBudgetProvenanceKind::LibraryDefault, "refinement_analysis_default"};
    AnalysisBudgetProvenance boundary_finalization_provenance{
        AnalysisBudgetProvenanceKind::LibraryDefault, "refinement_analysis_default"};
    AnalysisBudgetProvenance invalidated_records_provenance{
        AnalysisBudgetProvenanceKind::LibraryDefault, "refinement_analysis_default"};
    AnalysisBudgetProvenance transactions_provenance{
        AnalysisBudgetProvenanceKind::LibraryDefault, "not_configured"};
};

struct IndirectTargetRefinementAnalysisWork
{
    std::string module;
    std::size_t functions_analyzed = 0U;
    std::size_t functions_reanalyzed = 0U;
    std::size_t functions_reused = 0U;
    std::size_t instructions = 0U;
    std::size_t blocks = 0U;
    std::size_t edges = 0U;
    memory::GuestSize bytes_analyzed = 0U;
    std::size_t boundary_finalization_passes = 0U;
    std::size_t invalidated_records = 0U;
    std::size_t transactions = 0U;
};

struct IndirectTargetRefinementAnalysisSummary
{
    IndirectTargetRefinementAnalysisBudgets configured;
    std::size_t functions_analyzed = 0U;
    std::size_t functions_reanalyzed = 0U;
    std::size_t functions_reused = 0U;
    std::size_t instructions = 0U;
    std::size_t blocks = 0U;
    std::size_t edges = 0U;
    memory::GuestSize bytes_analyzed = 0U;
    std::size_t boundary_finalization_passes = 0U;
    std::size_t invalidated_records = 0U;
    std::size_t transactions = 0U;
};

enum class IndirectTargetRefinementBudgetDimension : std::uint8_t
{
    None,
    StagnantRounds,
    CounterOverflow,
    UniqueCandidates,
    StructuralCandidateUniverse,
    CandidateAssessments,
    Promotions,
    MapRebuilds,
    RefinementAnalysisFunctions,
    RefinementAnalysisReanalyzedFunctions,
    RefinementAnalysisInstructions,
    RefinementAnalysisBlocks,
    RefinementAnalysisEdges,
    RefinementAnalysisBytes,
    RefinementAnalysisBoundaryFinalizationPasses,
    RefinementAnalysisInvalidatedRecords,
    RefinementAnalysisTransactions,
};

[[nodiscard]] std::string_view indirect_target_refinement_budget_dimension_name(
    IndirectTargetRefinementBudgetDimension dimension) noexcept;

struct IndirectTargetRefinementBudgets
{
    // These are outer discovery budgets.  CFG/function budgets remain owned
    // by AnalysisBudgets and are enforced independently for every immutable
    // map construction.
    // Productive execution attempts are bounded by the dimensions below.
    // This budget applies only to consecutive attempts which make no
    // monotonic worklist transition.
    std::size_t max_stagnant_rounds = 64U;
    // Optional pre-M28 compatibility guard. Ordinary operation derives the
    // candidate bound from candidate_universe instead of using a fixed count.
    std::optional<std::size_t> max_unique_candidates;
    AnalysisBudgetProvenance candidate_limit_provenance{
        AnalysisBudgetProvenanceKind::LibraryDefault, "not_configured"};
    // Optional pre-M32 compatibility guard. Ordinary operation accounts for
    // first assessments and generation-dependent reassessments directly from
    // candidate/generation state instead of using a historical event count.
    std::optional<std::size_t> max_candidate_assessments;
    AnalysisBudgetProvenance candidate_assessment_limit_provenance{
        AnalysisBudgetProvenanceKind::LibraryDefault, "not_configured"};
    // Retained only for explicit pre-M27 compatibility. Ordinary defaults do
    // not charge valid monotonic work to these successful-event counts.
    std::size_t max_promotions = 128U;
    std::size_t max_map_rebuilds = 128U;
    bool legacy_event_limits = false;
    IndirectTargetCandidateUniverse candidate_universe;
    IndirectTargetRefinementAnalysisBudgets analysis;
};

struct IndirectTargetRefinementExhaustion
{
    IndirectTargetRefinementBudgetDimension dimension =
        IndirectTargetRefinementBudgetDimension::None;
    std::size_t consumed = 0U;
    std::size_t limit = 0U;
    std::string module;
    std::size_t generation = 0U;
    std::optional<IndirectTargetCandidateIdentity> next_work;
};

struct IndirectTargetRefinementSummary
{
    IndirectTargetRefinementBudgets configured;
    std::size_t total_execution_attempts = 0U;
    std::size_t productive_rounds = 0U;
    std::size_t stagnant_rounds = 0U;
    std::size_t observations_received = 0U;
    std::size_t unique_observations = 0U;
    std::size_t unique_candidates = 0U;
    std::size_t candidate_records = 0U;
    std::size_t structurally_ineligible_candidates = 0U;
    std::size_t candidate_assessments = 0U;
    std::size_t first_candidate_assessments = 0U;
    std::size_t generation_reassessments = 0U;
    std::size_t same_generation_assessment_attempts = 0U;
    std::size_t failed_refinements = 0U;
    std::size_t rollback_assessments = 0U;
    std::size_t terminal_resolutions = 0U;
    std::size_t successful_promotions = 0U;
    std::size_t existing_trusted_hits = 0U;
    std::size_t duplicate_coalesced_observations = 0U;
    std::size_t rejected_candidates = 0U;
    std::size_t boundary_reconciliations = 0U;
    std::size_t map_rebuilds = 0U;
    std::size_t module_maps_rebuilt = 0U;
    std::size_t module_maps_reused = 0U;
    std::size_t candidates_reconsidered_after_map_change = 0U;
    std::size_t refinement_batches = 0U;
    std::size_t batch_candidates = 0U;
    std::size_t singleton_batches = 0U;
    std::size_t rebuilds_avoided = 0U;
    std::size_t max_batch_width = 0U;
    std::size_t finalized_functions_reused = 0U;
    std::size_t cfgs_reused = 0U;
    std::size_t functions_rebuilt = 0U;
    std::size_t modules_touched = 0U;
    std::size_t incremental_updates = 0U;
    std::size_t full_rebuilds = 0U;
    std::size_t map_generation = 0U;
    std::size_t pending_candidate_count = 0U;
    IndirectTargetRefinementAnalysisSummary analysis;
    std::optional<IndirectTargetCandidateIdentity> last_assessed_candidate;
    std::optional<std::size_t> last_assessment_generation;
    std::optional<IndirectTargetCandidateIdentity> last_processed_candidate;
    std::optional<IndirectTargetCandidateIdentity> next_pending_candidate;
    IndirectTargetRefinementExhaustion exhaustion;
};

struct IndirectTargetRefinementObservationResult
{
    bool accepted = true;
    bool newly_unique_observation = false;
    bool newly_unique_candidate = false;
    bool reconsidered_after_map_change = false;
};

struct IndirectTargetAssessment;

// Deterministic, finite outer-discovery accounting.  The worklist stores one
// sparse logical work item per target module/address.  The full source,
// control-flow, register, and pointer-provenance identity remains in the
// representative observation/history, so coalescing records does not discard
// guest-side evidence.  The structural candidate universe therefore bounds
// both unique target records and persistent candidate storage.
class IndirectTargetRefinementWorklist
{
  public:
    explicit IndirectTargetRefinementWorklist(
        IndirectTargetRefinementBudgets budgets = {});

    [[nodiscard]] IndirectTargetRefinementObservationResult observe(
        const IndirectTargetAssessment& assessment);
    [[nodiscard]] bool begin_round() noexcept;
    void end_round() noexcept;
    [[nodiscard]] bool begin_candidate_assessment(
        const IndirectTargetCandidateIdentity& candidate) noexcept;
    [[nodiscard]] bool can_promote() noexcept;
    [[nodiscard]] bool can_commit_refinement(
        const IndirectTargetCandidateIdentity& candidate,
        const IndirectTargetRefinementAnalysisWork& work,
        std::size_t module_maps_rebuilt = 0U,
        std::size_t module_maps_reused = 0U) noexcept;
    [[nodiscard]] bool can_commit_batch_refinement(
        std::span<const IndirectTargetCandidateIdentity> candidates,
        const IndirectTargetRefinementAnalysisWork& work,
        std::size_t module_maps_rebuilt = 0U,
        std::size_t module_maps_reused = 0U) noexcept;
    void record_terminal_candidate(const IndirectTargetCandidateIdentity& candidate) noexcept;
    void record_failed_refinement(const IndirectTargetCandidateIdentity& candidate) noexcept;
    void record_rollback_assessment(const IndirectTargetCandidateIdentity& candidate) noexcept;
    void record_promotion(const IndirectTargetCandidateIdentity& candidate,
                          std::size_t module_maps_rebuilt = 0U,
                          std::size_t module_maps_reused = 0U,
                          const IndirectTargetRefinementAnalysisWork& work = {},
                          std::size_t finalized_functions_reused = 0U,
                          std::size_t cfgs_reused = 0U,
                          std::size_t functions_rebuilt = 0U,
                          std::size_t modules_touched = 0U,
                          std::size_t incremental_updates = 1U,
                          std::size_t full_rebuilds = 0U) noexcept;
    void record_batch_promotion(
        std::span<const IndirectTargetCandidateIdentity> candidates,
        std::size_t module_maps_rebuilt = 0U,
        std::size_t module_maps_reused = 0U,
        const IndirectTargetRefinementAnalysisWork& work = {},
        std::size_t finalized_functions_reused = 0U,
        std::size_t cfgs_reused = 0U,
        std::size_t functions_rebuilt = 0U,
        std::size_t modules_touched = 0U,
        std::size_t incremental_updates = 1U,
        std::size_t full_rebuilds = 0U) noexcept;

    [[nodiscard]] std::vector<ObservedIndirectTarget> pending_candidates() const;
    [[nodiscard]] IndirectTargetRefinementSummary summary() const;
    [[nodiscard]] bool exhausted() const noexcept;

  private:
    struct WorkItem
    {
        ObservedIndirectTarget observed;
        bool pending = false;
        bool processed = false;
        bool promoted = false;
        std::size_t processed_generation = 0U;
        std::optional<std::size_t> last_assessed_generation;
    };

    using TargetIdentity = std::pair<std::string, memory::GuestAddress>;

    [[nodiscard]] std::size_t candidate_limit() const noexcept;
    [[nodiscard]] bool increment_counter(
        std::size_t& counter, IndirectTargetRefinementBudgetDimension dimension,
        std::string module = {},
        std::optional<IndirectTargetCandidateIdentity> next_work = std::nullopt) noexcept;
    [[nodiscard]] bool add_counter(
        std::size_t& counter, std::size_t delta,
        IndirectTargetRefinementBudgetDimension dimension, std::string module = {},
        std::optional<IndirectTargetCandidateIdentity> next_work = std::nullopt) noexcept;
    [[nodiscard]] std::vector<ObservedIndirectTarget> all_pending_candidates() const;

    IndirectTargetRefinementBudgets budgets_;
    IndirectTargetRefinementSummary counters_;
    std::map<TargetIdentity, WorkItem> work_items_;
    std::map<std::string, bool> unique_observations_;
    std::size_t map_generation_ = 0U;
    std::optional<IndirectTargetCandidateIdentity> overflow_pending_;
    bool round_active_ = false;
    bool round_productive_ = false;
};

struct IndirectTargetValidation
{
    bool nonzero = false;
    bool aligned = false;
    bool mapped = false;
    bool executable = false;
    bool unique_module_owner = false;
    // This is an immutable process-image eligibility result only. It is not
    // certification and never authorizes promotion or guest dispatch.
    bool structurally_eligible = false;
    std::string target_module;
    std::optional<memory::GuestAddress> target_module_base;
    IndirectTargetOwnership ownership = IndirectTargetOwnership::Unknown;
    std::optional<memory::GuestAddress> existing_canonical_entry;
    std::vector<GuestAddressRange> candidate_owned_code_ranges;
    std::vector<GuestAddressRange> overlap_ranges;
    FunctionBoundaryReconciliation boundary_reconciliation;
    std::optional<ControlFlowGraph> cfg;
    IndirectTargetCFGStatus cfg_status = IndirectTargetCFGStatus::NotAnalyzed;
    std::size_t blocks = 0U;
    std::size_t instructions = 0U;
    std::size_t edges = 0U;
    std::vector<memory::GuestAddress> direct_call_targets;
    std::vector<UnresolvedControlFlow> unresolved_control_flow;
    std::optional<Error> analysis_error;
    std::optional<memory::GuestAddress> pointer_slot;
    std::string pointer_slot_module;
    bool pointer_slot_relocation_found = false;
    bool pointer_slot_value_verified = false;
    bool pointer_target_declared_function = false;
    std::vector<RelocationFunctionPointerProvenance> relocation_provenance;
};

struct IndirectTargetDecision
{
    IndirectTargetDecisionKind kind = IndirectTargetDecisionKind::UnsupportedTarget;
    FunctionConfidence confidence = FunctionConfidence::Low;
    bool eligible_for_promotion = false;
    bool promoted = false;
    std::optional<memory::GuestAddress> canonical_entry;
    std::string reason;
};

struct IndirectTargetAssessment
{
    ObservedIndirectTarget observed;
    std::vector<IndirectTargetStaticEvidence> static_evidence;
    std::vector<FunctionEntryEvidence> entry_evidence;
    std::vector<FunctionEntryEvidenceRejection> rejected_evidence;
    IndirectTargetValidation validation;
    IndirectTargetDecision decision;
    FunctionCandidate candidate;
    FunctionCertificationResult certification;
    bool guest_code_entered = false;
    std::optional<memory::GuestAddress> first_guest_pc;
    std::optional<std::uint32_t> first_guest_opcode;
    std::optional<memory::GuestAddress> next_guest_pc;
    // Filled by the outer immutable refinement driver for a successful
    // promotion.  Zero is the initial frozen process-map generation.
    std::size_t map_generation_before = 0U;
    std::size_t map_generation_after = 0U;
};

struct IndirectTargetDiscoveryOptions
{
    AnalysisBudgets budgets;
    AnalysisOptions cfg;
    bool allow_runtime_cfg_promotion = true;
    bool require_independent_static_evidence = false;
};

struct FunctionMapRefinement
{
    FinalizedFunctionMap map;
    IndirectTargetAssessment assessment;
    IndirectTargetRefinementAnalysisWork analysis_work;
};

struct ProcessFunctionMapRefinement
{
    ProcessFunctionMap map;
    IndirectTargetAssessment assessment;
    std::size_t module_maps_rebuilt = 0U;
    std::size_t module_maps_reused = 0U;
    IndirectTargetRefinementAnalysisWork analysis_work;
    std::size_t finalized_functions_reused = 0U;
    std::size_t cfgs_reused = 0U;
    std::size_t functions_rebuilt = 0U;
    std::size_t modules_touched = 0U;
    std::size_t incremental_updates = 1U;
    std::size_t full_rebuilds = 0U;
};

struct ProcessFunctionMapBatchRefinement
{
    ProcessFunctionMap map;
    std::vector<IndirectTargetAssessment> assessments;
    std::size_t module_maps_rebuilt = 0U;
    std::size_t module_maps_reused = 0U;
    IndirectTargetRefinementAnalysisWork analysis_work;
    std::size_t finalized_functions_reused = 0U;
    std::size_t cfgs_reused = 0U;
    std::size_t functions_rebuilt = 0U;
    std::size_t modules_touched = 0U;
    std::size_t incremental_updates = 1U;
    std::size_t full_rebuilds = 0U;
    bool all_promoted = false;
};

[[nodiscard]] Result<IndirectTargetAssessment> assess_indirect_target(
    const ObservedIndirectTarget& observed, const memory::GuestMemory& memory,
    const FinalizedFunctionMap* function_map = nullptr,
    const ProcessFunctionMap* process_function_map = nullptr,
    const ProcessImage* process_image = nullptr,
    const IndirectTargetDiscoveryOptions& options = {});

// This operation never mutates the supplied frozen map. A successful
// promotion returns a newly built, independently validated finalized map.
[[nodiscard]] Result<FunctionMapRefinement> refine_function_map(
    const FinalizedFunctionMap& existing, const ModuleAnalysisInput& input,
    const ObservedIndirectTarget& observed,
    const IndirectTargetDiscoveryOptions& options = {});

// Process refinement is the corresponding immutable expansion operation for
// multi-module execution. It rebuilds from the ProcessImage's deterministic
// module seeds and adds the observed target only to its owning module.
[[nodiscard]] Result<ProcessFunctionMapRefinement> refine_process_function_map(
    const ProcessFunctionMap& existing, const ProcessImage& process_image,
    const ObservedIndirectTarget& observed,
    const IndirectTargetDiscoveryOptions& options = {});

// Assessments in a batch are made against one immutable input generation. The
// coordinator must only pass structurally independent candidates; the helper
// builds each touched module once and publishes one deterministic map.
[[nodiscard]] Result<ProcessFunctionMapBatchRefinement> refine_process_function_map_batch(
    const ProcessFunctionMap& existing, const ProcessImage& process_image,
    std::span<const IndirectTargetAssessment> assessments,
    const IndirectTargetDiscoveryOptions& options = {});

void sort_observed_indirect_targets(std::vector<ObservedIndirectTarget>& targets);

} // namespace switchrecomp::analysis
