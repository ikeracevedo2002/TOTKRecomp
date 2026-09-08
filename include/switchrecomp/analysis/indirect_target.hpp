#pragma once

#include "switchrecomp/analysis/function_map.hpp"
#include "switchrecomp/analysis/process_image.hpp"

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

struct IndirectTargetStaticEvidence
{
    FunctionDiscoverySource source = FunctionDiscoverySource::Heuristic;
    FunctionConfidence confidence = FunctionConfidence::Low;
    std::string detail;

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
    std::optional<ControlFlowGraph> cfg;
    IndirectTargetCFGStatus cfg_status = IndirectTargetCFGStatus::NotAnalyzed;
    std::size_t blocks = 0U;
    std::size_t instructions = 0U;
    std::size_t edges = 0U;
    std::vector<memory::GuestAddress> direct_call_targets;
    std::vector<UnresolvedControlFlow> unresolved_control_flow;
    std::optional<Error> analysis_error;
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
    IndirectTargetValidation validation;
    IndirectTargetDecision decision;
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
