#pragma once

#include "switchrecomp/analysis/function_map.hpp"
#include "switchrecomp/analysis/process_image.hpp"
#include "switchrecomp/format/elf_rela.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
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

    friend bool operator==(const ObservedIndirectTarget&, const ObservedIndirectTarget&) = default;
};

struct IndirectTargetValidation
{
    bool nonzero = false;
    bool aligned = false;
    bool mapped = false;
    bool executable = false;
    bool unique_module_owner = false;
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
};

struct ProcessFunctionMapRefinement
{
    ProcessFunctionMap map;
    IndirectTargetAssessment assessment;
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

void sort_observed_indirect_targets(std::vector<ObservedIndirectTarget>& targets);

} // namespace switchrecomp::analysis
