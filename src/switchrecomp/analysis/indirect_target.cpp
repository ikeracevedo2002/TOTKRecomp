#include "switchrecomp/analysis/indirect_target.hpp"

#include "switchrecomp/common/checked_arithmetic.hpp"
#include "switchrecomp/loader/nso_guest_loader.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

namespace switchrecomp::analysis
{

namespace
{

using GuestAddress = memory::GuestAddress;

[[nodiscard]] std::string hex_address(GuestAddress address)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::setw(16) << std::setfill('0') << address;
    return output.str();
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

[[nodiscard]] bool is_budget_error(ErrorCode code) noexcept
{
    switch (code)
    {
    case ErrorCode::AnalysisBudgetExceeded:
    case ErrorCode::AnalysisInstructionLimitExceeded:
    case ErrorCode::AnalysisBlockLimitExceeded:
    case ErrorCode::AnalysisWorklistLimitExceeded:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool is_supported_relocation_type(format::AArch64RelocationType type) noexcept
{
    return type != format::AArch64RelocationType::None &&
           type != format::AArch64RelocationType::Unknown;
}

[[nodiscard]] bool checked_add_size(std::size_t left, std::size_t right,
                                    std::size_t& result) noexcept
{
    if (right > std::numeric_limits<std::size_t>::max() - left) return false;
    result = left + right;
    return true;
}

[[nodiscard]] ObservedIndirectTarget::Provenance observation_provenance(
    const ObservedIndirectTarget& observed)
{
    return ObservedIndirectTarget::Provenance{
        observed.source_module,
        observed.source_function,
        observed.source_pc,
        observed.control_flow,
        observed.target_register,
        observed.pointer_provenance,
        observed.guest_load_address};
}

[[nodiscard]] std::string observation_identity_key(const ObservedIndirectTarget& observed)
{
    std::ostringstream output;
    const auto append_string = [&](std::string_view value) {
        output << value.size() << ':' << value;
    };
    append_string(observed.source_module);
    output << observed.source_function << ':' << observed.source_pc << ':'
           << static_cast<unsigned int>(observed.control_flow) << ':';
    append_string(observed.target_register);
    output << observed.target << ':';
    append_string(observed.target_module);
    output << static_cast<unsigned int>(observed.pointer_provenance) << ':';
    if (observed.guest_load_address)
        output << '1' << ':' << observed.guest_load_address.value();
    else
        output << '0' << ':';
    return output.str();
}

[[nodiscard]] bool observed_order_less(const ObservedIndirectTarget& left,
                                       const ObservedIndirectTarget& right) noexcept
{
    return std::tie(left.target_module, left.target, left.source_module, left.source_function,
                    left.source_pc, left.control_flow, left.target_register,
                    left.pointer_provenance, left.guest_load_address) <
           std::tie(right.target_module, right.target, right.source_module,
                    right.source_function, right.source_pc, right.control_flow,
                    right.target_register, right.pointer_provenance,
                    right.guest_load_address);
}

void add_observation_provenance(std::vector<ObservedIndirectTarget::Provenance>& history,
                                const ObservedIndirectTarget::Provenance& item)
{
    if (std::find(history.begin(), history.end(), item) == history.end()) history.push_back(item);
    std::sort(history.begin(), history.end(), [](const auto& left, const auto& right) {
        return std::tie(left.source_module, left.source_function, left.source_pc,
                        left.control_flow, left.target_register, left.pointer_provenance,
                        left.guest_load_address) <
               std::tie(right.source_module, right.source_function, right.source_pc,
                        right.control_flow, right.target_register, right.pointer_provenance,
                        right.guest_load_address);
    });
}

void add_evidence(std::vector<IndirectTargetStaticEvidence>& evidence,
                  IndirectTargetStaticEvidence item)
{
    if (item.detail.empty()) return;
    const auto duplicate = std::find(evidence.begin(), evidence.end(), item);
    if (duplicate == evidence.end()) evidence.push_back(std::move(item));
}

void normalize_evidence(std::vector<IndirectTargetStaticEvidence>& evidence)
{
    std::sort(evidence.begin(), evidence.end(), [](const auto& left, const auto& right) {
        return std::tie(left.source, left.confidence, left.detail) <
               std::tie(right.source, right.confidence, right.detail);
    });
    evidence.erase(std::unique(evidence.begin(), evidence.end()), evidence.end());
}

void add_entry_evidence(std::vector<FunctionEntryEvidence>& evidence,
                        FunctionEntryEvidence item)
{
    if (item.detail.empty()) return;
    if (std::find(evidence.begin(), evidence.end(), item) == evidence.end())
        evidence.push_back(std::move(item));
}

void normalize_entry_evidence(std::vector<FunctionEntryEvidence>& evidence)
{
    std::sort(evidence.begin(), evidence.end(), [](const auto& left, const auto& right) {
        return std::tie(left.kind, left.strength, left.source_module, left.source_address,
                        left.target_module, left.target, left.relocation_index,
                        left.relocation_type, left.relocation_source, left.symbol_index,
                        left.symbol_name, left.target_declared_function, left.slot_value_verified,
                        left.detail) <
               std::tie(right.kind, right.strength, right.source_module, right.source_address,
                        right.target_module, right.target, right.relocation_index,
                        right.relocation_type, right.relocation_source, right.symbol_index,
                        right.symbol_name, right.target_declared_function, right.slot_value_verified,
                        right.detail);
    });
    evidence.erase(std::unique(evidence.begin(), evidence.end()), evidence.end());
}

void add_rejection(std::vector<FunctionEntryEvidenceRejection>& rejections,
                   FunctionEntryEvidenceRejection item)
{
    if (item.detail.empty()) return;
    const auto duplicate = std::find_if(rejections.begin(), rejections.end(),
                                        [&](const auto& existing) {
                                            return existing.kind == item.kind &&
                                                   existing.evidence_kind == item.evidence_kind &&
                                                   existing.source_module == item.source_module &&
                                                   existing.source_address == item.source_address &&
                                                   existing.detail == item.detail;
                                        });
    if (duplicate == rejections.end()) rejections.push_back(std::move(item));
}

void normalize_rejections(std::vector<FunctionEntryEvidenceRejection>& rejections)
{
    std::sort(rejections.begin(), rejections.end(), [](const auto& left, const auto& right) {
        return std::tie(left.kind, left.evidence_kind, left.source_module, left.source_address,
                        left.detail) <
               std::tie(right.kind, right.evidence_kind, right.source_module,
                        right.source_address, right.detail);
    });
    rejections.erase(std::unique(rejections.begin(), rejections.end(),
                                 [](const auto& left, const auto& right) {
                                     return left.kind == right.kind &&
                                            left.evidence_kind == right.evidence_kind &&
                                            left.source_module == right.source_module &&
                                            left.source_address == right.source_address &&
                                            left.detail == right.detail;
                                 }),
                     rejections.end());
}

[[nodiscard]] std::optional<GuestAddress> symbol_address(const format::DynamicSymbol& symbol,
                                                          GuestAddress base)
{
    constexpr std::uint16_t shn_abs = 0xfff1U;
    if (symbol.section_index == shn_abs) return symbol.value;
    const auto address = checked_add_u64(base, symbol.value);
    return address ? std::optional<GuestAddress>(address.value()) : std::nullopt;
}

void collect_process_static_evidence(const ObservedIndirectTarget& observed,
                                     const ProcessImage* process_image,
                                     bool include_target_static_evidence,
                                     std::vector<IndirectTargetStaticEvidence>& evidence,
                                     std::vector<FunctionEntryEvidence>& entry_evidence,
                                     std::vector<FunctionEntryEvidenceRejection>& rejections,
                                     IndirectTargetValidation& validation)
{
    if (process_image == nullptr) return;
    if (include_target_static_evidence)
    {
        for (const auto& reference : process_image->function_target_references(observed.target))
        {
            const auto* module = process_image->module(reference.module);
            if (module == nullptr || !module->symbols) continue;
            const auto* symbol = module->symbols->at(reference.symbol_index);
            if (symbol == nullptr) continue;
            if (!reference.relocation_index)
            {
                add_evidence(evidence, IndirectTargetStaticEvidence{
                                              FunctionDiscoverySource::DynamicSymbol,
                                              FunctionConfidence::Confirmed,
                                              "defined dynamic function symbol " +
                                                  std::to_string(symbol->index) +
                                                  (symbol->name.empty() ? std::string{}
                                                                        : ":" + symbol->name),
                                              FunctionEntryEvidenceKind::DynamicSymbolFunction,
                                              FunctionEntryEvidenceStrength::Exact});
                add_entry_evidence(entry_evidence, FunctionEntryEvidence{
                                                               FunctionEntryEvidenceKind::DynamicSymbolFunction,
                                                               FunctionEntryEvidenceStrength::Exact,
                                                               module->identity.module, std::nullopt,
                                                               module->identity.module, observed.target,
                                                               std::nullopt, std::nullopt, std::nullopt,
                                                               symbol->index, symbol->name, true, false,
                                                               "defined dynamic function symbol"});
                continue;
            }

            if (reference.relocation_index.value() >= module->relocations.size()) continue;
            const auto& relocation = module->relocations[reference.relocation_index.value()];
            add_evidence(evidence, IndirectTargetStaticEvidence{
                                          FunctionDiscoverySource::RelocationReference,
                                          FunctionConfidence::High,
                                          "relocation-backed function pointer at " +
                                              hex_address(relocation.target_address) + ", type=" +
                                              std::string(format::aarch64_relocation_type_name(
                                                  relocation.type)) +
                                              ", symbol=" + std::to_string(symbol->index) +
                                              (symbol->name.empty() ? std::string{}
                                                                    : ":" + symbol->name),
                                          FunctionEntryEvidenceKind::RelocationFunctionTarget,
                                          FunctionEntryEvidenceStrength::Exact});
            add_entry_evidence(entry_evidence, FunctionEntryEvidence{
                                                           FunctionEntryEvidenceKind::RelocationFunctionTarget,
                                                           FunctionEntryEvidenceStrength::Exact,
                                                           module->identity.module,
                                                           relocation.target_address,
                                                           observed.target_module, observed.target,
                                                           std::nullopt, relocation.type,
                                                           relocation.source, symbol->index,
                                                           symbol->name, true, false,
                                                           "relocation resolves to a declared function symbol"});
        }
    }

    if (!observed.guest_load_address) return;
    validation.pointer_slot = observed.guest_load_address;
    if (const auto* slot_owner = process_image->module_for_address(
            observed.guest_load_address.value(), sizeof(std::uint64_t)))
    {
        validation.pointer_slot_module = slot_owner->identity.module;
    }
    const auto provenance = inspect_relocation_function_pointer_provenance(
        *process_image, observed.guest_load_address.value(), observed.target);
    if (!provenance)
    {
        validation.analysis_error = provenance.error();
        add_rejection(rejections, FunctionEntryEvidenceRejection{
                                         FunctionEntryEvidenceRejectionKind::InvalidRelocation,
                                         FunctionEntryEvidenceKind::RelocatedFunctionPointer,
                                         observed.target_module, observed.guest_load_address,
                                         provenance.error().message});
        return;
    }
    validation.relocation_provenance = provenance.value();
    validation.pointer_slot_relocation_found = !provenance.value().empty();
    for (const auto& item : provenance.value())
    {
        validation.pointer_slot_value_verified |= item.slot_value_verified;
        validation.pointer_target_declared_function |= item.target_declared_function;
        if (!item.resolved_target || item.resolved_target.value() != observed.target)
        {
            add_rejection(rejections, FunctionEntryEvidenceRejection{
                                             FunctionEntryEvidenceRejectionKind::ProvenanceMismatch,
                                             FunctionEntryEvidenceKind::RelocatedFunctionPointer,
                                             item.source_module, item.source_slot,
                                             "relocation does not resolve to the observed guest target"});
            continue;
        }
        if (!item.slot_value_verified)
        {
            add_rejection(rejections, FunctionEntryEvidenceRejection{
                                             FunctionEntryEvidenceRejectionKind::InvalidRelocation,
                                             FunctionEntryEvidenceKind::RelocatedFunctionPointer,
                                             item.source_module, item.source_slot,
                                             "relocation slot readback was not verified"});
            continue;
        }
        const auto kind = item.target_declared_function
                              ? FunctionEntryEvidenceKind::RelocationFunctionTarget
                              : FunctionEntryEvidenceKind::RelocatedFunctionPointer;
        const auto strength = item.target_declared_function
                                  ? FunctionEntryEvidenceStrength::Exact
                                  : FunctionEntryEvidenceStrength::Supporting;
        add_entry_evidence(entry_evidence, FunctionEntryEvidence{
                                                       kind, strength, item.source_module,
                                                       item.source_slot, item.target_module,
                                                       observed.target, item.relocation_index,
                                                       item.relocation.type, item.relocation.source,
                                                       item.symbol_index, item.symbol_name,
                                                       item.target_declared_function,
                                                       item.slot_value_verified,
                                                       item.target_declared_function
                                                           ? "relocation-backed pointer with function metadata"
                                                           : "R_AARCH64_RELATIVE rebased guest slot; no function metadata"});
        if (!item.target_declared_function)
        {
            add_rejection(rejections, FunctionEntryEvidenceRejection{
                                             FunctionEntryEvidenceRejectionKind::NotFunctionMetadata,
                                             FunctionEntryEvidenceKind::RelocatedFunctionPointer,
                                             item.source_module, item.source_slot,
                                             "loader relocation proves rebasing only; target is retained as supporting evidence"});
        }
        if (validation.pointer_slot_module.empty())
            validation.pointer_slot_module = item.source_module;
        add_evidence(evidence, IndirectTargetStaticEvidence{
                                      item.target_declared_function
                                          ? FunctionDiscoverySource::RelocationReference
                                          : FunctionDiscoverySource::ObservedIndirectTarget,
                                      item.target_declared_function ? FunctionConfidence::High
                                                                    : FunctionConfidence::Medium,
                                      "relocation slot " + hex_address(item.source_slot) +
                                          " resolves to observed target, type=" +
                                          std::string(format::aarch64_relocation_type_name(
                                              item.relocation.type)) +
                                          (item.target_declared_function
                                               ? ", target declared function"
                                               : ", target function type not declared"),
                                      item.target_declared_function
                                          ? FunctionEntryEvidenceKind::RelocationFunctionTarget
                                          : FunctionEntryEvidenceKind::RelocatedFunctionPointer,
                                      item.target_declared_function
                                          ? FunctionEntryEvidenceStrength::Exact
                                          : FunctionEntryEvidenceStrength::Supporting});
    }
}

void collect_direct_call_evidence(const ObservedIndirectTarget& observed,
                                  const FinalizedFunctionMap* function_map,
                                  const ProcessFunctionMap* process_function_map,
                                  std::vector<IndirectTargetStaticEvidence>& evidence)
{
    const auto collect = [&](const FinalizedFunctionMap& map) {
        for (const auto& function : map.functions())
        {
            if (std::find(function.direct_calls.begin(), function.direct_calls.end(),
                          observed.target) == function.direct_calls.end())
                continue;
            add_evidence(evidence, IndirectTargetStaticEvidence{
                                      FunctionDiscoverySource::DirectCall,
                                      FunctionConfidence::High,
                                      "finalized direct call target from " +
                                          hex_address(function.canonical_entry)});
        }
    };
    if (function_map != nullptr) collect(*function_map);
    if (process_function_map != nullptr)
    {
        for (const auto& map : process_function_map->maps()) collect(map);
    }
}

struct MapCandidate
{
    const FinalizedFunctionMap* map = nullptr;
    const FunctionRecord* exact = nullptr;
    bool executable_range_contains = false;
};

[[nodiscard]] std::vector<MapCandidate> candidate_maps(
    GuestAddress target, const FinalizedFunctionMap* function_map,
    const ProcessFunctionMap* process_function_map)
{
    std::vector<MapCandidate> result;
    if (process_function_map != nullptr)
    {
        for (const auto& map : process_function_map->maps())
        {
            const auto exact = map.find_exact_entry(target);
            const bool in_range = contains_any(map.identity().executable_ranges, target, 4U);
            if (exact != nullptr || in_range)
                result.push_back(MapCandidate{&map, exact, in_range});
        }
    }
    else if (function_map != nullptr)
    {
        result.push_back(MapCandidate{
            function_map, function_map->find_exact_entry(target),
            contains_any(function_map->identity().executable_ranges, target, 4U)});
    }
    return result;
}

[[nodiscard]] const FunctionRecord* find_precise_owner(const FinalizedFunctionMap& map,
                                                        GuestAddress target)
{
    const auto owners = map.find_precise_owners(target);
    return owners.empty() ? nullptr : owners.front();
}

[[nodiscard]] bool inside_display_envelope(const FunctionRecord& function,
                                            GuestAddress target) noexcept
{
    return function.range_begin <= target && target < function.range_end;
}

[[nodiscard]] Result<std::vector<GuestAddressRange>> cfg_owned_ranges(
    const ControlFlowGraph& cfg)
{
    std::vector<GuestAddress> instruction_addresses;
    instruction_addresses.reserve(cfg.instruction_count);
    for (const auto& [unused, block] : cfg.blocks)
    {
        (void)unused;
        for (const auto& instruction : block.instructions)
            instruction_addresses.push_back(instruction.address);
    }
    return normalize_code_ranges(instruction_addresses);
}

[[nodiscard]] bool ranges_are_subset(const std::vector<GuestAddressRange>& subset,
                                     const std::vector<GuestAddressRange>& superset) noexcept
{
    for (const auto& range : subset)
    {
        if (!std::any_of(superset.begin(), superset.end(), [&range](const auto& enclosing) {
                return contains(enclosing, range.base, range.size);
            }))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<GuestAddress> ordinary_branch_source(
    const ControlFlowGraph& cfg, GuestAddress target)
{
    for (const auto& [unused, block] : cfg.blocks)
    {
        (void)unused;
        for (const auto& edge : block.successors)
        {
            if (edge.kind == EdgeKind::Branch && edge.internal && edge.target == target)
                return edge.source;
        }
    }
    return std::nullopt;
}

void configure_cfg_options(AnalysisOptions& cfg,
                           const IndirectTargetDiscoveryOptions& options,
                           const FinalizedFunctionMap* target_map, GuestAddress target);

struct AnalyzerOverClaimAttempt
{
    FunctionBoundaryReconciliation reconciliation;
    ControlFlowGraph candidate_cfg;
};

[[nodiscard]] std::optional<AnalyzerOverClaimAttempt> reconcile_analyzer_overclaim(
    const FunctionRecord& existing_function, const ControlFlowGraph& candidate_cfg,
    const std::vector<GuestAddressRange>& candidate_ranges,
    const memory::GuestMemory& memory, const FinalizedFunctionMap& analysis_map,
    GuestAddress candidate_entry, const IndirectTargetDiscoveryOptions& options)
{
    if (!existing_function.cfg || existing_function.entry_trust_status ==
                                      FunctionEntryTrustStatus::Conflict ||
        existing_function.translation_status == TranslationStatus::Conflict)
    {
        return std::nullopt;
    }

    const auto overlap = intersect_owned_ranges(candidate_ranges,
                                                existing_function.owned_code_ranges);
    if (!overlap || overlap.value().empty()) return std::nullopt;

    // A boundary proof must contain all three independent structural facts:
    // the old function branches to the proposed helper, the candidate calls
    // it directly, and the candidate also transfers to it as a tail. A raw
    // observed target or relocation is intentionally not accepted here.
    std::set<GuestAddress> boundary_entries;
    std::optional<GuestAddress> existing_branch_source;
    std::optional<GuestAddress> candidate_call_source;
    std::optional<GuestAddress> candidate_branch_source;
    for (const auto& call : candidate_cfg.calls)
    {
        if (call.kind != CallKind::Direct || !call.target ||
            !contains_any(existing_function.owned_code_ranges, call.target.value(), 4U) ||
            !contains_any(overlap.value(), call.target.value(), 4U))
        {
            continue;
        }
        const auto target = call.target.value();
        const auto old_branch = ordinary_branch_source(existing_function.cfg.value(), target);
        const auto new_branch = ordinary_branch_source(candidate_cfg, target);
        if (!old_branch || !new_branch) continue;
        boundary_entries.insert(target);
        existing_branch_source = old_branch;
        candidate_call_source = call.address;
        candidate_branch_source = new_branch;
    }
    if (boundary_entries.size() != 1U) return std::nullopt;

    AnalysisOptions boundary_options = options.cfg;
    configure_cfg_options(boundary_options, options, &analysis_map, candidate_entry);
    boundary_options.known_function_entries.insert(boundary_entries.begin(), boundary_entries.end());

    const auto analyze = [&](GuestAddress entry)
        -> std::optional<std::pair<ControlFlowGraph, std::vector<GuestAddressRange>>> {
        const auto graph = analyze_control_flow(memory, entry, boundary_options);
        if (!graph || graph.value().blocks.empty()) return std::nullopt;
        const auto ranges = cfg_owned_ranges(graph.value());
        if (!ranges || ranges.value().empty()) return std::nullopt;
        return std::make_pair(graph.value(), ranges.value());
    };

    const auto existing_after = analyze(existing_function.canonical_entry);
    const auto candidate_after = analyze(candidate_entry);
    if (!existing_after || !candidate_after) return std::nullopt;

    std::vector<std::pair<GuestAddress, std::vector<GuestAddressRange>>> boundary_after;
    std::optional<ControlFlowGraph> boundary_cfg;
    for (const auto entry : boundary_entries)
    {
        const auto analyzed = analyze(entry);
        if (!analyzed || !analyzed->first.unresolved.empty()) return std::nullopt;
        boundary_cfg = analyzed->first;
        boundary_after.emplace_back(entry, analyzed->second);
    }

    std::vector<GuestAddressRange> boundary_ranges;
    for (const auto& [unused, ranges] : boundary_after)
    {
        (void)unused;
        const auto normalized = normalize_code_ranges(ranges);
        if (!normalized) return std::nullopt;
        boundary_ranges.insert(boundary_ranges.end(), normalized.value().begin(),
                               normalized.value().end());
    }
    const auto normalized_boundary_ranges = normalize_code_ranges(boundary_ranges);
    if (!normalized_boundary_ranges || normalized_boundary_ranges.value() != overlap.value())
        return std::nullopt;
    if (owned_ranges_overlap(existing_after->second, candidate_after->second) ||
        contains_any(existing_after->second, *boundary_entries.begin(), 4U) ||
        contains_any(candidate_after->second, *boundary_entries.begin(), 4U) ||
        !ranges_are_subset(existing_after->second, existing_function.owned_code_ranges) ||
        !ranges_are_subset(candidate_after->second, candidate_ranges))
    {
        return std::nullopt;
    }

    FunctionBoundaryReconciliation reconciliation;
    reconciliation.kind = FunctionBoundaryReconciliationKind::AnalyzerOverClaim;
    reconciliation.existing_canonical_entry = existing_function.canonical_entry;
    reconciliation.existing_owned_code_ranges_before = existing_function.owned_code_ranges;
    reconciliation.candidate_owned_code_ranges_before = candidate_ranges;
    reconciliation.precise_overlap_ranges = overlap.value();
    reconciliation.existing_owned_code_ranges_after = existing_after->second;
    reconciliation.candidate_owned_code_ranges_after = candidate_after->second;
    reconciliation.boundary_owned_code_ranges = normalized_boundary_ranges.value();
    reconciliation.boundary_entries.assign(boundary_entries.begin(), boundary_entries.end());
    reconciliation.existing_cfg_before = existing_function.cfg;
    reconciliation.candidate_cfg_before = candidate_cfg;
    reconciliation.boundary_cfg = boundary_cfg;
    reconciliation.witnesses.push_back(FunctionBoundaryWitness{
        FunctionBoundaryWitnessKind::BoundaryCFG, *boundary_entries.begin(),
        *boundary_entries.begin()});
    reconciliation.witnesses.push_back(FunctionBoundaryWitness{
        FunctionBoundaryWitnessKind::ExistingUnconditionalBranch, existing_branch_source.value(),
        *boundary_entries.begin()});
    reconciliation.witnesses.push_back(FunctionBoundaryWitness{
        FunctionBoundaryWitnessKind::CandidateDirectCall, candidate_call_source.value(),
        *boundary_entries.begin()});
    reconciliation.witnesses.push_back(FunctionBoundaryWitness{
        FunctionBoundaryWitnessKind::CandidateUnconditionalBranch, candidate_branch_source.value(),
        *boundary_entries.begin()});
    std::sort(reconciliation.witnesses.begin(), reconciliation.witnesses.end(),
              [](const auto& left, const auto& right) {
                  return std::tie(left.kind, left.source, left.target) <
                         std::tie(right.kind, right.source, right.target);
              });
    reconciliation.reason =
        "precise overlap is exactly a separately discoverable direct-call/tail-transfer helper; "
        "boundary-aware reanalysis removes it from both callers";
    return AnalyzerOverClaimAttempt{std::move(reconciliation), candidate_after->first};
}

void add_record_evidence(const FunctionRecord* record,
                         std::vector<IndirectTargetStaticEvidence>& evidence)
{
    if (record == nullptr) return;
    for (const auto& item : record->evidence)
    {
        add_evidence(evidence, IndirectTargetStaticEvidence{
                                  item.source, item.confidence,
                                  "finalized function evidence: " + item.note});
    }
}

[[nodiscard]] std::optional<GuestAddressRange> range_containing(
    const std::vector<GuestAddressRange>& ranges, GuestAddress address)
{
    for (const auto& range : ranges)
    {
        if (contains(range, address, 4U)) return range;
    }
    return std::nullopt;
}

void configure_cfg_options(AnalysisOptions& cfg, const IndirectTargetDiscoveryOptions& options,
                           const FinalizedFunctionMap* target_map, GuestAddress target)
{
    cfg.max_instructions = std::min(cfg.max_instructions, options.budgets.max_instructions);
    cfg.max_basic_blocks = std::min(cfg.max_basic_blocks, options.budgets.max_blocks);
    cfg.max_pending_targets = std::min(cfg.max_pending_targets, options.budgets.max_seeds);
    if (target_map != nullptr)
    {
        if (!cfg.allowed_code_range)
            cfg.allowed_code_range = range_containing(target_map->identity().executable_ranges,
                                                      target);
        for (const auto& function : target_map->functions())
        {
            for (const auto entry : function.entries) cfg.known_function_entries.insert(entry);
        }
    }
}

void account_cfg(const ControlFlowGraph& cfg, IndirectTargetValidation& validation)
{
    validation.blocks = cfg.blocks.size();
    validation.instructions = cfg.instruction_count;
    validation.edges = cfg.unresolved.size();
    for (const auto& [unused, block] : cfg.blocks)
    {
        (void)unused;
        std::size_t block_edges = 0U;
        if (!checked_add_size(block.successors.size(), block.calls.size(), block_edges) ||
            !checked_add_size(validation.edges, block_edges, validation.edges))
        {
            validation.analysis_error = make_error(
                ErrorCode::AnalysisBudgetExceeded,
                "candidate CFG edge count overflows the host size type");
            validation.cfg_status = IndirectTargetCFGStatus::BudgetExceeded;
            return;
        }
    }
    for (const auto& call : cfg.calls)
    {
        if (call.kind == CallKind::Direct && call.target)
            validation.direct_call_targets.push_back(call.target.value());
    }
    std::sort(validation.direct_call_targets.begin(), validation.direct_call_targets.end());
    validation.direct_call_targets.erase(
        std::unique(validation.direct_call_targets.begin(), validation.direct_call_targets.end()),
        validation.direct_call_targets.end());
    validation.unresolved_control_flow = cfg.unresolved;
}

[[nodiscard]] FunctionMapOptions map_options(const IndirectTargetDiscoveryOptions& options)
{
    FunctionMapOptions result;
    result.budgets = options.budgets;
    result.cfg = options.cfg;
    result.continue_after_function_failure = false;
    return result;
}

[[nodiscard]] IndirectTargetRefinementAnalysisWork analysis_work_from_accounting(
    const AnalysisAccounting& accounting)
{
    return IndirectTargetRefinementAnalysisWork{
        accounting.module,
        accounting.functions_cfg_analyzed,
        accounting.reanalyzed_functions,
        accounting.reused_functions,
        accounting.instructions_consumed,
        accounting.blocks_consumed,
        accounting.edges_consumed,
        accounting.bytes_analyzed,
        accounting.boundary_finalization_passes,
        accounting.invalidated_records,
        accounting.refinement_transactions};
}

[[nodiscard]] std::vector<FunctionSeed> seeds_from_finalized_map(
    const FinalizedFunctionMap& map)
{
    std::vector<FunctionSeed> seeds;
    for (const auto& function : map.functions())
    {
        if (function.evidence.empty())
        {
            seeds.push_back(FunctionSeed{function.canonical_entry,
                                         FunctionDiscoverySource::Heuristic,
                                         function.confidence, std::nullopt, function.name,
                                         "reconstructed from frozen function map"});
            continue;
        }
        for (const auto& evidence : function.evidence)
        {
            seeds.push_back(FunctionSeed{evidence.entry, evidence.source, evidence.confidence,
                                         function.canonical_entry == evidence.entry
                                             ? std::nullopt
                                             : std::optional<GuestAddress>(function.canonical_entry),
                                         function.name, evidence.note});
        }
    }
    return seeds;
}

void set_rebuild_roots(FunctionMapOptions& options, const std::vector<FunctionSeed>& seeds)
{
    if (options.budgets.strategy != AnalysisStrategy::ExecutionClosure) return;
    options.execution_closure_roots.clear();
    for (const auto& seed : seeds)
    {
        options.execution_closure_roots.insert(seed.entry);
        options.execution_closure_roots.insert(seed.canonical_entry.value_or(seed.entry));
    }
}

void mark_refinement_failure(IndirectTargetAssessment& assessment, const Error& error)
{
    assessment.validation.analysis_error = error;
    assessment.decision.eligible_for_promotion = false;
    assessment.decision.promoted = false;
    assessment.decision.kind = is_budget_error(error.code)
                                   ? IndirectTargetDecisionKind::BudgetExceeded
                                   : IndirectTargetDecisionKind::AnalysisFailed;
    assessment.decision.confidence = FunctionConfidence::Low;
    assessment.decision.reason = "immutable function-map refinement failed: " + error.message;
    assessment.certification.certified = false;
    assessment.certification.confidence = FunctionConfidence::Low;
    assessment.certification.reason = assessment.decision.reason;
    assessment.certification.status = is_budget_error(error.code)
                                          ? FunctionCertificationStatus::BoundsExceeded
                                          : FunctionCertificationStatus::CFGIncomplete;
}

[[nodiscard]] bool boundary_refinement_matches(
    const FinalizedFunctionMap& map, const FunctionBoundaryReconciliation& reconciliation,
    GuestAddress candidate_entry) noexcept
{
    if (reconciliation.kind != FunctionBoundaryReconciliationKind::AnalyzerOverClaim)
        return true;
    if (!reconciliation.existing_canonical_entry) return false;
    const auto* existing = map.find_canonical_entry(
        reconciliation.existing_canonical_entry.value());
    const auto* candidate = map.find_exact_entry(candidate_entry);
    if (existing == nullptr || candidate == nullptr || candidate->canonical_entry != candidate_entry ||
        candidate->owned_code_ranges != reconciliation.candidate_owned_code_ranges_after ||
        existing->owned_code_ranges != reconciliation.existing_owned_code_ranges_after)
    {
        return false;
    }
    for (const auto boundary : reconciliation.boundary_entries)
    {
        const auto* canonical = map.find_canonical_entry(boundary);
        const auto owners = map.find_precise_owners(boundary);
        if (canonical == nullptr || owners.size() != 1U || owners.front() != canonical ||
            canonical->owned_code_ranges != reconciliation.boundary_owned_code_ranges)
        {
            return false;
        }
    }
    return !owned_ranges_overlap(existing->owned_code_ranges, candidate->owned_code_ranges);
}

[[nodiscard]] Result<IndirectTargetAssessment> complete_assessment(
    IndirectTargetAssessment assessment)
{
    normalize_entry_evidence(assessment.entry_evidence);
    normalize_rejections(assessment.rejected_evidence);
    assessment.candidate.module = assessment.validation.target_module;
    assessment.candidate.entry = assessment.observed.target;
    assessment.candidate.owned_code_ranges = assessment.validation.candidate_owned_code_ranges;
    assessment.candidate.cfg = assessment.validation.cfg;
    assessment.candidate.evidence = assessment.entry_evidence;
    assessment.certification.accepted_evidence = assessment.entry_evidence;
    assessment.certification.rejected_evidence = assessment.rejected_evidence;
    assessment.certification.confidence = assessment.decision.confidence;
    assessment.certification.reason = assessment.decision.reason;
    switch (assessment.decision.kind)
    {
    case IndirectTargetDecisionKind::TrustedExistingEntry:
    case IndirectTargetDecisionKind::TrustedNewEntry:
    case IndirectTargetDecisionKind::AliasOfExistingEntry:
        assessment.certification.status = FunctionCertificationStatus::Certified;
        assessment.certification.certified = assessment.decision.eligible_for_promotion;
        break;
    case IndirectTargetDecisionKind::InvalidAlignment:
        assessment.certification.status = FunctionCertificationStatus::Misaligned;
        break;
    case IndirectTargetDecisionKind::NonExecutable:
        assessment.certification.status = FunctionCertificationStatus::NotExecutable;
        break;
    case IndirectTargetDecisionKind::Unmapped:
        assessment.certification.status = FunctionCertificationStatus::OutsideKnownModule;
        break;
    case IndirectTargetDecisionKind::CrossModuleAmbiguity:
        assessment.certification.status = FunctionCertificationStatus::AmbiguousOwnership;
        break;
    case IndirectTargetDecisionKind::FunctionBoundaryConflict:
        assessment.certification.status = assessment.validation.ownership ==
                                                   IndirectTargetOwnership::CandidateOverlap
                                               ? FunctionCertificationStatus::OverlapsExistingFunction
                                               : FunctionCertificationStatus::ConflictsWithExistingFunction;
        break;
    case IndirectTargetDecisionKind::InsideExistingFunction:
        assessment.certification.status = FunctionCertificationStatus::OverlapsExistingFunction;
        break;
    case IndirectTargetDecisionKind::BudgetExceeded:
        assessment.certification.status = FunctionCertificationStatus::BoundsExceeded;
        break;
    case IndirectTargetDecisionKind::AnalysisFailed:
        assessment.certification.status = assessment.validation.analysis_error &&
                                                   assessment.validation.analysis_error->code ==
                                                       ErrorCode::DecodeFailed
                                               ? FunctionCertificationStatus::DecodeFailed
                                               : FunctionCertificationStatus::CFGIncomplete;
        break;
    case IndirectTargetDecisionKind::InsufficientEvidence:
        assessment.certification.status = FunctionCertificationStatus::InsufficientEvidence;
        break;
    case IndirectTargetDecisionKind::UnsupportedTarget:
        assessment.certification.status = FunctionCertificationStatus::InvalidProvenance;
        break;
    }
    if (!assessment.certification.certified &&
        assessment.certification.status == FunctionCertificationStatus::InsufficientEvidence)
    {
        add_rejection(assessment.certification.rejected_evidence,
                      FunctionEntryEvidenceRejection{
                          FunctionEntryEvidenceRejectionKind::InsufficientEvidence,
                          FunctionEntryEvidenceKind::ObservedIndirectCall,
                          assessment.observed.source_module, assessment.observed.source_pc,
                          assessment.decision.reason});
        assessment.rejected_evidence = assessment.certification.rejected_evidence;
        normalize_rejections(assessment.rejected_evidence);
    }
    return Result<IndirectTargetAssessment>::success(std::move(assessment));
}

} // namespace

std::string_view indirect_control_flow_kind_name(IndirectControlFlowKind kind) noexcept
{
    switch (kind)
    {
    case IndirectControlFlowKind::Call: return "indirect_call";
    case IndirectControlFlowKind::Branch: return "indirect_branch";
    case IndirectControlFlowKind::Return: return "return";
    }
    return "unknown";
}

std::string_view indirect_target_pointer_provenance_name(
    IndirectTargetPointerProvenanceKind kind) noexcept
{
    switch (kind)
    {
    case IndirectTargetPointerProvenanceKind::Unknown: return "unknown";
    case IndirectTargetPointerProvenanceKind::GuestLoad: return "guest_load";
    }
    return "unknown";
}

std::string_view indirect_target_ownership_name(IndirectTargetOwnership ownership) noexcept
{
    switch (ownership)
    {
    case IndirectTargetOwnership::Unknown: return "unknown";
    case IndirectTargetOwnership::TrustedExistingEntry: return "trusted_existing_entry";
    case IndirectTargetOwnership::ExistingSecondaryEntry: return "existing_secondary_entry";
    case IndirectTargetOwnership::InsideExistingFunction: return "inside_existing_function";
    case IndirectTargetOwnership::ExistingDisplayEnvelopeOnly:
        return "existing_display_envelope_only";
    case IndirectTargetOwnership::CandidateOverlap: return "candidate_overlap";
    case IndirectTargetOwnership::NewEntry: return "new_entry";
    case IndirectTargetOwnership::CrossModuleAmbiguity: return "cross_module_ambiguity";
    }
    return "unknown";
}

std::string_view indirect_target_cfg_status_name(IndirectTargetCFGStatus status) noexcept
{
    switch (status)
    {
    case IndirectTargetCFGStatus::NotAnalyzed: return "not_analyzed";
    case IndirectTargetCFGStatus::ExistingTrustedCFG: return "existing_trusted_cfg";
    case IndirectTargetCFGStatus::Validated: return "validated";
    case IndirectTargetCFGStatus::ValidatedWithUnresolvedFlow:
        return "validated_with_unresolved_flow";
    case IndirectTargetCFGStatus::AnalysisFailed: return "analysis_failed";
    case IndirectTargetCFGStatus::BudgetExceeded: return "budget_exceeded";
    }
    return "unknown";
}

std::string_view indirect_target_decision_name(IndirectTargetDecisionKind decision) noexcept
{
    switch (decision)
    {
    case IndirectTargetDecisionKind::TrustedExistingEntry: return "trusted_existing_entry";
    case IndirectTargetDecisionKind::TrustedNewEntry: return "trusted_new_entry";
    case IndirectTargetDecisionKind::AliasOfExistingEntry: return "alias_of_existing_entry";
    case IndirectTargetDecisionKind::FunctionBoundaryConflict:
        return "function_boundary_conflict";
    case IndirectTargetDecisionKind::InsideExistingFunction: return "inside_existing_function";
    case IndirectTargetDecisionKind::NonExecutable: return "non_executable";
    case IndirectTargetDecisionKind::Unmapped: return "unmapped";
    case IndirectTargetDecisionKind::CrossModuleAmbiguity: return "cross_module_ambiguity";
    case IndirectTargetDecisionKind::InvalidAlignment: return "invalid_alignment";
    case IndirectTargetDecisionKind::AnalysisFailed: return "analysis_failed";
    case IndirectTargetDecisionKind::InsufficientEvidence: return "insufficient_evidence";
    case IndirectTargetDecisionKind::BudgetExceeded: return "budget_exceeded";
    case IndirectTargetDecisionKind::UnsupportedTarget: return "unsupported_target";
    }
    return "unsupported_target";
}

std::string_view function_boundary_reconciliation_kind_name(
    FunctionBoundaryReconciliationKind kind) noexcept
{
    switch (kind)
    {
    case FunctionBoundaryReconciliationKind::None: return "none";
    case FunctionBoundaryReconciliationKind::AnalyzerOverClaim: return "analyzer_over_claim";
    case FunctionBoundaryReconciliationKind::IncompatiblePreciseOverlap:
        return "incompatible_precise_overlap";
    }
    return "none";
}

std::string_view function_boundary_witness_kind_name(
    FunctionBoundaryWitnessKind kind) noexcept
{
    switch (kind)
    {
    case FunctionBoundaryWitnessKind::ExistingUnconditionalBranch:
        return "existing_unconditional_branch";
    case FunctionBoundaryWitnessKind::CandidateDirectCall: return "candidate_direct_call";
    case FunctionBoundaryWitnessKind::CandidateUnconditionalBranch:
        return "candidate_unconditional_branch";
    case FunctionBoundaryWitnessKind::BoundaryCFG: return "boundary_cfg";
    }
    return "boundary_cfg";
}

std::string_view function_entry_evidence_kind_name(FunctionEntryEvidenceKind kind) noexcept
{
    switch (kind)
    {
    case FunctionEntryEvidenceKind::SymbolFunction: return "symbol_function";
    case FunctionEntryEvidenceKind::DynamicSymbolFunction: return "dynamic_symbol_function";
    case FunctionEntryEvidenceKind::DirectCallTarget: return "direct_call_target";
    case FunctionEntryEvidenceKind::RelocationFunctionTarget:
        return "relocation_function_target";
    case FunctionEntryEvidenceKind::RelocatedFunctionPointer: return "relocated_function_pointer";
    case FunctionEntryEvidenceKind::ExportedFunction: return "exported_function";
    case FunctionEntryEvidenceKind::KnownProviderEntry: return "known_provider_entry";
    case FunctionEntryEvidenceKind::BoundedCfgCandidate: return "bounded_cfg_candidate";
    case FunctionEntryEvidenceKind::ObservedIndirectCall: return "observed_indirect_call";
    case FunctionEntryEvidenceKind::GuestLoadedPointer: return "guest_loaded_pointer";
    }
    return "bounded_cfg_candidate";
}

std::string_view function_entry_evidence_strength_name(
    FunctionEntryEvidenceStrength strength) noexcept
{
    switch (strength)
    {
    case FunctionEntryEvidenceStrength::Exact: return "exact";
    case FunctionEntryEvidenceStrength::Supporting: return "supporting";
    }
    return "supporting";
}

std::string_view function_entry_evidence_rejection_name(
    FunctionEntryEvidenceRejectionKind kind) noexcept
{
    switch (kind)
    {
    case FunctionEntryEvidenceRejectionKind::NotFunctionMetadata: return "not_function_metadata";
    case FunctionEntryEvidenceRejectionKind::ProvenanceMismatch: return "provenance_mismatch";
    case FunctionEntryEvidenceRejectionKind::InvalidRelocation: return "invalid_relocation";
    case FunctionEntryEvidenceRejectionKind::InsufficientEvidence: return "insufficient_evidence";
    }
    return "insufficient_evidence";
}

std::string_view function_certification_status_name(FunctionCertificationStatus status) noexcept
{
    switch (status)
    {
    case FunctionCertificationStatus::Certified: return "certified";
    case FunctionCertificationStatus::InsufficientEvidence: return "insufficient_evidence";
    case FunctionCertificationStatus::NotExecutable: return "not_executable";
    case FunctionCertificationStatus::Misaligned: return "misaligned";
    case FunctionCertificationStatus::OutsideKnownModule: return "outside_known_module";
    case FunctionCertificationStatus::ConflictsWithExistingFunction:
        return "conflicts_with_existing_function";
    case FunctionCertificationStatus::OverlapsExistingFunction: return "overlaps_existing_function";
    case FunctionCertificationStatus::DecodeFailed: return "decode_failed";
    case FunctionCertificationStatus::CFGIncomplete: return "cfg_incomplete";
    case FunctionCertificationStatus::AmbiguousOwnership: return "ambiguous_ownership";
    case FunctionCertificationStatus::BoundsExceeded: return "bounds_exceeded";
    case FunctionCertificationStatus::InvalidProvenance: return "invalid_provenance";
    }
    return "insufficient_evidence";
}

Result<std::vector<RelocationFunctionPointerProvenance>>
inspect_relocation_function_pointer_provenance(const ProcessImage& process_image,
                                               GuestAddress source_slot,
                                               GuestAddress expected_target)
{
    std::vector<RelocationFunctionPointerProvenance> result;
    for (const auto& reference : process_image.relocation_references(source_slot))
    {
        const auto* module = process_image.module(reference.module);
        if (module == nullptr || reference.relocation_index >= module->relocations.size()) continue;
        const auto index = reference.relocation_index;
        const auto& relocation = module->relocations[index];

        RelocationFunctionPointerProvenance item;
        item.source_module = module->identity.module;
        item.source_slot = source_slot;
        item.relocation_index = index;
        item.relocation = relocation;
        item.symbol_index = relocation.symbol_index;

        if (!is_supported_relocation_type(relocation.type))
        {
            item.resolution_error = "relocation type is not supported by the guest loader";
            result.push_back(std::move(item));
            continue;
        }

        std::optional<std::uint64_t> resolved;
        if (relocation.type == format::AArch64RelocationType::Relative)
        {
            const auto value = checked_add_signed_u64(module->identity.guest_base,
                                                      relocation.addend);
            if (value) resolved = value.value();
            else item.resolution_error = value.error().message;
        }
        else if (module->symbols)
        {
            const auto* symbol = module->symbols->at(relocation.symbol_index);
            if (symbol == nullptr)
            {
                item.resolution_error = "relocation symbol index is not present";
            }
            else
            {
                item.symbol_name = symbol->name;
                item.target_declared_function = symbol->type == format::SymbolType::Function;
                if (symbol->is_defined())
                {
                    const auto address = symbol_address(*symbol, module->identity.guest_base);
                    if (!address)
                        item.resolution_error = "relocation symbol address overflows";
                    else
                    {
                        const auto value = checked_add_signed_u64(address.value(), relocation.addend);
                        if (value) resolved = value.value();
                        else item.resolution_error = value.error().message;
                    }
                }
                else
                {
                    const auto binding = std::find_if(
                        process_image.bindings().begin(), process_image.bindings().end(),
                        [&](const auto& candidate) {
                            return candidate.consumer_module == module->identity.module &&
                                   candidate.relocation_index == index;
                        });
                    if (binding != process_image.bindings().end())
                    {
                        item.symbol_name = binding->symbol;
                        item.target_declared_function =
                            binding->provider.selected_candidate &&
                            binding->provider.candidates[binding->provider.selected_candidate.value()]
                                    .type == format::SymbolType::Function;
                        if (binding->resolved_value)
                            resolved = binding->resolved_value.value();
                        else
                            item.resolution_error = "unresolved imported relocation";
                    }
                    else
                        item.resolution_error = "relocation has no process binding";
                }
            }
        }
        else
        {
            item.resolution_error = "relocation has no dynamic symbol table";
        }

        if (resolved)
        {
            item.resolved_target = resolved.value();
            if (const auto* target_module = process_image.module_for_address(resolved.value(), 4U))
                item.target_module = target_module->identity.module;

            const auto width = loader::relocation_width(relocation.type);
            std::array<std::byte, sizeof(std::uint64_t)> bytes{};
            const auto read = process_image.memory().read(
                source_slot, std::span<std::byte>(bytes.data(), width));
            if (read)
            {
                std::uint64_t observed_value = 0U;
                for (std::size_t byte = 0U; byte < width; ++byte)
                {
                    observed_value |= static_cast<std::uint64_t>(
                                          std::to_integer<unsigned int>(bytes[byte]))
                                      << (byte * 8U);
                }
                const auto expected_value = width == 4U ? (resolved.value() & 0xffffffffULL)
                                                        : resolved.value();
                item.slot_value_verified = observed_value == expected_value;
                if (!item.slot_value_verified)
                    item.resolution_error = "relocation slot value does not match its resolved value";
            }
            else
            {
                item.resolution_error = "relocation slot could not be read back";
            }
        }
        if (!item.resolved_target || item.resolved_target.value() != expected_target)
        {
            if (!item.resolution_error)
                item.resolution_error = "relocation resolved target differs from observed target";
        }
        result.push_back(std::move(item));
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return std::tie(left.source_module, left.source_slot, left.relocation_index,
                        left.relocation.type, left.relocation.source) <
               std::tie(right.source_module, right.source_slot, right.relocation_index,
                        right.relocation.type, right.relocation.source);
    });
    return Result<std::vector<RelocationFunctionPointerProvenance>>::success(std::move(result));
}

Result<IndirectTargetAssessment> assess_indirect_target(
    const ObservedIndirectTarget& observed, const memory::GuestMemory& memory,
    const FinalizedFunctionMap* function_map, const ProcessFunctionMap* process_function_map,
    const ProcessImage* process_image, const IndirectTargetDiscoveryOptions& options)
{
    IndirectTargetAssessment assessment;
    assessment.observed = observed;
    assessment.static_evidence = observed.static_evidence;
    normalize_evidence(assessment.static_evidence);
    for (const auto& item : observed.entry_evidence)
        add_entry_evidence(assessment.entry_evidence, item);
    for (const auto& item : observed.static_evidence)
    {
        FunctionEntryEvidenceKind kind = FunctionEntryEvidenceKind::BoundedCfgCandidate;
        FunctionEntryEvidenceStrength strength = FunctionEntryEvidenceStrength::Supporting;
        switch (item.source)
        {
        case FunctionDiscoverySource::DynamicSymbol:
            kind = FunctionEntryEvidenceKind::DynamicSymbolFunction;
            strength = FunctionEntryEvidenceStrength::Exact;
            break;
        case FunctionDiscoverySource::DirectCall:
            kind = FunctionEntryEvidenceKind::DirectCallTarget;
            strength = FunctionEntryEvidenceStrength::Exact;
            break;
        case FunctionDiscoverySource::RelocationReference:
            kind = FunctionEntryEvidenceKind::RelocationFunctionTarget;
            strength = FunctionEntryEvidenceStrength::Exact;
            break;
        case FunctionDiscoverySource::Export:
            kind = FunctionEntryEvidenceKind::ExportedFunction;
            strength = FunctionEntryEvidenceStrength::Exact;
            break;
        default: break;
        }
        add_entry_evidence(assessment.entry_evidence,
                           FunctionEntryEvidence{kind, strength, observed.source_module,
                                                  observed.source_pc, observed.target_module,
                                                  observed.target, std::nullopt, std::nullopt,
                                                  std::nullopt, std::nullopt, {}, false, false,
                                                  item.detail});
    }
    if (observed.control_flow != IndirectControlFlowKind::Return)
    {
        add_entry_evidence(assessment.entry_evidence,
                           FunctionEntryEvidence{
                               FunctionEntryEvidenceKind::ObservedIndirectCall,
                               FunctionEntryEvidenceStrength::Supporting, observed.source_module,
                               observed.source_pc, observed.target_module, observed.target,
                               std::nullopt, std::nullopt, std::nullopt, std::nullopt, {}, false,
                               false, "runtime observed indirect control flow"});
    }
    if (observed.pointer_provenance == IndirectTargetPointerProvenanceKind::GuestLoad &&
        observed.guest_load_address)
    {
        add_entry_evidence(assessment.entry_evidence,
                           FunctionEntryEvidence{
                               FunctionEntryEvidenceKind::GuestLoadedPointer,
                               FunctionEntryEvidenceStrength::Supporting, observed.source_module,
                               observed.guest_load_address, observed.target_module, observed.target,
                               std::nullopt, std::nullopt, std::nullopt, std::nullopt, {}, false,
                               false, "runtime register value was loaded from guest memory"});
    }
    auto& validation = assessment.validation;
    auto& decision = assessment.decision;
    validation.nonzero = observed.target != 0U;
    validation.aligned = (observed.target & 0x3U) == 0U;
    if (!validation.nonzero || !validation.aligned)
    {
        decision.kind = validation.nonzero ? IndirectTargetDecisionKind::InvalidAlignment
                                            : IndirectTargetDecisionKind::Unmapped;
        decision.reason = validation.nonzero ? "target is not 4-byte AArch64 aligned"
                                             : "target is zero";
        return complete_assessment(std::move(assessment));
    }

    const auto region = memory.region_at(observed.target);
    validation.mapped = region.has_value();
    const auto executable = memory.is_executable(observed.target, 4U);
    validation.executable = executable && executable.value();
    if (!validation.mapped)
    {
        decision.kind = IndirectTargetDecisionKind::Unmapped;
        decision.reason = "target is not mapped in checked guest memory";
        if (!executable) validation.analysis_error = executable.error();
        return complete_assessment(std::move(assessment));
    }
    if (!executable || !executable.value())
    {
        decision.kind = IndirectTargetDecisionKind::NonExecutable;
        decision.reason = executable ? "target mapping lacks execute permission"
                                     : executable.error().message;
        if (!executable) validation.analysis_error = executable.error();
        return complete_assessment(std::move(assessment));
    }

    const auto maps = candidate_maps(observed.target, function_map, process_function_map);
    const ProcessModule* process_owner =
        process_image == nullptr ? nullptr : process_image->module_for_address(observed.target, 4U);
    if (process_owner != nullptr)
    {
        validation.target_module = process_owner->identity.module;
        validation.target_module_base = process_owner->identity.guest_base;
        validation.unique_module_owner = true;
    }
    else if (process_image != nullptr)
    {
        decision.kind = IndirectTargetDecisionKind::CrossModuleAmbiguity;
        decision.reason = "executable target is not owned by exactly one process module";
        validation.ownership = IndirectTargetOwnership::CrossModuleAmbiguity;
        return complete_assessment(std::move(assessment));
    }
    else if (maps.size() == 1U && maps.front().executable_range_contains)
    {
        validation.target_module = maps.front().map->identity().module;
        validation.target_module_base = maps.front().map->identity().guest_base;
        validation.unique_module_owner = true;
    }
    else if (maps.size() > 1U)
    {
        decision.kind = IndirectTargetDecisionKind::CrossModuleAmbiguity;
        decision.reason = "executable target is covered by multiple process-map modules";
        validation.ownership = IndirectTargetOwnership::CrossModuleAmbiguity;
        return complete_assessment(std::move(assessment));
    }
    else
    {
        decision.kind = IndirectTargetDecisionKind::CrossModuleAmbiguity;
        decision.reason = "executable target has no unique project-owned module";
        return complete_assessment(std::move(assessment));
    }
    if (!observed.target_module.empty() && observed.target_module != validation.target_module)
    {
        decision.kind = IndirectTargetDecisionKind::CrossModuleAmbiguity;
        decision.reason = "runtime target module disagrees with the unique mapped module";
        validation.ownership = IndirectTargetOwnership::CrossModuleAmbiguity;
        validation.unique_module_owner = false;
        return complete_assessment(std::move(assessment));
    }

    const auto exact_count = static_cast<std::size_t>(std::count_if(
        maps.begin(), maps.end(), [](const auto& item) { return item.exact != nullptr; }));
    collect_process_static_evidence(observed, process_image, exact_count == 0U,
                                    assessment.static_evidence,
                                    assessment.entry_evidence, assessment.rejected_evidence,
                                    validation);
    if (exact_count == 0U)
        collect_direct_call_evidence(observed, function_map, process_function_map,
                                     assessment.static_evidence);
    for (const auto& item : assessment.static_evidence)
    {
        if (item.source == FunctionDiscoverySource::ObservedIndirectTarget)
            continue;
        FunctionEntryEvidenceKind kind = FunctionEntryEvidenceKind::BoundedCfgCandidate;
        FunctionEntryEvidenceStrength strength = FunctionEntryEvidenceStrength::Supporting;
        if (item.source == FunctionDiscoverySource::DynamicSymbol)
        {
            kind = FunctionEntryEvidenceKind::DynamicSymbolFunction;
            strength = FunctionEntryEvidenceStrength::Exact;
        }
        else if (item.source == FunctionDiscoverySource::DirectCall)
        {
            kind = FunctionEntryEvidenceKind::DirectCallTarget;
            strength = FunctionEntryEvidenceStrength::Exact;
        }
        else if (item.source == FunctionDiscoverySource::RelocationReference)
        {
            kind = FunctionEntryEvidenceKind::RelocationFunctionTarget;
            strength = FunctionEntryEvidenceStrength::Exact;
        }
        else if (item.source == FunctionDiscoverySource::Export)
        {
            kind = FunctionEntryEvidenceKind::ExportedFunction;
            strength = FunctionEntryEvidenceStrength::Exact;
        }
        add_entry_evidence(assessment.entry_evidence,
                           FunctionEntryEvidence{kind, strength, observed.source_module,
                                                  observed.source_pc, observed.target_module,
                                                  observed.target, std::nullopt, std::nullopt,
                                                  std::nullopt, std::nullopt, {},
                                                  strength == FunctionEntryEvidenceStrength::Exact,
                                                  false, item.detail});
    }
    normalize_entry_evidence(assessment.entry_evidence);
    if (exact_count > 1U)
    {
        decision.kind = IndirectTargetDecisionKind::CrossModuleAmbiguity;
        decision.reason = "target is an exact entry in more than one process-map module";
        validation.ownership = IndirectTargetOwnership::CrossModuleAmbiguity;
        return complete_assessment(std::move(assessment));
    }
    if (exact_count == 1U)
    {
        const auto exact = std::find_if(maps.begin(), maps.end(),
                                        [](const auto& item) { return item.exact != nullptr; });
        const auto* record = exact->exact;
        validation.existing_canonical_entry = record->canonical_entry;
        validation.ownership = record->canonical_entry == observed.target
                                   ? IndirectTargetOwnership::TrustedExistingEntry
                                   : IndirectTargetOwnership::ExistingSecondaryEntry;
        validation.cfg = record->cfg;
        validation.cfg_status = record->cfg
                                    ? IndirectTargetCFGStatus::ExistingTrustedCFG
                                    : IndirectTargetCFGStatus::NotAnalyzed;
        if (record->cfg) account_cfg(record->cfg.value(), validation);
        validation.candidate_owned_code_ranges = record->owned_code_ranges;
        add_record_evidence(record, assessment.static_evidence);
        normalize_evidence(assessment.static_evidence);
        decision.kind = validation.ownership == IndirectTargetOwnership::TrustedExistingEntry
                            ? IndirectTargetDecisionKind::TrustedExistingEntry
                            : IndirectTargetDecisionKind::AliasOfExistingEntry;
        decision.confidence = record->confidence;
        decision.eligible_for_promotion = true;
        decision.canonical_entry = record->canonical_entry;
        decision.reason = "target is already represented by a finalized trusted function record";
        if (record->entry_trust_status == FunctionEntryTrustStatus::Conflict ||
            record->translation_status == TranslationStatus::Conflict)
        {
            decision.kind = IndirectTargetDecisionKind::FunctionBoundaryConflict;
            decision.eligible_for_promotion = false;
            decision.reason = "exact map entry is marked as a function ownership conflict";
        }
        else if (record->entry_trust_status != FunctionEntryTrustStatus::Trusted || !record->cfg)
        {
            decision.kind = IndirectTargetDecisionKind::InsufficientEvidence;
            decision.eligible_for_promotion = false;
            decision.reason = record->entry_trust_status != FunctionEntryTrustStatus::Trusted
                                  ? "exact map entry is only a candidate and has not been certified"
                                  : "exact trusted seed has no finalized bounded CFG";
            add_rejection(assessment.rejected_evidence,
                          FunctionEntryEvidenceRejection{
                              FunctionEntryEvidenceRejectionKind::InsufficientEvidence,
                              FunctionEntryEvidenceKind::BoundedCfgCandidate, record->module,
                              record->canonical_entry,
                              "candidate record is not a trusted function-entry boundary"});
        }
        return complete_assessment(std::move(assessment));
    }

    const auto target_map = std::find_if(maps.begin(), maps.end(),
                                         [](const auto& item) { return item.map != nullptr; });
    const FinalizedFunctionMap* analysis_map =
        target_map == maps.end() ? function_map : target_map->map;
    if (analysis_map != nullptr)
    {
        const auto* precise_owner = find_precise_owner(*analysis_map, observed.target);
        if (precise_owner != nullptr)
        {
            validation.ownership = IndirectTargetOwnership::InsideExistingFunction;
            validation.existing_canonical_entry = precise_owner->canonical_entry;
            add_record_evidence(precise_owner, assessment.static_evidence);
            normalize_evidence(assessment.static_evidence);
            decision.kind = IndirectTargetDecisionKind::InsideExistingFunction;
            decision.reason = "target lies in precise owned code ranges of an existing function";
            return complete_assessment(std::move(assessment));
        }
        for (const auto& function : analysis_map->functions())
        {
            if (inside_display_envelope(function, observed.target))
            {
                validation.ownership = IndirectTargetOwnership::ExistingDisplayEnvelopeOnly;
                break;
            }
        }
    }

    AnalysisOptions cfg_options = options.cfg;
    configure_cfg_options(cfg_options, options, analysis_map, observed.target);
    if (cfg_options.max_instructions == 0U || cfg_options.max_basic_blocks == 0U ||
        cfg_options.max_pending_targets == 0U)
    {
        decision.kind = IndirectTargetDecisionKind::BudgetExceeded;
        decision.reason = "candidate CFG budget is zero";
        validation.cfg_status = IndirectTargetCFGStatus::BudgetExceeded;
        validation.analysis_error = make_error(ErrorCode::AnalysisBudgetExceeded,
                                                "candidate CFG budget is zero");
        return complete_assessment(std::move(assessment));
    }
    const auto graph = analyze_control_flow(memory, observed.target, cfg_options);
    if (!graph)
    {
        validation.analysis_error = graph.error();
        validation.cfg_status = is_budget_error(graph.error().code)
                                    ? IndirectTargetCFGStatus::BudgetExceeded
                                    : IndirectTargetCFGStatus::AnalysisFailed;
        decision.kind = is_budget_error(graph.error().code)
                            ? IndirectTargetDecisionKind::BudgetExceeded
                            : IndirectTargetDecisionKind::AnalysisFailed;
        decision.reason = "bounded CFG analysis failed: " + graph.error().message;
        return complete_assessment(std::move(assessment));
    }
    for (const auto& [unused, block] : graph.value().blocks)
    {
        (void)unused;
        for (const auto& instruction : block.instructions)
        {
            if (instruction.id == aarch64::InstructionId::Unknown)
            {
                validation.analysis_error = make_error(
                    ErrorCode::DecodeFailed,
                    "candidate CFG contains an undecoded instruction at " +
                        hex_address(instruction.address));
                validation.cfg_status = IndirectTargetCFGStatus::AnalysisFailed;
                decision.kind = IndirectTargetDecisionKind::AnalysisFailed;
                decision.reason = validation.analysis_error->message;
                return complete_assessment(std::move(assessment));
            }
        }
    }
    validation.cfg = graph.value();
    account_cfg(graph.value(), validation);
    if (validation.cfg_status == IndirectTargetCFGStatus::BudgetExceeded)
    {
        decision.kind = IndirectTargetDecisionKind::BudgetExceeded;
        decision.reason = validation.analysis_error
                              ? validation.analysis_error->message
                              : "candidate CFG accounting exceeded its budget";
        return complete_assessment(std::move(assessment));
    }
    const std::vector<GuestAddress> instruction_addresses = [&]() {
        std::vector<GuestAddress> addresses;
        addresses.reserve(graph.value().instruction_count);
        for (const auto& [unused, block] : graph.value().blocks)
        {
            (void)unused;
            for (const auto& instruction : block.instructions)
                addresses.push_back(instruction.address);
        }
        return addresses;
    }();
    const auto owned = normalize_code_ranges(instruction_addresses);
    if (!owned || owned.value().empty())
    {
        validation.analysis_error = owned ? make_error(ErrorCode::InvalidControlFlow,
                                                        "candidate CFG has no instruction ownership")
                                          : owned.error();
        validation.cfg_status = IndirectTargetCFGStatus::AnalysisFailed;
        decision.kind = IndirectTargetDecisionKind::AnalysisFailed;
        decision.reason = validation.analysis_error->message;
        return complete_assessment(std::move(assessment));
    }
    validation.candidate_owned_code_ranges = owned.value();
    add_entry_evidence(assessment.entry_evidence,
                       FunctionEntryEvidence{FunctionEntryEvidenceKind::BoundedCfgCandidate,
                                             FunctionEntryEvidenceStrength::Supporting,
                                             validation.target_module, observed.target,
                                             validation.target_module, observed.target,
                                             std::nullopt, std::nullopt, std::nullopt,
                                             std::nullopt, {}, false, false,
                                             "bounded CFG decoded within configured limits"});
    bool reconciled_boundary = false;
    if (analysis_map != nullptr)
    {
        std::vector<const FunctionRecord*> overlapping_functions;
        for (const auto& function : analysis_map->functions())
        {
            if (!owned_ranges_overlap(validation.candidate_owned_code_ranges,
                                       function.owned_code_ranges))
                continue;
            overlapping_functions.push_back(&function);
        }
        if (!overlapping_functions.empty())
        {
            const auto* function = overlapping_functions.front();
            const auto overlap = intersect_owned_ranges(validation.candidate_owned_code_ranges,
                                                        function->owned_code_ranges);
            if (overlap) validation.overlap_ranges = overlap.value();
            validation.boundary_reconciliation.kind =
                FunctionBoundaryReconciliationKind::IncompatiblePreciseOverlap;
            validation.boundary_reconciliation.existing_canonical_entry =
                function->canonical_entry;
            validation.boundary_reconciliation.existing_owned_code_ranges_before =
                function->owned_code_ranges;
            validation.boundary_reconciliation.candidate_owned_code_ranges_before =
                validation.candidate_owned_code_ranges;
            validation.boundary_reconciliation.precise_overlap_ranges = validation.overlap_ranges;
            validation.boundary_reconciliation.existing_cfg_before = function->cfg;
            validation.boundary_reconciliation.candidate_cfg_before = graph.value();

            if (overlapping_functions.size() == 1U && overlap && !overlap.value().empty())
            {
                const auto reconciliation = reconcile_analyzer_overclaim(
                    *function, graph.value(), validation.candidate_owned_code_ranges, memory,
                    *analysis_map, observed.target, options);
                if (reconciliation)
                {
                    validation.boundary_reconciliation = reconciliation->reconciliation;
                    validation.cfg = reconciliation->candidate_cfg;
                    validation.candidate_owned_code_ranges =
                        validation.boundary_reconciliation.candidate_owned_code_ranges_after;
                    validation.overlap_ranges.clear();
                    validation.ownership = IndirectTargetOwnership::NewEntry;
                    validation.direct_call_targets.clear();
                    validation.unresolved_control_flow.clear();
                    validation.edges = 0U;
                    account_cfg(validation.cfg.value(), validation);
                    reconciled_boundary = true;
                }
            }
            if (!reconciled_boundary)
            {
                validation.ownership = IndirectTargetOwnership::CandidateOverlap;
                validation.boundary_reconciliation.reason =
                    "no complete direct-call/tail-transfer boundary proof reconciles the precise overlap";
                decision.kind = IndirectTargetDecisionKind::FunctionBoundaryConflict;
                decision.confidence = FunctionConfidence::Conflict;
                decision.reason =
                    "candidate CFG overlaps precise ownership of existing function " +
                    hex_address(function->canonical_entry);
                return complete_assessment(std::move(assessment));
            }
        }
    }

    normalize_evidence(assessment.static_evidence);
    const auto has_exact_static_evidence = std::any_of(
        assessment.entry_evidence.begin(), assessment.entry_evidence.end(), [](const auto& item) {
            return item.strength == FunctionEntryEvidenceStrength::Exact &&
                   item.kind != FunctionEntryEvidenceKind::ObservedIndirectCall &&
                   item.kind != FunctionEntryEvidenceKind::GuestLoadedPointer &&
                   item.kind != FunctionEntryEvidenceKind::BoundedCfgCandidate;
        });
    if (options.require_independent_static_evidence && !has_exact_static_evidence)
    {
        decision.kind = IndirectTargetDecisionKind::InsufficientEvidence;
        decision.reason = "runtime observation and executable CFG are insufficient under the configured trust policy";
        validation.cfg_status = graph.value().unresolved.empty()
                                    ? IndirectTargetCFGStatus::Validated
                                    : IndirectTargetCFGStatus::ValidatedWithUnresolvedFlow;
        return complete_assessment(std::move(assessment));
    }
    if (!options.allow_runtime_cfg_promotion && !has_exact_static_evidence)
    {
        decision.kind = IndirectTargetDecisionKind::InsufficientEvidence;
        decision.reason = "runtime CFG promotion is disabled and no independent static evidence exists";
        validation.cfg_status = graph.value().unresolved.empty()
                                    ? IndirectTargetCFGStatus::Validated
                                    : IndirectTargetCFGStatus::ValidatedWithUnresolvedFlow;
        return complete_assessment(std::move(assessment));
    }
    if (validation.ownership != IndirectTargetOwnership::ExistingDisplayEnvelopeOnly)
        validation.ownership = IndirectTargetOwnership::NewEntry;
    validation.cfg_status = validation.cfg && validation.cfg->unresolved.empty()
                                ? IndirectTargetCFGStatus::Validated
                                : IndirectTargetCFGStatus::ValidatedWithUnresolvedFlow;
    decision.kind = IndirectTargetDecisionKind::TrustedNewEntry;
    decision.eligible_for_promotion = true;
    decision.confidence = assessment.static_evidence.empty() ? FunctionConfidence::Medium
                                                              : FunctionConfidence::High;
    for (const auto& item : assessment.static_evidence)
    {
        if (item.source == FunctionDiscoverySource::DynamicSymbol)
        {
            decision.confidence = FunctionConfidence::Confirmed;
            break;
        }
    }
    decision.canonical_entry = observed.target;
    decision.reason = reconciled_boundary
                          ? "observed indirect target passed typed analyzer-overclaim reconciliation; "
                            "the separately discovered direct-call helper is excluded from candidate ownership"
                      : validation.pointer_slot_relocation_found &&
                              validation.pointer_slot_value_verified
                          ? (validation.pointer_target_declared_function
                                 ? "observed indirect call plus verified relocation-backed function target and bounded CFG"
                                 : "observed indirect call plus verified rebased guest slot and bounded CFG; relocation alone is not function proof")
                          : (graph.value().unresolved.empty()
                                 ? "runtime observation plus bounded executable CFG has no ownership conflict"
                                 : "runtime observation plus bounded executable CFG is structurally valid; unresolved indirect flow remains explicit");
    return complete_assessment(std::move(assessment));
}

Result<FunctionMapRefinement> refine_function_map(
    const FinalizedFunctionMap& existing, const ModuleAnalysisInput& input,
    const ObservedIndirectTarget& observed, const IndirectTargetDiscoveryOptions& options)
{
    FunctionMapRefinement result;
    const auto assessment = assess_indirect_target(observed, *input.memory, &existing, nullptr,
                                                   nullptr, options);
    if (!assessment)
        return Result<FunctionMapRefinement>::failure(assessment.error());
    result.assessment = assessment.value();
    result.map = existing;
    if (!result.assessment.decision.eligible_for_promotion ||
        result.assessment.decision.kind != IndirectTargetDecisionKind::TrustedNewEntry)
        return Result<FunctionMapRefinement>::success(std::move(result));
    if (input.identity.module != result.assessment.validation.target_module)
    {
        mark_refinement_failure(result.assessment, make_error(
            ErrorCode::InvalidArgument,
            "candidate owner does not match the module-analysis input"));
        return Result<FunctionMapRefinement>::success(std::move(result));
    }
    auto seeds = input.seeds;
    seeds.push_back(FunctionSeed{observed.target, FunctionDiscoverySource::ObservedIndirectTarget,
                                 result.assessment.decision.confidence, std::nullopt, std::nullopt,
                                 "immutable refinement from runtime indirect target at " +
                                     hex_address(observed.source_pc)});
    auto rebuild_options = map_options(options);
    set_rebuild_roots(rebuild_options, seeds);
    rebuild_options.reuse_map = &existing;
    rebuild_options.newly_introduced_function_entries.insert(observed.target);
    for (const auto target : result.assessment.validation.direct_call_targets)
        rebuild_options.newly_introduced_function_entries.insert(target);
    const auto rebuilt = FunctionMapBuilder::build(
        ModuleAnalysisInput{input.identity, input.memory, std::move(seeds)}, rebuild_options);
    if (!rebuilt)
    {
        mark_refinement_failure(result.assessment, rebuilt.error());
        return Result<FunctionMapRefinement>::success(std::move(result));
    }
    const auto* record = rebuilt.value().find_exact_entry(observed.target);
    if (record == nullptr || record->translation_status == TranslationStatus::Conflict ||
        !record->cfg ||
        !boundary_refinement_matches(
            rebuilt.value(), result.assessment.validation.boundary_reconciliation,
            observed.target))
    {
        mark_refinement_failure(
            result.assessment,
            make_error(ErrorCode::FunctionBoundaryConflict,
                       "refined map did not produce a non-conflicting trusted candidate record"));
        return Result<FunctionMapRefinement>::success(std::move(result));
    }
    result.map = std::move(rebuilt).value();
    result.analysis_work = analysis_work_from_accounting(result.map.accounting());
    result.assessment.decision.kind = IndirectTargetDecisionKind::TrustedNewEntry;
    result.assessment.decision.canonical_entry = record->canonical_entry;
    result.assessment.decision.promoted = true;
    result.assessment.decision.reason = "new immutable finalized function record was built and validated";
    return Result<FunctionMapRefinement>::success(std::move(result));
}

Result<ProcessFunctionMapRefinement> refine_process_function_map(
    const ProcessFunctionMap& existing, const ProcessImage& process_image,
    const ObservedIndirectTarget& observed, const IndirectTargetDiscoveryOptions& options)
{
    ProcessFunctionMapRefinement result;
    const auto assessment = assess_indirect_target(
        observed, process_image.memory(), nullptr, &existing, &process_image, options);
    if (!assessment)
        return Result<ProcessFunctionMapRefinement>::failure(assessment.error());
    result.assessment = assessment.value();
    result.map = existing;
    if (!result.assessment.decision.eligible_for_promotion ||
        result.assessment.decision.kind != IndirectTargetDecisionKind::TrustedNewEntry)
        return Result<ProcessFunctionMapRefinement>::success(std::move(result));
    if (result.assessment.validation.target_module.empty())
    {
        mark_refinement_failure(result.assessment, make_error(
            ErrorCode::InvalidCrossModuleTransfer,
            "candidate has no unique process-module owner"));
        return Result<ProcessFunctionMapRefinement>::success(std::move(result));
    }

    std::vector<FinalizedFunctionMap> maps;
    maps.reserve(std::max(existing.maps().size(), process_image.modules().size()));
    bool target_module_rebuilt = false;
    for (const auto& existing_map : existing.maps())
    {
        const auto* module = process_image.module(existing_map.identity().module);
        if (module == nullptr)
        {
            mark_refinement_failure(result.assessment, make_error(
                ErrorCode::InvalidArgument,
                "existing process map has no corresponding process-image module"));
            return Result<ProcessFunctionMapRefinement>::success(std::move(result));
        }
        if (existing_map.identity().module != result.assessment.validation.target_module)
        {
            // Finalized maps are frozen values. Reusing this value preserves
            // the previous module generation exactly while avoiding another
            // CFG/function reconstruction for a logically unchanged module.
            maps.push_back(existing_map);
            ++result.module_maps_reused;
            continue;
        }
        target_module_rebuilt = true;
        auto seeds = seeds_from_finalized_map(existing_map);
        seeds.push_back(FunctionSeed{
            observed.target, FunctionDiscoverySource::ObservedIndirectTarget,
            result.assessment.decision.confidence, std::nullopt, std::nullopt,
            "immutable process refinement from runtime indirect target at " +
                hex_address(observed.source_pc)});
        auto rebuild_options = map_options(options);
        set_rebuild_roots(rebuild_options, seeds);
        rebuild_options.reuse_map = &existing_map;
        rebuild_options.newly_introduced_function_entries.insert(observed.target);
        for (const auto target : result.assessment.validation.direct_call_targets)
            rebuild_options.newly_introduced_function_entries.insert(target);
        const auto rebuilt = FunctionMapBuilder::build(
            ModuleAnalysisInput{module->identity, &process_image.memory(), std::move(seeds)},
            rebuild_options);
        if (!rebuilt)
        {
            mark_refinement_failure(result.assessment, rebuilt.error());
            return Result<ProcessFunctionMapRefinement>::success(std::move(result));
        }
        maps.push_back(std::move(rebuilt).value());
        ++result.module_maps_rebuilt;
    }
    if (!target_module_rebuilt)
    {
        const auto* target_module = process_image.module(
            result.assessment.validation.target_module);
        if (target_module == nullptr)
        {
            mark_refinement_failure(result.assessment, make_error(
                ErrorCode::InvalidCrossModuleTransfer,
                "candidate target module is not present in the process image"));
            return Result<ProcessFunctionMapRefinement>::success(std::move(result));
        }
        std::vector<FunctionSeed> seeds{FunctionSeed{
            observed.target, FunctionDiscoverySource::ObservedIndirectTarget,
            result.assessment.decision.confidence, std::nullopt, std::nullopt,
            "immutable process refinement from runtime indirect target at " +
                hex_address(observed.source_pc)}};
        auto rebuild_options = map_options(options);
        set_rebuild_roots(rebuild_options, seeds);
        rebuild_options.newly_introduced_function_entries.insert(observed.target);
        for (const auto target : result.assessment.validation.direct_call_targets)
            rebuild_options.newly_introduced_function_entries.insert(target);
        const auto rebuilt = FunctionMapBuilder::build(
            ModuleAnalysisInput{target_module->identity, &process_image.memory(), std::move(seeds)},
            rebuild_options);
        if (!rebuilt)
        {
            mark_refinement_failure(result.assessment, rebuilt.error());
            return Result<ProcessFunctionMapRefinement>::success(std::move(result));
        }
        maps.push_back(std::move(rebuilt).value());
        ++result.module_maps_rebuilt;
    }
    const auto rebuilt = ProcessFunctionMap::build(std::move(maps));
    if (!rebuilt)
    {
        mark_refinement_failure(result.assessment, rebuilt.error());
        return Result<ProcessFunctionMapRefinement>::success(std::move(result));
    }
    const auto* record = rebuilt.value().find(observed.target);
    const auto* rebuilt_module = rebuilt.value().map_for(observed.target);
    if (record == nullptr || record->translation_status == TranslationStatus::Conflict ||
        !record->cfg ||
        rebuilt_module == nullptr ||
        !boundary_refinement_matches(
            *rebuilt_module, result.assessment.validation.boundary_reconciliation,
            observed.target))
    {
        mark_refinement_failure(
            result.assessment,
            make_error(ErrorCode::FunctionBoundaryConflict,
                       "refined process map did not produce a non-conflicting candidate record"));
        return Result<ProcessFunctionMapRefinement>::success(std::move(result));
    }
    result.map = std::move(rebuilt).value();
    if (const auto* rebuilt_module = result.map.map_for(observed.target))
        result.analysis_work = analysis_work_from_accounting(rebuilt_module->accounting());
    result.assessment.decision.kind = IndirectTargetDecisionKind::TrustedNewEntry;
    result.assessment.decision.canonical_entry = record->canonical_entry;
    result.assessment.decision.promoted = true;
    result.assessment.decision.reason = "new immutable process function record was built and validated";
    return Result<ProcessFunctionMapRefinement>::success(std::move(result));
}

IndirectTargetCandidateIdentity indirect_target_candidate_identity(
    const ObservedIndirectTarget& observed)
{
    return IndirectTargetCandidateIdentity{observed.target_module,
                                          observed.target,
                                          observed.source_module,
                                          observed.source_function,
                                          observed.source_pc,
                                          observed.control_flow,
                                          observed.target_register,
                                          observed.pointer_provenance,
                                          observed.guest_load_address};
}

std::string_view indirect_target_refinement_budget_dimension_name(
    IndirectTargetRefinementBudgetDimension dimension) noexcept
{
    switch (dimension)
    {
    case IndirectTargetRefinementBudgetDimension::None: return "none";
    case IndirectTargetRefinementBudgetDimension::StagnantRounds:
        return "stagnant_rounds";
    case IndirectTargetRefinementBudgetDimension::UniqueCandidates:
        return "unique_candidates";
    case IndirectTargetRefinementBudgetDimension::CandidateAssessments:
        return "candidate_assessments";
    case IndirectTargetRefinementBudgetDimension::Promotions: return "promotions";
    case IndirectTargetRefinementBudgetDimension::MapRebuilds: return "map_rebuilds";
    case IndirectTargetRefinementBudgetDimension::RefinementAnalysisFunctions:
        return "refinement_analysis_functions";
    case IndirectTargetRefinementBudgetDimension::RefinementAnalysisReanalyzedFunctions:
        return "refinement_analysis_reanalyzed_functions";
    case IndirectTargetRefinementBudgetDimension::RefinementAnalysisInstructions:
        return "refinement_analysis_instructions";
    case IndirectTargetRefinementBudgetDimension::RefinementAnalysisBlocks:
        return "refinement_analysis_blocks";
    case IndirectTargetRefinementBudgetDimension::RefinementAnalysisEdges:
        return "refinement_analysis_edges";
    case IndirectTargetRefinementBudgetDimension::RefinementAnalysisBytes:
        return "refinement_analysis_bytes";
    case IndirectTargetRefinementBudgetDimension::RefinementAnalysisBoundaryFinalizationPasses:
        return "refinement_analysis_boundary_finalization_passes";
    case IndirectTargetRefinementBudgetDimension::RefinementAnalysisInvalidatedRecords:
        return "refinement_analysis_invalidated_records";
    case IndirectTargetRefinementBudgetDimension::RefinementAnalysisTransactions:
        return "refinement_analysis_transactions";
    }
    return "unknown";
}

std::string_view indirect_target_refinement_analysis_dimension_name(
    IndirectTargetRefinementAnalysisDimension dimension) noexcept
{
    switch (dimension)
    {
    case IndirectTargetRefinementAnalysisDimension::None: return "none";
    case IndirectTargetRefinementAnalysisDimension::FunctionsAnalyzed:
        return "functions_analyzed";
    case IndirectTargetRefinementAnalysisDimension::FunctionsReanalyzed:
        return "functions_reanalyzed";
    case IndirectTargetRefinementAnalysisDimension::Instructions: return "instructions";
    case IndirectTargetRefinementAnalysisDimension::Blocks: return "blocks";
    case IndirectTargetRefinementAnalysisDimension::Edges: return "edges";
    case IndirectTargetRefinementAnalysisDimension::BytesAnalyzed: return "bytes_analyzed";
    case IndirectTargetRefinementAnalysisDimension::BoundaryFinalizationPasses:
        return "boundary_finalization_passes";
    case IndirectTargetRefinementAnalysisDimension::InvalidatedRecords:
        return "invalidated_records";
    case IndirectTargetRefinementAnalysisDimension::Transactions: return "transactions";
    }
    return "unknown";
}

IndirectTargetRefinementWorklist::IndirectTargetRefinementWorklist(
    IndirectTargetRefinementBudgets budgets)
    : budgets_(budgets)
{
    counters_.configured = budgets_;
    counters_.analysis.configured = budgets_.analysis;
}

IndirectTargetRefinementObservationResult IndirectTargetRefinementWorklist::observe(
    const IndirectTargetAssessment& assessment)
{
    IndirectTargetRefinementObservationResult result;
    const auto& observed = assessment.observed;
    counters_.observations_received += observed.observation_count;
    if (assessment.validation.boundary_reconciliation.kind !=
        FunctionBoundaryReconciliationKind::None)
        ++counters_.boundary_reconciliations;

    auto normalized = observed;
    if (normalized.target_module.empty())
        normalized.target_module = assessment.validation.target_module;
    result.newly_unique_observation =
        unique_observations_.emplace(observation_identity_key(normalized), true).second;
    if (result.newly_unique_observation)
        ++counters_.unique_observations;
    else
        ++counters_.duplicate_coalesced_observations;
    const auto identity = indirect_target_candidate_identity(normalized);
    const bool trusted_existing =
        assessment.decision.kind == IndirectTargetDecisionKind::TrustedExistingEntry ||
        assessment.decision.kind == IndirectTargetDecisionKind::AliasOfExistingEntry;
    const bool candidate_observation = !trusted_existing;

    if (!candidate_observation)
    {
        ++counters_.existing_trusted_hits;
        return result;
    }

    const auto target_identity = std::make_pair(identity.target_module, identity.target);
    const bool newly_unique_target =
        unique_target_candidates_.find(target_identity) == unique_target_candidates_.end();
    auto item = work_items_.find(identity);
    if (item == work_items_.end())
    {
        if (newly_unique_target && counters_.unique_candidates >= budgets_.max_unique_candidates)
        {
            counters_.exhaustion = IndirectTargetRefinementExhaustion{
                IndirectTargetRefinementBudgetDimension::UniqueCandidates,
                counters_.unique_candidates, budgets_.max_unique_candidates, {}, 0U,
                std::nullopt};
            overflow_pending_ = identity;
            result.accepted = false;
            return result;
        }
        if (newly_unique_target)
        {
            unique_target_candidates_.emplace(target_identity, true);
            ++counters_.unique_candidates;
            result.newly_unique_candidate = true;
            round_productive_ = round_active_ || round_productive_;
        }
        WorkItem work;
        work.observed = std::move(normalized);
        work.observed.observation_count = observed.observation_count;
        add_observation_provenance(work.observed.observation_provenance,
                                   observation_provenance(observed));
        work.pending = assessment.decision.eligible_for_promotion &&
                       assessment.decision.kind == IndirectTargetDecisionKind::TrustedNewEntry;
        work.processed = !work.pending;
        work.processed_generation = map_generation_;
        work_items_.emplace(identity, std::move(work));
        if (!assessment.decision.eligible_for_promotion)
        {
            ++counters_.rejected_candidates;
            ++counters_.terminal_resolutions;
        }
        return result;
    }

    auto& work = item->second;
    const auto prior_observation_count = work.observed.observation_count;
    if (std::numeric_limits<std::size_t>::max() - prior_observation_count >=
        observed.observation_count)
        work.observed.observation_count += observed.observation_count;
    else
        work.observed.observation_count = std::numeric_limits<std::size_t>::max();
    for (const auto& evidence : observed.static_evidence)
        add_evidence(work.observed.static_evidence, evidence);
    for (const auto& evidence : observed.entry_evidence)
        add_entry_evidence(work.observed.entry_evidence, evidence);
    add_observation_provenance(work.observed.observation_provenance,
                               observation_provenance(observed));
    for (const auto& provenance : observed.observation_provenance)
        add_observation_provenance(work.observed.observation_provenance, provenance);
    normalize_evidence(work.observed.static_evidence);
    normalize_entry_evidence(work.observed.entry_evidence);
    if (observed_order_less(observed, work.observed))
    {
        const auto count = work.observed.observation_count;
        const auto static_evidence = work.observed.static_evidence;
        const auto entry_evidence = work.observed.entry_evidence;
        const auto history = work.observed.observation_provenance;
        work.observed = std::move(normalized);
        work.observed.observation_count = count;
        work.observed.static_evidence = static_evidence;
        work.observed.entry_evidence = entry_evidence;
        work.observed.observation_provenance = history;
    }

    if (assessment.decision.kind == IndirectTargetDecisionKind::TrustedExistingEntry ||
        assessment.decision.kind == IndirectTargetDecisionKind::AliasOfExistingEntry)
    {
        ++counters_.existing_trusted_hits;
        if (work.pending || !work.processed)
        {
            ++counters_.terminal_resolutions;
            round_productive_ = round_active_ || round_productive_;
        }
        work.pending = false;
        work.processed = true;
        work.processed_generation = map_generation_;
        return result;
    }
    if (assessment.decision.eligible_for_promotion &&
        assessment.decision.kind == IndirectTargetDecisionKind::TrustedNewEntry)
    {
        if (!work.pending && work.processed && work.processed_generation < map_generation_)
        {
            work.pending = true;
            result.reconsidered_after_map_change = true;
            ++counters_.candidates_reconsidered_after_map_change;
            round_productive_ = round_active_ || round_productive_;
        }
        else if (!work.processed)
        {
            work.pending = true;
            round_productive_ = round_active_ || round_productive_;
        }
    }
    else
    {
        const bool was_unresolved = work.pending || !work.processed;
        if (was_unresolved)
        {
            ++counters_.terminal_resolutions;
            round_productive_ = round_active_ || round_productive_;
        }
        ++counters_.rejected_candidates;
        work.pending = false;
        work.processed = true;
        work.processed_generation = map_generation_;
    }
    return result;
}

bool IndirectTargetRefinementWorklist::begin_round() noexcept
{
    if (counters_.exhaustion.dimension != IndirectTargetRefinementBudgetDimension::None)
        return false;
    if (round_active_) return false;
    ++counters_.total_execution_attempts;
    round_active_ = true;
    round_productive_ = false;
    return true;
}

void IndirectTargetRefinementWorklist::end_round() noexcept
{
    if (!round_active_) return;
    if (round_productive_)
    {
        ++counters_.productive_rounds;
        counters_.stagnant_rounds = 0U;
    }
    else
    {
        ++counters_.stagnant_rounds;
        if (counters_.stagnant_rounds >= budgets_.max_stagnant_rounds &&
            counters_.exhaustion.dimension == IndirectTargetRefinementBudgetDimension::None)
        {
            counters_.exhaustion = IndirectTargetRefinementExhaustion{
                IndirectTargetRefinementBudgetDimension::StagnantRounds,
                counters_.stagnant_rounds, budgets_.max_stagnant_rounds, {}, 0U, std::nullopt};
        }
    }
    round_active_ = false;
    round_productive_ = false;
}

bool IndirectTargetRefinementWorklist::begin_candidate_assessment(
    const IndirectTargetCandidateIdentity& candidate) noexcept
{
    if (counters_.exhaustion.dimension != IndirectTargetRefinementBudgetDimension::None)
        return false;
    if (counters_.candidate_assessments >= budgets_.max_candidate_assessments)
    {
        counters_.exhaustion = IndirectTargetRefinementExhaustion{
            IndirectTargetRefinementBudgetDimension::CandidateAssessments,
            counters_.candidate_assessments, budgets_.max_candidate_assessments, {}, 0U,
            std::nullopt};
        return false;
    }
    ++counters_.candidate_assessments;
    counters_.last_processed_candidate = candidate;
    return true;
}

bool IndirectTargetRefinementWorklist::can_promote() noexcept
{
    if (counters_.exhaustion.dimension != IndirectTargetRefinementBudgetDimension::None)
        return false;
    const bool legacy_limits_enabled = budgets_.legacy_event_limits ||
                                       budgets_.max_promotions != 128U ||
                                       budgets_.max_map_rebuilds != 128U;
    if (legacy_limits_enabled && counters_.successful_promotions >= budgets_.max_promotions)
    {
        counters_.exhaustion = IndirectTargetRefinementExhaustion{
            IndirectTargetRefinementBudgetDimension::Promotions,
            counters_.successful_promotions, budgets_.max_promotions, {}, 0U, std::nullopt};
        return false;
    }
    if (legacy_limits_enabled && counters_.map_rebuilds >= budgets_.max_map_rebuilds)
    {
        counters_.exhaustion = IndirectTargetRefinementExhaustion{
            IndirectTargetRefinementBudgetDimension::MapRebuilds,
            counters_.map_rebuilds, budgets_.max_map_rebuilds, {}, 0U, std::nullopt};
        return false;
    }
    return true;
}

bool IndirectTargetRefinementWorklist::can_commit_refinement(
    const IndirectTargetCandidateIdentity& candidate,
    const IndirectTargetRefinementAnalysisWork& work) noexcept
{
    if (counters_.exhaustion.dimension != IndirectTargetRefinementBudgetDimension::None)
        return false;
    if (!can_promote()) return false;

    const auto exceeds = [](auto consumed, auto delta, auto limit) {
        return delta > limit || consumed > limit - delta;
    };
    const auto fail = [&](IndirectTargetRefinementBudgetDimension dimension, std::size_t consumed,
                          std::size_t limit) {
        counters_.exhaustion = IndirectTargetRefinementExhaustion{
            dimension, consumed, limit, work.module, counters_.map_rebuilds, candidate};
        return false;
    };
    const auto& configured = budgets_.analysis;
    const auto& consumed = counters_.analysis;
    if (exceeds(consumed.functions_analyzed, work.functions_analyzed,
                configured.max_functions_analyzed))
        return fail(IndirectTargetRefinementBudgetDimension::RefinementAnalysisFunctions,
                    consumed.functions_analyzed, configured.max_functions_analyzed);
    if (exceeds(consumed.functions_reanalyzed, work.functions_reanalyzed,
                configured.max_functions_reanalyzed))
        return fail(IndirectTargetRefinementBudgetDimension::RefinementAnalysisReanalyzedFunctions,
                    consumed.functions_reanalyzed, configured.max_functions_reanalyzed);
    if (exceeds(consumed.instructions, work.instructions, configured.max_instructions))
        return fail(IndirectTargetRefinementBudgetDimension::RefinementAnalysisInstructions,
                    consumed.instructions, configured.max_instructions);
    if (exceeds(consumed.blocks, work.blocks, configured.max_blocks))
        return fail(IndirectTargetRefinementBudgetDimension::RefinementAnalysisBlocks,
                    consumed.blocks, configured.max_blocks);
    if (exceeds(consumed.edges, work.edges, configured.max_edges))
        return fail(IndirectTargetRefinementBudgetDimension::RefinementAnalysisEdges,
                    consumed.edges, configured.max_edges);
    if (exceeds(consumed.bytes_analyzed, work.bytes_analyzed, configured.max_bytes_analyzed))
        return fail(IndirectTargetRefinementBudgetDimension::RefinementAnalysisBytes,
                    static_cast<std::size_t>(consumed.bytes_analyzed),
                    static_cast<std::size_t>(configured.max_bytes_analyzed));
    if (exceeds(consumed.boundary_finalization_passes, work.boundary_finalization_passes,
                configured.max_boundary_finalization_passes))
        return fail(IndirectTargetRefinementBudgetDimension::RefinementAnalysisBoundaryFinalizationPasses,
                    consumed.boundary_finalization_passes,
                    configured.max_boundary_finalization_passes);
    if (exceeds(consumed.invalidated_records, work.invalidated_records,
                configured.max_invalidated_records))
        return fail(IndirectTargetRefinementBudgetDimension::RefinementAnalysisInvalidatedRecords,
                    consumed.invalidated_records, configured.max_invalidated_records);
    if (exceeds(consumed.transactions, work.transactions, configured.max_transactions))
        return fail(IndirectTargetRefinementBudgetDimension::RefinementAnalysisTransactions,
                    consumed.transactions, configured.max_transactions);
    return true;
}

void IndirectTargetRefinementWorklist::record_terminal_candidate(
    const IndirectTargetCandidateIdentity& candidate) noexcept
{
    const auto item = work_items_.find(candidate);
    if (item == work_items_.end()) return;
    const bool was_unresolved = item->second.pending || !item->second.processed;
    item->second.pending = false;
    item->second.processed = true;
    item->second.processed_generation = map_generation_;
    if (was_unresolved)
    {
        ++counters_.terminal_resolutions;
        round_productive_ = round_active_ || round_productive_;
    }
}

void IndirectTargetRefinementWorklist::record_promotion(
    const IndirectTargetCandidateIdentity& candidate, std::size_t module_maps_rebuilt,
    std::size_t module_maps_reused,
    const IndirectTargetRefinementAnalysisWork& work) noexcept
{
    if (!can_promote()) return;
    if (!can_commit_refinement(candidate, work)) return;
    ++counters_.successful_promotions;
    ++counters_.map_rebuilds;
    counters_.module_maps_rebuilt += module_maps_rebuilt;
    counters_.module_maps_reused += module_maps_reused;
    counters_.analysis.functions_analyzed += work.functions_analyzed;
    counters_.analysis.functions_reanalyzed += work.functions_reanalyzed;
    counters_.analysis.functions_reused += work.functions_reused;
    counters_.analysis.instructions += work.instructions;
    counters_.analysis.blocks += work.blocks;
    counters_.analysis.edges += work.edges;
    counters_.analysis.bytes_analyzed += work.bytes_analyzed;
    counters_.analysis.boundary_finalization_passes += work.boundary_finalization_passes;
    counters_.analysis.invalidated_records += work.invalidated_records;
    counters_.analysis.transactions += work.transactions;
    const auto item = work_items_.find(candidate);
    if (item != work_items_.end())
    {
        item->second.pending = false;
        item->second.processed = true;
        item->second.processed_generation = map_generation_;
    }
    ++map_generation_;
    round_productive_ = round_active_ || round_productive_;
}

std::vector<ObservedIndirectTarget> IndirectTargetRefinementWorklist::pending_candidates() const
{
    std::vector<ObservedIndirectTarget> result;
    for (const auto& [unused, item] : work_items_)
    {
        (void)unused;
        if (item.pending) result.push_back(item.observed);
    }
    std::sort(result.begin(), result.end(), observed_order_less);
    return result;
}

IndirectTargetRefinementSummary IndirectTargetRefinementWorklist::summary() const
{
    auto result = counters_;
    const auto pending = pending_candidates();
    result.pending_candidate_count = pending.size() + (overflow_pending_ ? 1U : 0U);
    if (!pending.empty()) result.next_pending_candidate = indirect_target_candidate_identity(pending.front());
    else if (overflow_pending_) result.next_pending_candidate = overflow_pending_;
    return result;
}

bool IndirectTargetRefinementWorklist::exhausted() const noexcept
{
    return counters_.exhaustion.dimension != IndirectTargetRefinementBudgetDimension::None;
}

void sort_observed_indirect_targets(std::vector<ObservedIndirectTarget>& targets)
{
    std::sort(targets.begin(), targets.end(), [](const auto& left, const auto& right) {
        return std::tie(left.target_module, left.target, left.source_module, left.source_function,
                        left.source_pc, left.control_flow, left.target_register,
                        left.pointer_provenance, left.guest_load_address) <
               std::tie(right.target_module, right.target, right.source_module,
                        right.source_function, right.source_pc, right.control_flow,
                        right.target_register, right.pointer_provenance,
                        right.guest_load_address);
    });
}

} // namespace switchrecomp::analysis
