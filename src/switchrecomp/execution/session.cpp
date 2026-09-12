#include "switchrecomp/execution/session.hpp"

#include "switchrecomp/aarch64/decoder.hpp"
#include "switchrecomp/common/checked_arithmetic.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <bit>
#include <exception>
#include <iomanip>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <span>
#include <sstream>
#include <utility>

namespace switchrecomp::execution
{

namespace
{

using GuestAddress = memory::GuestAddress;
using json = nlohmann::json;

[[nodiscard]] std::string hex_address(GuestAddress address)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::setw(16) << std::setfill('0') << address;
    return output.str();
}

[[nodiscard]] bool profiling_enabled() noexcept
{
    const auto* value = std::getenv("SWITCHRECOMP_PROFILE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

class ProfileTimer
{
  public:
    explicit ProfileTimer(const char* name)
        : name_(name), enabled_(profiling_enabled()),
          start_(enabled_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
    ProfileTimer(const char* name, std::uint64_t* elapsed_us, std::size_t* calls)
        : name_(name), enabled_(profiling_enabled()), elapsed_us_(elapsed_us), calls_(calls),
          start_(enabled_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
    ~ProfileTimer() noexcept
    {
        if (!enabled_) return;
        const auto elapsed = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start_)
                .count());
        if (elapsed_us_ != nullptr)
        {
            *elapsed_us_ += elapsed;
            if (calls_ != nullptr) ++*calls_;
            return;
        }
        std::cerr << "[switchrecomp profile] phase=" << name_ << " elapsed_us=" << elapsed << "\n";
    }

  private:
    const char* name_;
    bool enabled_;
    std::uint64_t* elapsed_us_ = nullptr;
    std::size_t* calls_ = nullptr;
    std::chrono::steady_clock::time_point start_;
};

void profile_counter(const char* name, std::size_t value) noexcept
{
    if (profiling_enabled())
        std::cerr << "[switchrecomp profile] counter=" << name << " value=" << value << "\n";
}

[[nodiscard]] std::uint64_t cfg_identity(const analysis::ControlFlowGraph& cfg) noexcept
{
    std::uint64_t hash = 1469598103934665603ULL;
    const auto add = [&hash](std::uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ULL;
    };
    add(cfg.entry);
    for (const auto& [address, block] : cfg.blocks)
    {
        add(address);
        for (const auto& instruction : block.instructions)
        {
            add(instruction.address);
            add(instruction.opcode);
            add(static_cast<std::uint64_t>(instruction.id));
        }
        for (const auto& edge : block.successors)
        {
            add(edge.source);
            add(edge.target);
            add(static_cast<std::uint64_t>(edge.kind));
            add(edge.internal ? 1U : 0U);
        }
    }
    return hash;
}

[[nodiscard]] std::uint64_t independent_umulh_oracle(std::uint64_t left,
                                                     std::uint64_t right) noexcept
{
    // Four 32-bit partial products provide an independent report-time oracle;
    // this deliberately does not call the production UMULH helper.
    const auto left_low = static_cast<std::uint64_t>(static_cast<std::uint32_t>(left));
    const auto left_high = left >> 32U;
    const auto right_low = static_cast<std::uint64_t>(static_cast<std::uint32_t>(right));
    const auto right_high = right >> 32U;
    const auto low_product = left_low * right_low;
    const auto cross_low = left_low * right_high;
    const auto cross_high = left_high * right_low;
    const auto carry = (low_product >> 32U) +
                       static_cast<std::uint64_t>(static_cast<std::uint32_t>(cross_low)) +
                       static_cast<std::uint64_t>(static_cast<std::uint32_t>(cross_high));
    return left_high * right_high + (cross_low >> 32U) + (cross_high >> 32U) +
           (carry >> 32U);
}

[[nodiscard]] std::uint64_t independent_smulh_oracle(std::uint64_t left,
                                                     std::uint64_t right) noexcept
{
    auto high = independent_umulh_oracle(left, right);
    constexpr auto sign_bit = std::uint64_t{1} << 63U;
    if ((left & sign_bit) != 0U) high -= right;
    if ((right & sign_bit) != 0U) high -= left;
    return high;
}

[[nodiscard]] std::string hex_opcode(std::uint32_t opcode)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::setw(8) << std::setfill('0') << opcode;
    return output.str();
}

[[nodiscard]] json optional_address_json(
    const std::optional<memory::GuestAddress>& address)
{
    return address ? json(hex_address(address.value())) : json(nullptr);
}

[[nodiscard]] json transition_evidence_json(const FunctionTransitionEvidence& evidence,
                                             bool consumed)
{
    return json{{"sequence", evidence.sequence},
                {"resource_sequence", evidence.resource_sequence},
                {"charged", consumed},
                {"resource_charged", evidence.resource_charged},
                {"category", function_transition_category_name(evidence.category)},
                {"source_module", evidence.source_module},
                {"source_function", optional_address_json(evidence.source_function)},
                {"source_canonical_function", optional_address_json(evidence.source_canonical_function)},
                {"source_guest_pc", optional_address_json(evidence.source_guest_pc)},
                {"source_opcode", evidence.source_opcode ? json(evidence.source_opcode.value())
                                                          : json(nullptr)},
                {"source_instruction_id", evidence.source_instruction_id},
                {"source_instruction", evidence.source_instruction},
                {"target_module", evidence.target_module},
                {"target_function", hex_address(evidence.target_function)},
                {"target_canonical_function", optional_address_json(evidence.target_canonical_function)},
                {"target_register", evidence.target_register},
                {"target_provenance", evidence.target_provenance},
                {"call_depth_before", evidence.call_depth_before},
                {"call_depth_after", evidence.call_depth_after},
                {"boundary", runtime::execution_boundary_kind_name(evidence.boundary)},
                {"link_register_action",
                 function_transition_link_register_action_name(evidence.link_register_action)},
                {"expected_return_pc", optional_address_json(evidence.expected_return_pc)},
                {"call_frame_pushed", evidence.call_frame_pushed},
                {"target_ownership",
                 function_transition_target_ownership_name(evidence.target_ownership)},
                {"source_pc_owned_by_source_function",
                 evidence.source_pc_owned_by_source_function},
                {"canonical_boundary_valid", evidence.canonical_boundary_valid},
                {"target_equals_current_function", evidence.target_equals_current_function},
                {"target_previously_entered", evidence.target_previously_entered},
                {"previous_target_count", evidence.previous_target_count},
                {"source_target_edge_previously_seen", evidence.source_target_edge_previously_seen},
                {"previous_source_target_edge_count", evidence.previous_source_target_edge_count},
                {"guest_progress", json{{"guest_instructions", evidence.guest_instruction_count},
                                         {"guest_blocks", evidence.guest_block_count},
                                         {"ir_operations", evidence.ir_operation_count}}}};
}

[[nodiscard]] json transition_accounting_json(const FunctionTransitionAccounting& accounting)
{
    json history = json::array();
    for (const auto& evidence : accounting.history)
        history.push_back(transition_evidence_json(evidence, evidence.resource_charged));
    json terminal_history = json::array();
    const auto terminal_begin = accounting.history.size() > 64U
                                    ? accounting.history.size() - 64U
                                    : 0U;
    for (std::size_t index = terminal_begin; index < accounting.history.size(); ++index)
        terminal_history.push_back(transition_evidence_json(
            accounting.history[index], accounting.history[index].resource_charged));
    json modules = json::array();
    for (const auto& item : accounting.module_matrix)
    {
        modules.push_back(json{{"source_module", item.source_module},
                               {"target_module", item.target_module}, {"count", item.count}});
    }
    json depths = json::array();
    for (const auto& item : accounting.depth_counts)
    {
        depths.push_back(json{{"call_depth", item.call_depth}, {"count", item.count}});
    }
    const auto category_sum = accounting.initial_entries + accounting.direct_call_entries +
                              accounting.indirect_call_entries +
                              accounting.function_transfer_entries +
                              accounting.function_resume_entries + accounting.other_entries;
    return json{{"model", accounting.model},
                {"resource_definition",
                 "one unit per successful non-call function transfer; calls are bounded by "
                 "call depth and guest blocks"},
                {"configured_limit", accounting.configured_limit},
                {"total_charged", accounting.total_charged},
                {"total_function_entries", accounting.total_function_entries},
                {"entry_category_sum", category_sum},
                {"history_limit", accounting.history_limit},
                {"history_truncated", accounting.history_truncated},
                {"charged_category_sum", accounting.function_transfer_entries},
                {"reconciles", category_sum == accounting.total_function_entries &&
                                    accounting.total_charged ==
                                        accounting.function_transfer_entries},
                {"counts", json{{"initial_entries", accounting.initial_entries},
                                 {"direct_call_entries", accounting.direct_call_entries},
                                 {"indirect_call_entries", accounting.indirect_call_entries},
                                 {"function_transfer_entries", accounting.function_transfer_entries},
                                 {"function_resume_entries", accounting.function_resume_entries},
                                 {"other_entries", accounting.other_entries}}},
                {"unique_function_targets", accounting.unique_function_targets},
                {"unique_source_target_edges", accounting.unique_source_target_edges},
                {"unique_call_sites", accounting.unique_call_sites},
                {"repeated_target_count", accounting.repeated_target_count},
                {"repeated_edge_count", accounting.repeated_edge_count},
                {"maximum_target_repetition", accounting.maximum_target_repetition},
                {"maximum_edge_repetition", accounting.maximum_edge_repetition},
                {"maximum_consecutive_target_repetition",
                 accounting.maximum_consecutive_target_repetition},
                {"maximum_consecutive_edge_repetition",
                 accounting.maximum_consecutive_edge_repetition},
                {"same_function_transitions", accounting.same_function_transitions},
                {"module_matrix", std::move(modules)},
                {"transition_count_by_call_depth", std::move(depths)},
                {"terminal_attempt", accounting.terminal_attempt
                                         ? transition_evidence_json(
                                               accounting.terminal_attempt.value(), false)
                                         : json(nullptr)},
                {"terminal_history", std::move(terminal_history)},
                {"history", std::move(history)}};
}

[[nodiscard]] std::string architectural_register_name(const aarch64::Register& reg)
{
    auto architectural = reg;
    if (architectural.kind == aarch64::RegisterKind::General &&
        architectural.width == aarch64::RegisterWidth::W32)
    {
        architectural.width = aarch64::RegisterWidth::X64;
    }
    return aarch64::register_name(architectural);
}

[[nodiscard]] std::uint64_t architectural_register_value(
    const runtime::CpuState& cpu, const aarch64::Register& reg) noexcept
{
    if (reg.is_zero) return 0U;
    if (reg.is_stack_pointer) return cpu.sp;
    return reg.index < cpu.x.size() ? cpu.x[reg.index] : 0U;
}

[[nodiscard]] json range_json(const analysis::GuestAddressRange& range)
{
    return json{{"base", hex_address(range.base)}, {"size", range.size}};
}

[[nodiscard]] json analysis_report_json(
    const std::vector<analysis::AnalysisAccounting>& accounting)
{
    json modules = json::array();
    json totals{{"initial_seeds", 0U},
                {"normalized_unique_seeds", 0U},
                {"duplicate_coalesced_seeds", 0U},
                {"excluded_candidate_seeds", 0U},
                {"discovered_canonical_functions", 0U},
                {"candidate_function_entries", 0U},
                {"trusted_function_entries", 0U},
                {"cfg_analyzed", 0U},
                {"newly_analyzed_functions", 0U},
                {"reanalyzed_functions", 0U},
                {"reused_functions", 0U},
                {"invalidated_records", 0U},
                {"functions_with_cfg", 0U},
                {"direct_call_discoveries", 0U},
                {"new_seeds_generated", 0U},
                {"blocks", 0U},
                {"instructions", 0U},
                {"edges", 0U},
                {"bytes_analyzed", 0U},
                {"boundary_finalization_passes", 0U},
                {"refinement_transactions", 0U},
                {"failed_functions", 0U},
                {"function_boundary_conflicts", 0U},
                {"work_remaining_at_exhaustion", 0U}};
    const auto add_total = [&totals](const char* key, std::size_t value) {
        totals[key] = totals[key].get<std::size_t>() + value;
    };
    std::optional<std::string> strategy;
    bool same_strategy = true;
    for (const auto& item : accounting)
    {
        modules.push_back(json::parse(analysis::render_analysis_accounting_json(item)));
        const auto current_strategy = std::string(analysis::analysis_strategy_name(item.strategy));
        if (!strategy)
            strategy = current_strategy;
        else if (strategy.value() != current_strategy)
            same_strategy = false;
        add_total("initial_seeds", item.initial_seed_count);
        add_total("normalized_unique_seeds", item.normalized_unique_seed_count);
        add_total("duplicate_coalesced_seeds", item.duplicate_coalesced_seed_count);
        add_total("excluded_candidate_seeds", item.excluded_candidate_seed_count);
        add_total("discovered_canonical_functions", item.discovered_canonical_functions);
        add_total("candidate_function_entries", item.candidate_function_entries);
        add_total("trusted_function_entries", item.trusted_function_entries);
        add_total("cfg_analyzed", item.functions_cfg_analyzed);
        add_total("newly_analyzed_functions", item.newly_analyzed_functions);
        add_total("reanalyzed_functions", item.reanalyzed_functions);
        add_total("reused_functions", item.reused_functions);
        add_total("invalidated_records", item.invalidated_records);
        add_total("functions_with_cfg", item.canonical_functions_with_cfg);
        add_total("direct_call_discoveries", item.direct_call_discoveries);
        add_total("new_seeds_generated", item.new_seeds_generated);
        add_total("blocks", item.blocks_consumed);
        add_total("instructions", item.instructions_consumed);
        add_total("edges", item.edges_consumed);
        add_total("bytes_analyzed", item.bytes_analyzed);
        add_total("boundary_finalization_passes", item.boundary_finalization_passes);
        add_total("refinement_transactions", item.refinement_transactions);
        add_total("failed_functions", item.failed_functions);
        add_total("function_boundary_conflicts", item.function_boundary_conflicts);
        add_total("work_remaining_at_exhaustion", item.work_remaining_at_exhaustion);
    }
    return json{{"strategy", same_strategy && strategy ? json(strategy.value()) : json("mixed")},
                {"module_count", accounting.size()},
                {"modules", std::move(modules)},
                {"totals", std::move(totals)}};
}

[[nodiscard]] json ranges_json(const std::vector<analysis::GuestAddressRange>& ranges)
{
    json result = json::array();
    for (const auto& range : ranges) result.push_back(range_json(range));
    return result;
}

// This report-time calculation intentionally does not call the production
// ownership helpers. It independently walks the finalized, normalized exact
// instruction ranges so the recorded reconciliation cannot validate itself.
[[nodiscard]] std::vector<analysis::GuestAddressRange> independent_range_intersection(
    const std::vector<analysis::GuestAddressRange>& left,
    const std::vector<analysis::GuestAddressRange>& right)
{
    std::vector<analysis::GuestAddressRange> result;
    std::size_t left_index = 0U;
    std::size_t right_index = 0U;
    while (left_index < left.size() && right_index < right.size())
    {
        const auto& left_range = left[left_index];
        const auto& right_range = right[right_index];
        const auto left_end = checked_add_u64(left_range.base, left_range.size);
        const auto right_end = checked_add_u64(right_range.base, right_range.size);
        if (!left_end || !right_end) return {};
        const auto begin = std::max(left_range.base, right_range.base);
        const auto end = std::min(left_end.value(), right_end.value());
        if (begin < end) result.push_back({begin, end - begin});
        if (left_end.value() <= right_end.value())
            ++left_index;
        else
            ++right_index;
    }
    return result;
}

[[nodiscard]] json cfg_json(const std::optional<analysis::ControlFlowGraph>& cfg)
{
    if (!cfg) return nullptr;
    json blocks = json::array();
    for (const auto& [unused, block] : cfg->blocks)
    {
        (void)unused;
        json instructions = json::array();
        for (const auto& instruction : block.instructions)
        {
            instructions.push_back(json{{"pc", hex_address(instruction.address)},
                                        {"opcode", instruction.opcode},
                                        {"id", aarch64::instruction_id_name(instruction.id)},
                                        {"instruction", instruction.disassembly}});
        }
        json successors = json::array();
        for (const auto& edge : block.successors)
        {
            successors.push_back(json{{"source", hex_address(edge.source)},
                                      {"target", hex_address(edge.target)},
                                      {"kind", analysis::edge_kind_name(edge.kind)},
                                      {"internal", edge.internal}});
        }
        json calls = json::array();
        for (const auto& call : block.calls)
        {
            calls.push_back(json{{"address", hex_address(call.address)},
                                 {"kind", analysis::call_kind_name(call.kind)},
                                 {"target", call.target ? json(hex_address(*call.target))
                                                          : json(nullptr)},
                                 {"register_target", call.register_target
                                                          ? json(aarch64::register_name(
                                                                *call.register_target))
                                                          : json(nullptr)}});
        }
        std::optional<GuestAddress> end;
        if (!block.instructions.empty())
        {
            const auto checked_end = checked_add_u64(block.instructions.back().address, 4U);
            if (checked_end) end = checked_end.value();
        }
        blocks.push_back(json{{"start", hex_address(block.start)},
                              {"end", end ? json(hex_address(end.value())) : json(nullptr)},
                              {"instruction_count", block.instructions.size()},
                              {"instructions", std::move(instructions)},
                              {"successors", std::move(successors)},
                              {"calls", std::move(calls)},
                              {"termination", block.termination}});
    }
    json calls = json::array();
    for (const auto& call : cfg->calls)
    {
        calls.push_back(json{{"address", hex_address(call.address)},
                             {"kind", analysis::call_kind_name(call.kind)},
                             {"target", call.target ? json(hex_address(*call.target))
                                                      : json(nullptr)},
                             {"register_target", call.register_target
                                                      ? json(aarch64::register_name(
                                                            *call.register_target))
                                                      : json(nullptr)}});
    }
    json unresolved = json::array();
    for (const auto& item : cfg->unresolved)
    {
        unresolved.push_back(json{{"pc", hex_address(item.address)},
                                  {"kind", aarch64::control_flow_kind_name(item.kind)},
                                  {"register_target", item.register_target
                                                           ? json(aarch64::register_name(
                                                                 *item.register_target))
                                                           : json(nullptr)},
                                  {"reason", item.reason}});
    }
    return json{{"entry", hex_address(cfg->entry)},
                {"instruction_count", cfg->instruction_count},
                {"block_count", cfg->blocks.size()},
                {"blocks", std::move(blocks)},
                {"calls", std::move(calls)},
                {"unresolved_control_flow", std::move(unresolved)}};
}

[[nodiscard]] json boundary_reconciliation_json(
    const analysis::FunctionBoundaryReconciliation& reconciliation)
{
    const auto independent = independent_range_intersection(
        reconciliation.candidate_owned_code_ranges_before,
        reconciliation.existing_owned_code_ranges_before);
    json witnesses = json::array();
    for (const auto& witness : reconciliation.witnesses)
    {
        witnesses.push_back(json{{"kind", analysis::function_boundary_witness_kind_name(
                                              witness.kind)},
                                 {"source", hex_address(witness.source)},
                                 {"target", hex_address(witness.target)}});
    }
    json boundary_entries = json::array();
    for (const auto entry : reconciliation.boundary_entries)
        boundary_entries.push_back(hex_address(entry));
    return json{{"kind", analysis::function_boundary_reconciliation_kind_name(
                          reconciliation.kind)},
                {"existing_canonical_entry", reconciliation.existing_canonical_entry
                                                    ? json(hex_address(*reconciliation.existing_canonical_entry))
                                                    : json(nullptr)},
                {"existing_owned_code_ranges_before",
                 ranges_json(reconciliation.existing_owned_code_ranges_before)},
                {"candidate_owned_code_ranges_before",
                 ranges_json(reconciliation.candidate_owned_code_ranges_before)},
                {"precise_overlap_ranges", ranges_json(reconciliation.precise_overlap_ranges)},
                {"existing_owned_code_ranges_after",
                 ranges_json(reconciliation.existing_owned_code_ranges_after)},
                {"candidate_owned_code_ranges_after",
                 ranges_json(reconciliation.candidate_owned_code_ranges_after)},
                {"boundary_owned_code_ranges", ranges_json(reconciliation.boundary_owned_code_ranges)},
                {"boundary_entries", std::move(boundary_entries)},
                {"witnesses", std::move(witnesses)},
                {"existing_cfg_before", cfg_json(reconciliation.existing_cfg_before)},
                {"candidate_cfg_before", cfg_json(reconciliation.candidate_cfg_before)},
                {"boundary_cfg", cfg_json(reconciliation.boundary_cfg)},
                {"independent_overlap_check",
                 json{{"production_overlap_ranges", ranges_json(reconciliation.precise_overlap_ranges)},
                      {"independently_computed_ranges", ranges_json(independent)},
                      {"agree", independent == reconciliation.precise_overlap_ranges}}},
                {"reason", reconciliation.reason}};
}

[[nodiscard]] json indirect_target_assessment_json(
    const analysis::IndirectTargetAssessment& assessment)
{
    const auto& observed = assessment.observed;
    const auto& validation = assessment.validation;
    const auto& decision = assessment.decision;
    json static_evidence = json::array();
    for (const auto& item : assessment.static_evidence)
    {
        static_evidence.push_back(json{
            {"source", analysis::function_discovery_source_name(item.source)},
            {"confidence", analysis::function_confidence_name(item.confidence)},
            {"kind", analysis::function_entry_evidence_kind_name(item.kind)},
            {"strength", analysis::function_entry_evidence_strength_name(item.strength)},
            {"detail", item.detail}});
    }
    json entry_evidence = json::array();
    for (const auto& item : assessment.entry_evidence)
    {
        entry_evidence.push_back(json{
            {"kind", analysis::function_entry_evidence_kind_name(item.kind)},
            {"strength", analysis::function_entry_evidence_strength_name(item.strength)},
            {"source_module", item.source_module},
            {"source_address", item.source_address ? json(hex_address(*item.source_address)) : json(nullptr)},
            {"target_module", item.target_module},
            {"target", hex_address(item.target)},
            {"relocation_index", item.relocation_index ? json(*item.relocation_index) : json(nullptr)},
            {"relocation_type", item.relocation_type
                                     ? json(format::aarch64_relocation_type_name(*item.relocation_type))
                                     : json(nullptr)},
            {"relocation_source", item.relocation_source
                                       ? json(format::relocation_source_name(*item.relocation_source))
                                       : json(nullptr)},
            {"symbol_index", item.symbol_index ? json(*item.symbol_index) : json(nullptr)},
            {"symbol_name", item.symbol_name},
            {"target_declared_function", item.target_declared_function},
            {"slot_value_verified", item.slot_value_verified},
            {"detail", item.detail}});
    }
    json rejected_evidence = json::array();
    for (const auto& item : assessment.rejected_evidence)
    {
        rejected_evidence.push_back(json{
            {"reason", analysis::function_entry_evidence_rejection_name(item.kind)},
            {"evidence_kind", analysis::function_entry_evidence_kind_name(item.evidence_kind)},
            {"source_module", item.source_module},
            {"source_address", item.source_address ? json(hex_address(*item.source_address)) : json(nullptr)},
            {"detail", item.detail}});
    }
    json relocation_provenance = json::array();
    for (const auto& item : validation.relocation_provenance)
    {
        relocation_provenance.push_back(json{
            {"source_module", item.source_module},
            {"source_slot", hex_address(item.source_slot)},
            {"relocation_index", item.relocation_index},
            {"relocation_type", format::aarch64_relocation_type_name(item.relocation.type)},
            {"relocation_source", format::relocation_source_name(item.relocation.source)},
            {"resolved_target", item.resolved_target ? json(hex_address(*item.resolved_target)) : json(nullptr)},
            {"target_module", item.target_module},
            {"symbol_index", item.symbol_index ? json(*item.symbol_index) : json(nullptr)},
            {"symbol_name", item.symbol_name},
            {"target_declared_function", item.target_declared_function},
            {"slot_value_verified", item.slot_value_verified},
            {"resolution_error", item.resolution_error ? json(*item.resolution_error) : json(nullptr)}});
    }
    json candidate_ranges = ranges_json(validation.candidate_owned_code_ranges);
    json overlap_ranges = ranges_json(validation.overlap_ranges);
    json direct_calls = json::array();
    for (const auto target : validation.direct_call_targets)
        direct_calls.push_back(hex_address(target));
    json unresolved = json::array();
    for (const auto& item : validation.unresolved_control_flow)
    {
        unresolved.push_back(json{{"pc", hex_address(item.address)},
                                  {"kind", aarch64::control_flow_kind_name(item.kind)},
                                  {"reason", item.reason}});
    }
    json first_instruction = nullptr;
    if (validation.cfg && !validation.cfg->blocks.empty())
    {
        const auto entry_block = validation.cfg->blocks.find(observed.target);
        const auto block = entry_block == validation.cfg->blocks.end()
                               ? validation.cfg->blocks.begin()
                               : entry_block;
        if (!block->second.instructions.empty())
        {
            const auto& instruction = block->second.instructions.front();
            first_instruction = json{{"pc", hex_address(instruction.address)},
                                      {"opcode", instruction.opcode},
                                      {"id", aarch64::instruction_id_name(instruction.id)},
                                      {"instruction", instruction.disassembly}};
        }
    }
    json last_instruction = nullptr;
    if (validation.cfg)
    {
        const aarch64::DecodedInstruction* last = nullptr;
        for (const auto& [unused, block] : validation.cfg->blocks)
        {
            (void)unused;
            for (const auto& instruction : block.instructions)
            {
                if (last == nullptr || instruction.address > last->address) last = &instruction;
            }
        }
        if (last != nullptr)
        {
            last_instruction = json{{"pc", hex_address(last->address)},
                                    {"id", aarch64::instruction_id_name(last->id)},
                                    {"instruction", last->disassembly}};
        }
    }
    json analysis_error = nullptr;
    if (validation.analysis_error)
    {
        analysis_error = json{{"code", error_code_name(validation.analysis_error->code)},
                              {"message", validation.analysis_error->message}};
    }
    json observation_provenance = json::array();
    for (const auto& item : observed.observation_provenance)
    {
        observation_provenance.push_back(json{
            {"source_module", item.source_module},
            {"source_function", hex_address(item.source_function)},
            {"source_pc", hex_address(item.source_pc)},
            {"control_flow", analysis::indirect_control_flow_kind_name(item.control_flow)},
            {"target_register", item.target_register},
            {"pointer_provenance",
             analysis::indirect_target_pointer_provenance_name(item.pointer_provenance)},
            {"guest_load_address", item.guest_load_address
                                        ? json(hex_address(item.guest_load_address.value()))
                                        : json(nullptr)}});
    }
    return json{
        {"source_module", observed.source_module},
        {"source_function", hex_address(observed.source_function)},
        {"source_pc", hex_address(observed.source_pc)},
        {"control_flow", analysis::indirect_control_flow_kind_name(observed.control_flow)},
        {"target_register", observed.target_register},
        {"target", hex_address(observed.target)},
        {"target_module", validation.target_module.empty() ? observed.target_module
                                                              : validation.target_module},
        {"pointer_provenance", json{
            {"kind", analysis::indirect_target_pointer_provenance_name(
                          observed.pointer_provenance)},
            {"address", observed.guest_load_address
                             ? json(hex_address(observed.guest_load_address.value()))
                             : json(nullptr)}}},
        {"observation_count", observed.observation_count},
        {"observation_provenance", std::move(observation_provenance)},
        {"static_evidence", std::move(static_evidence)},
        {"observed_entry_evidence", [&]() {
             json items = json::array();
             for (const auto& item : observed.entry_evidence)
             {
                 items.push_back(json{{"kind", analysis::function_entry_evidence_kind_name(item.kind)},
                                      {"strength", analysis::function_entry_evidence_strength_name(item.strength)},
                                      {"detail", item.detail}});
             }
             return items;
         }()},
        {"entry_evidence", std::move(entry_evidence)},
        {"rejected_evidence", std::move(rejected_evidence)},
        {"certification", json{{"status", analysis::function_certification_status_name(
                                              assessment.certification.status)},
                                {"certified", assessment.certification.certified},
                                {"confidence", analysis::function_confidence_name(
                                                    assessment.certification.confidence)},
                                {"reason", assessment.certification.reason},
                                {"canonical_entry", decision.canonical_entry
                                                         ? json(hex_address(*decision.canonical_entry))
                                                         : json(nullptr)},
                                {"newly_promoted", decision.promoted},
                                {"guest_code_entered", assessment.guest_code_entered},
                                {"first_guest_pc", assessment.first_guest_pc
                                                         ? json(hex_address(*assessment.first_guest_pc))
                                                         : json(nullptr)},
                                {"first_guest_opcode", assessment.first_guest_opcode
                                                              ? json(*assessment.first_guest_opcode)
                                                              : json(nullptr)},
                                {"next_guest_pc", assessment.next_guest_pc
                                                       ? json(hex_address(*assessment.next_guest_pc))
                                                       : json(nullptr)}}},
        {"candidate", json{{"module", assessment.candidate.module},
                             {"entry", hex_address(assessment.candidate.entry)},
                             {"guest_code_entered", assessment.guest_code_entered}}},
        {"address_validation", json{{"nonzero", validation.nonzero},
                                     {"aligned", validation.aligned},
                                     {"mapped", validation.mapped},
                                     {"executable", validation.executable},
                                     {"unique_module_owner", validation.unique_module_owner},
                                     {"structurally_eligible", validation.structurally_eligible},
                                     {"target_module_base", validation.target_module_base
                                                                  ? json(hex_address(*validation.target_module_base))
                                                                  : json(nullptr)},
                                     {"pointer_slot", validation.pointer_slot
                                                           ? json(hex_address(*validation.pointer_slot))
                                                           : json(nullptr)},
                                     {"pointer_slot_module", validation.pointer_slot_module},
                                     {"pointer_slot_relocation_found", validation.pointer_slot_relocation_found},
                                     {"pointer_slot_value_verified", validation.pointer_slot_value_verified},
                                     {"pointer_target_declared_function", validation.pointer_target_declared_function},
                                     {"relocation_provenance", std::move(relocation_provenance)}}},
        {"ownership", json{{"status", analysis::indirect_target_ownership_name(
                                         validation.ownership)},
                            {"existing_canonical_entry", validation.existing_canonical_entry
                                                               ? json(hex_address(*validation.existing_canonical_entry))
                                                               : json(nullptr)},
                            {"candidate_owned_code_ranges", std::move(candidate_ranges)},
                            {"overlap_ranges", std::move(overlap_ranges)},
                            {"boundary_reconciliation",
                             boundary_reconciliation_json(validation.boundary_reconciliation)}}},
        {"cfg_validation", json{{"status", analysis::indirect_target_cfg_status_name(
                                              validation.cfg_status)},
                                 {"blocks", validation.blocks},
                                 {"instructions", validation.instructions},
                                 {"edges", validation.edges},
                                 {"first_instruction", std::move(first_instruction)},
                                 {"last_instruction", std::move(last_instruction)},
                                 {"direct_call_targets", std::move(direct_calls)},
                                 {"unresolved_indirect_flow", std::move(unresolved)},
                                 {"blocks_detail", cfg_json(validation.cfg)},
                                 {"analysis_error", std::move(analysis_error)}}},
        {"decision", json{{"kind", analysis::indirect_target_decision_name(decision.kind)},
                           {"confidence", analysis::function_confidence_name(decision.confidence)},
                           {"eligible_for_promotion", decision.eligible_for_promotion},
                           {"promoted", decision.promoted},
                           {"canonical_entry", decision.canonical_entry
                                                   ? json(hex_address(*decision.canonical_entry))
                                                   : json(nullptr)},
                           {"reason", decision.reason}}},
        {"map_generation", json{{"before", assessment.map_generation_before},
                                 {"after", assessment.map_generation_after}}}};
}

[[nodiscard]] json refinement_candidate_identity_json(
    const std::optional<analysis::IndirectTargetCandidateIdentity>& identity)
{
    if (!identity) return nullptr;
    return json{{"target_module", identity->target_module},
                {"target", hex_address(identity->target)},
                {"source_module", identity->source_module},
                {"source_function", hex_address(identity->source_function)},
                {"source_pc", hex_address(identity->source_pc)},
                {"control_flow", analysis::indirect_control_flow_kind_name(identity->control_flow)},
                {"target_register", identity->target_register},
                {"pointer_provenance",
                 analysis::indirect_target_pointer_provenance_name(identity->pointer_provenance)},
                {"guest_load_address", identity->guest_load_address
                                            ? json(hex_address(identity->guest_load_address.value()))
                                            : json(nullptr)}};
}

[[nodiscard]] json indirect_target_refinement_json(
    const analysis::IndirectTargetRefinementSummary& summary)
{
    const auto& budgets = summary.configured;
    const auto& exhaustion = summary.exhaustion;
    const auto provenance_json = [](const analysis::AnalysisBudgetProvenance& provenance) {
        return json{{"kind", analysis::analysis_budget_provenance_kind_name(provenance.kind)},
                    {"detail", provenance.detail}};
    };
    const auto& analysis_budgets = budgets.analysis;
    const auto& analysis = summary.analysis;
    const auto compatibility_exhaustion =
        exhaustion.dimension ==
                analysis::IndirectTargetRefinementBudgetDimension::RefinementAnalysisTransactions
            ? json{{"consumed", exhaustion.consumed},
                   {"limit", exhaustion.limit},
                   {"module", exhaustion.module},
                   {"generation", exhaustion.generation},
                   {"next_work", refinement_candidate_identity_json(exhaustion.next_work)}}
            : json(nullptr);
    const auto counter_overflow_failure =
        exhaustion.dimension == analysis::IndirectTargetRefinementBudgetDimension::CounterOverflow
            ? json{{"consumed", exhaustion.consumed},
                   {"limit", exhaustion.limit},
                   {"module", exhaustion.module},
                   {"generation", exhaustion.generation},
                   {"next_work", refinement_candidate_identity_json(exhaustion.next_work)}}
            : json(nullptr);
    return json{
        {"configured_limits",
         json{{"max_stagnant_rounds", budgets.max_stagnant_rounds},
              {"legacy_max_unique_candidates", budgets.max_unique_candidates
                                                       ? json(budgets.max_unique_candidates.value())
                                                       : json(nullptr)},
              {"candidate_limit_provenance", provenance_json(budgets.candidate_limit_provenance)},
              {"max_candidate_assessments", budgets.max_candidate_assessments
                                                 ? json(budgets.max_candidate_assessments.value())
                                                 : json(nullptr)},
              {"candidate_assessment_limit_provenance",
               provenance_json(budgets.candidate_assessment_limit_provenance)},
              {"max_promotions", budgets.max_promotions},
              {"max_map_rebuilds", budgets.max_map_rebuilds},
              {"legacy_event_limits", budgets.legacy_event_limits}}},
        {"candidate_scaling",
         json{{"structural_universe_kind", "executable_guest_instruction_slots"},
              {"structural_universe_limit",
               budgets.candidate_universe.executable_instruction_slots},
              {"structural_universe_provenance",
               provenance_json(budgets.candidate_universe.provenance)},
              {"effective_unique_candidate_limit",
               budgets.max_unique_candidates
                   ? std::min(budgets.max_unique_candidates.value(),
                              budgets.candidate_universe.executable_instruction_slots)
                   : budgets.candidate_universe.executable_instruction_slots},
              {"sparse_records", true}}},
        {"aggregate_analysis_limits",
         json{{"max_functions_analyzed", analysis_budgets.max_functions_analyzed},
              {"max_functions_reanalyzed", analysis_budgets.max_functions_reanalyzed},
              {"max_instructions", analysis_budgets.max_instructions},
              {"max_blocks", analysis_budgets.max_blocks},
              {"max_edges", analysis_budgets.max_edges},
              {"max_bytes_analyzed", analysis_budgets.max_bytes_analyzed},
              {"max_boundary_finalization_passes",
               analysis_budgets.max_boundary_finalization_passes},
              {"max_invalidated_records", analysis_budgets.max_invalidated_records},
              {"max_transactions", analysis_budgets.max_transactions
                                       ? json(analysis_budgets.max_transactions.value())
                                       : json(nullptr)}}},
        {"aggregate_analysis_provenance",
         json{{"functions_analyzed", provenance_json(analysis_budgets.functions_analyzed_provenance)},
              {"functions_reanalyzed", provenance_json(analysis_budgets.functions_reanalyzed_provenance)},
              {"instructions", provenance_json(analysis_budgets.instructions_provenance)},
              {"blocks", provenance_json(analysis_budgets.blocks_provenance)},
              {"edges", provenance_json(analysis_budgets.edges_provenance)},
              {"bytes_analyzed", provenance_json(analysis_budgets.bytes_provenance)},
              {"boundary_finalization_passes",
               provenance_json(analysis_budgets.boundary_finalization_provenance)},
              {"invalidated_records", provenance_json(analysis_budgets.invalidated_records_provenance)},
              {"transactions", provenance_json(analysis_budgets.transactions_provenance)}}},
        {"transaction_resource",
         json{{"ordinary_termination_mode", "semantic_aggregate_resources"},
              {"transaction_ceiling_configured", analysis_budgets.max_transactions.has_value()},
              {"configured_transaction_limit", analysis_budgets.max_transactions
                                                    ? json(analysis_budgets.max_transactions.value())
                                                    : json(nullptr)},
              {"transaction_limit_provenance",
               provenance_json(analysis_budgets.transactions_provenance)},
              {"transactions_consumed", analysis.transactions},
              {"dominating_resource", "boundary_finalization_passes"},
              {"termination_invariant", "transactions <= boundary_finalization_passes"},
              {"explicit_compatibility_exhaustion", std::move(compatibility_exhaustion)},
              {"counter_overflow_failure", std::move(counter_overflow_failure)}}},
        {"aggregate_analysis_consumption",
         json{{"functions_analyzed", analysis.functions_analyzed},
              {"functions_reanalyzed", analysis.functions_reanalyzed},
              {"functions_reused", analysis.functions_reused},
              {"instructions", analysis.instructions},
              {"blocks", analysis.blocks},
              {"edges", analysis.edges},
              {"bytes_analyzed", analysis.bytes_analyzed},
              {"boundary_finalization_passes", analysis.boundary_finalization_passes},
              {"invalidated_records", analysis.invalidated_records},
              {"transactions", analysis.transactions}}},
        {"assessment_accounting",
         json{{"model", "structural_candidate_generation_v1"},
              {"first_time_assessments", summary.first_candidate_assessments},
              {"generation_dependent_reassessments", summary.generation_reassessments},
              {"total_assessments", summary.candidate_assessments},
              {"same_generation_duplicate_attempts",
               summary.same_generation_assessment_attempts},
              {"legacy_assessment_limit_configured",
               budgets.max_candidate_assessments.has_value()},
              {"legacy_assessment_limit", budgets.max_candidate_assessments
                                               ? json(budgets.max_candidate_assessments.value())
                                               : json(nullptr)},
              {"legacy_limit_provenance",
               provenance_json(budgets.candidate_assessment_limit_provenance)},
              {"ordinary_structural_bound",
               budgets.candidate_universe.executable_instruction_slots},
              {"termination_model",
               "finite candidate records; at most one assessment per candidate per relevant "
               "immutable map generation; map generations are finite published refinements"},
              {"map_generation", summary.map_generation},
              {"last_assessed_candidate",
               refinement_candidate_identity_json(summary.last_assessed_candidate)},
              {"last_assessment_generation",
               summary.last_assessment_generation
                   ? json(summary.last_assessment_generation.value())
                   : json(nullptr)},
              {"next_pending_candidate",
               refinement_candidate_identity_json(summary.next_pending_candidate)},
              {"typed_exhaustion_reason",
               analysis::indirect_target_refinement_budget_dimension_name(
                   exhaustion.dimension)}}},
        {"total_execution_attempts", summary.total_execution_attempts},
        {"productive_rounds", summary.productive_rounds},
        {"stagnant_rounds", summary.stagnant_rounds},
        {"observations_received", summary.observations_received},
        {"unique_observations", summary.unique_observations},
        {"unique_candidates", summary.unique_candidates},
        {"candidate_records", summary.candidate_records},
        {"structurally_ineligible_candidates", summary.structurally_ineligible_candidates},
        {"candidate_assessments", summary.candidate_assessments},
        {"first_candidate_assessments", summary.first_candidate_assessments},
        {"generation_reassessments", summary.generation_reassessments},
        {"same_generation_assessment_attempts", summary.same_generation_assessment_attempts},
        {"failed_refinements", summary.failed_refinements},
        {"rollback_assessments", summary.rollback_assessments},
        {"terminal_resolutions", summary.terminal_resolutions},
        {"successful_promotions", summary.successful_promotions},
        {"existing_trusted_hits", summary.existing_trusted_hits},
        {"duplicate_coalesced_observations", summary.duplicate_coalesced_observations},
        {"rejected_candidates", summary.rejected_candidates},
        {"boundary_reconciliations", summary.boundary_reconciliations},
        {"map_rebuilds", summary.map_rebuilds},
        {"module_maps_rebuilt", summary.module_maps_rebuilt},
        {"module_maps_reused", summary.module_maps_reused},
        {"batching",
         json{{"batch_count", summary.refinement_batches},
              {"candidates", summary.batch_candidates},
              {"rebuilds_avoided", summary.rebuilds_avoided},
              {"singleton_batches", summary.singleton_batches},
              {"average_batch_width", summary.refinement_batches == 0U
                                            ? 0.0
                                            : static_cast<double>(summary.batch_candidates) /
                                                  static_cast<double>(summary.refinement_batches)},
              {"max_batch_width", summary.max_batch_width}}},
        {"incremental_reuse",
         json{{"finalized_functions_reused", summary.finalized_functions_reused},
              {"cfgs_reused", summary.cfgs_reused},
              {"functions_rebuilt", summary.functions_rebuilt},
              {"modules_touched", summary.modules_touched},
              {"incremental_updates", summary.incremental_updates},
              {"full_rebuilds", summary.full_rebuilds}}},
        {"candidates_reconsidered_after_map_change",
         summary.candidates_reconsidered_after_map_change},
        {"map_generation", summary.map_generation},
        {"exhausted_dimension",
         analysis::indirect_target_refinement_budget_dimension_name(exhaustion.dimension)},
        {"exhausted_consumed", exhaustion.consumed},
        {"exhausted_limit", exhaustion.limit},
        {"exhausted_module", exhaustion.module},
        {"exhausted_generation", exhaustion.generation},
        {"exhausted_next_work", refinement_candidate_identity_json(exhaustion.next_work)},
        {"pending_candidate_count", summary.pending_candidate_count},
        {"last_processed_candidate",
         refinement_candidate_identity_json(summary.last_processed_candidate)},
        {"last_assessed_candidate",
         refinement_candidate_identity_json(summary.last_assessed_candidate)},
        {"next_pending_candidate",
         refinement_candidate_identity_json(summary.next_pending_candidate)}};
}

[[nodiscard]] json cpu_json(const runtime::CpuState& cpu)
{
    json registers = json::array();
    for (const auto value : cpu.x)
    {
        registers.push_back(hex_address(value));
    }
    return json{{"pc", hex_address(cpu.pc)}, {"sp", hex_address(cpu.sp)},
                {"x30", hex_address(cpu.x[30U])}, {"x", std::move(registers)}};
}

[[nodiscard]] json abi_snapshot_json(const runtime::GuestAbiSnapshot& snapshot)
{
    json arguments = json::array();
    for (const auto value : snapshot.x0_x7)
    {
        arguments.push_back(hex_address(value));
    }
    return json{{"x0_x7", std::move(arguments)}, {"sp", hex_address(snapshot.sp)},
                {"x29", hex_address(snapshot.x29)}, {"x30", hex_address(snapshot.x30)},
                {"pc", hex_address(snapshot.pc)}};
}

[[nodiscard]] json dynamic_pointer_json(const std::optional<format::DynamicPointer>& pointer)
{
    if (!pointer)
    {
        return nullptr;
    }
    return json{{"module_offset", hex_address(pointer->module_offset)},
                {"address", hex_address(pointer->address)}};
}

[[nodiscard]] json mod0_range_json(const std::optional<format::Mod0Range>& range)
{
    if (!range)
    {
        return nullptr;
    }
    return json{{"start_offset", range->start_offset}, {"end_offset", range->end_offset},
                {"start_address", hex_address(range->start_address)},
                {"end_address", hex_address(range->end_address)}};
}

[[nodiscard]] json module_metadata_json(
    const std::optional<format::ModuleMetadata>& metadata)
{
    if (!metadata)
    {
        return json{{"present", false}};
    }

    json value{{"present", true}, {"module_base", hex_address(metadata->module_base)}};
    if (metadata->mod0)
    {
        const auto& mod0 = metadata->mod0.value();
        value["mod0"] = json{
            {"address", hex_address(mod0.address)},
            {"dynamic_offset", mod0.dynamic_offset},
            {"bss_start_offset", mod0.bss_start_offset},
            {"bss_end_offset", mod0.bss_end_offset},
            {"exception_info_start_offset", mod0.exception_info_start_offset},
            {"exception_info_end_offset", mod0.exception_info_end_offset},
            {"module_object_offset", mod0.module_object_offset},
            {"dynamic_address", hex_address(mod0.dynamic_address)},
            {"bss_start_address", hex_address(mod0.bss_start_address)},
            {"bss_end_address", hex_address(mod0.bss_end_address)},
            {"exception_info_start_address", hex_address(mod0.exception_info_start_address)},
            {"exception_info_end_address", hex_address(mod0.exception_info_end_address)},
            {"module_object_address", hex_address(mod0.module_object_address)},
            {"relro", mod0_range_json(mod0.relro)},
            {"nx_debug_link", mod0_range_json(mod0.nx_debug_link)},
            {"gnu_build_id_note", mod0_range_json(mod0.gnu_build_id_note)}};
    }
    else
    {
        value["mod0"] = nullptr;
    }

    if (!metadata->dynamic)
    {
        value["dynamic"] = nullptr;
        return value;
    }

    const auto& dynamic = metadata->dynamic.value();
    json entries = json::array();
    for (const auto& entry : dynamic.entries)
    {
        entries.push_back(json{{"tag", entry.tag},
                               {"tag_name", format::dynamic_tag_name(
                                                   static_cast<format::DynamicTag>(entry.tag))},
                               {"value", hex_address(entry.value)}});
    }
    value["dynamic"] = json{
        {"module_base", hex_address(dynamic.module_base)},
        {"address", hex_address(dynamic.address)},
        {"entry_count", dynamic.entry_count},
        {"entries", std::move(entries)},
        {"pltgot", dynamic_pointer_json(dynamic.pltgot)},
        {"hash", dynamic_pointer_json(dynamic.hash)},
        {"gnu_hash", dynamic_pointer_json(dynamic.gnu_hash)},
        {"strtab", dynamic_pointer_json(dynamic.strtab)},
        {"symtab", dynamic_pointer_json(dynamic.symtab)},
        {"rela", dynamic_pointer_json(dynamic.rela)},
        {"init", dynamic_pointer_json(dynamic.init)},
        {"fini", dynamic_pointer_json(dynamic.fini)},
        {"rel", dynamic_pointer_json(dynamic.rel)},
        {"debug", dynamic_pointer_json(dynamic.debug)},
        {"jmprel", dynamic_pointer_json(dynamic.jmprel)},
        {"init_array", dynamic_pointer_json(dynamic.init_array)},
        {"fini_array", dynamic_pointer_json(dynamic.fini_array)},
        {"preinit_array", dynamic_pointer_json(dynamic.preinit_array)},
        {"pltrelsz", dynamic.pltrelsz ? json(dynamic.pltrelsz.value()) : json(nullptr)},
        {"strsz", dynamic.strsz ? json(dynamic.strsz.value()) : json(nullptr)},
        {"syment", dynamic.syment ? json(dynamic.syment.value()) : json(nullptr)},
        {"relasz", dynamic.relasz ? json(dynamic.relasz.value()) : json(nullptr)},
        {"relaent", dynamic.relaent ? json(dynamic.relaent.value()) : json(nullptr)},
        {"relsz", dynamic.relsz ? json(dynamic.relsz.value()) : json(nullptr)},
        {"relent", dynamic.relent ? json(dynamic.relent.value()) : json(nullptr)},
        {"plt_rel_type", dynamic.plt_rel_type ? json(dynamic.plt_rel_type.value()) : json(nullptr)},
        {"init_array_size", dynamic.init_array_size ? json(dynamic.init_array_size.value()) : json(nullptr)},
        {"fini_array_size", dynamic.fini_array_size ? json(dynamic.fini_array_size.value()) : json(nullptr)},
        {"preinit_array_size", dynamic.preinit_array_size ? json(dynamic.preinit_array_size.value()) : json(nullptr)},
        {"relacount", dynamic.relacount ? json(dynamic.relacount.value()) : json(nullptr)},
        {"relcount", dynamic.relcount ? json(dynamic.relcount.value()) : json(nullptr)},
        {"rela_count", dynamic.rela_count ? json(dynamic.rela_count.value()) : json(nullptr)},
        {"jmprel_count", dynamic.jmprel_count ? json(dynamic.jmprel_count.value()) : json(nullptr)},
        {"rel_count", dynamic.rel_count ? json(dynamic.rel_count.value()) : json(nullptr)}};
    return value;
}

[[nodiscard]] json runtime_import_json(const RuntimeImportObservation& observation)
{
    const auto& descriptor = observation.descriptor;
    json abi_arguments = json::array();
    for (const auto kind : descriptor.signature.arguments)
    {
        abi_arguments.push_back(runtime::abi_value_kind_name(kind));
    }
    if (descriptor.signature.arguments.empty() && descriptor.signature.argument_count)
    {
        abi_arguments = json::array();
    }

    json observed_arguments = json::array();
    for (const auto& argument : observation.arguments)
    {
        observed_arguments.push_back(json{
            {"index", argument.index},
            {"location", argument.location},
            {"readable", argument.readable},
            {"value", hex_address(argument.value)},
            {"diagnostic", argument.diagnostic},
            {"mapping", argument.mapping},
            {"permissions", argument.permissions},
            {"region_kind", argument.region_kind},
            {"aligned_8", argument.aligned_8},
            {"aligned_16", argument.aligned_16},
            {"dynamic_tags", argument.dynamic_tags},
            {"dynamic_symbols", argument.dynamic_symbols},
            {"relocation_ranges", argument.relocation_ranges},
            {"range_validity", argument.range_validity}});
    }

    json trampoline_evidence = observation.trampoline_evidence;
    const auto& import = observation.provenance;
    return json{
        {"symbol", descriptor.symbol_name},
        {"support", runtime::runtime_support_status_name(descriptor.support)},
        {"subsystem", runtime::runtime_subsystem_name(descriptor.subsystem)},
        {"signature", json{
                          {"argument_count", descriptor.signature.argument_count
                                                   ? json(descriptor.signature.argument_count.value())
                                                   : json(nullptr)},
                          {"observed_argument_count", descriptor.signature.observed_argument_count},
                          {"arguments", std::move(abi_arguments)},
                          {"return", runtime::abi_return_kind_name(
                                         descriptor.signature.return_kind)}}},
        {"evidence", json{{"sources", descriptor.evidence.sources},
                           {"confidence", descriptor.evidence.confidence},
                           {"rationale", descriptor.evidence.rationale}}},
        {"invocation", runtime::external_invocation_kind_name(observation.invocation)},
        {"source_guest_pc", hex_address(observation.source_guest_pc)},
        {"dynamic_pltgot", observation.dynamic_pltgot
                                ? json(hex_address(observation.dynamic_pltgot.value()))
                                : json(nullptr)},
        {"dynamic_pltgot_slot_delta", observation.dynamic_pltgot_slot_delta
                                          ? json(hex_address(observation.dynamic_pltgot_slot_delta.value()))
                                          : json(nullptr)},
        {"guest_provider_resolution", observation.guest_provider_resolution},
        {"runtime_fallback_eligible", observation.runtime_fallback_eligible},
        {"provenance", json{
                           {"consumer_module", import.consumer_module},
                           {"relocation_target", hex_address(import.relocation_target)},
                           {"relocation_index", import.relocation_index},
                           {"relocation_offset", hex_address(import.relocation.offset)},
                           {"relocation_type", import.relocation.raw_type},
                           {"relocation_type_name", format::aarch64_relocation_type_name(
                                                         import.relocation.type)},
                           {"relocation_source", format::relocation_source_name(
                                                       import.relocation.source)},
                           {"relocation_addend", import.relocation.addend},
                           {"symbol_index", import.symbol.symbol_index},
                           {"symbol", import.symbol.name},
                           {"binding", format::symbol_binding_name(import.symbol.binding)},
                           {"type", format::symbol_type_name(import.symbol.type)},
                           {"visibility", format::symbol_visibility_name(import.symbol.visibility)},
                           {"section_index", import.symbol.section_index}}},
        {"abi", json{{"snapshot", abi_snapshot_json(observation.abi)},
                      {"validation", observation.abi_validation},
                      {"diagnostic", observation.abi_diagnostic},
                      {"arguments", std::move(observed_arguments)}}},
        {"trampoline", json{{"classification", observation.trampoline_classification},
                             {"evidence", std::move(trampoline_evidence)}}},
        {"outcome", json{{"status", observation.outcome},
                          {"diagnostic", observation.outcome_diagnostic}}}};
}

[[nodiscard]] bool is_memory_error(ErrorCode code) noexcept
{
    switch (code)
    {
    case ErrorCode::OutOfBounds:
    case ErrorCode::UnmappedMemory:
    case ErrorCode::PermissionDenied:
    case ErrorCode::InstructionFetchFailed:
    case ErrorCode::NonExecutableAddress:
    case ErrorCode::InvalidGuestAddress:
    case ErrorCode::MisalignedAtomicAccess:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] Result<GuestAddress> align_up(GuestAddress value, GuestAddress alignment)
{
    const auto mask = alignment - 1U;
    const auto rounded = checked_add_u64(value, mask);
    if (!rounded)
    {
        return Result<GuestAddress>::failure(rounded.error());
    }
    return Result<GuestAddress>::success(rounded.value() & ~mask);
}

[[nodiscard]] std::string permissions_text(memory::GuestMemoryPermissions permissions)
{
    std::string result;
    if (memory::has_permission(permissions, memory::GuestMemoryPermissions::Read)) result += 'r';
    else result += '-';
    if (memory::has_permission(permissions, memory::GuestMemoryPermissions::Write)) result += 'w';
    else result += '-';
    if (memory::has_permission(permissions, memory::GuestMemoryPermissions::Execute)) result += 'x';
    else result += '-';
    return result;
}

[[nodiscard]] bool range_contains(GuestAddress begin, GuestAddress end, GuestAddress value) noexcept
{
    return begin <= value && value < end;
}

void add_pointer_relation(const std::optional<format::DynamicPointer>& pointer,
                          std::string_view tag, GuestAddress value,
                          std::vector<std::string>& relations)
{
    if (pointer && pointer->address == value)
    {
        relations.emplace_back(tag);
    }
}

void add_range_relation(const std::optional<format::DynamicPointer>& pointer,
                        const std::optional<std::uint64_t>& size, std::string_view tag,
                        GuestAddress value, std::vector<std::string>& relations)
{
    if (!pointer || !size || size.value() == 0U)
    {
        return;
    }
    const auto end = checked_add_u64(pointer->address, size.value());
    if (end && range_contains(pointer->address, end.value(), value))
    {
        relations.emplace_back(tag);
    }
}

void add_symbol_relations(const format::DynamicSymbolTable* symbols, GuestAddress module_base,
                          GuestAddress value, std::vector<std::string>& relations)
{
    if (symbols == nullptr)
    {
        return;
    }
    constexpr std::uint16_t shn_abs = 0xfff1U;
    for (const auto& symbol : symbols->symbols)
    {
        if (!symbol.is_defined() || symbol.name.empty())
        {
            continue;
        }
        const auto address = symbol.section_index == shn_abs
                                 ? Result<GuestAddress>::success(symbol.value)
                                 : checked_add_u64(module_base, symbol.value);
        if (address && address.value() == value)
        {
            relations.push_back(std::to_string(symbol.index) + ":" + symbol.name);
        }
    }
}

[[nodiscard]] RuntimeAbiArgumentObservation observe_argument(
    const runtime::AArch64GuestCall& abi, std::size_t index, const memory::GuestMemory& memory,
    const format::DynamicInfo* dynamic, const format::DynamicSymbolTable* symbols)
{
    RuntimeAbiArgumentObservation observation;
    observation.index = index;
    if (index < 8U)
    {
        observation.location = "x" + std::to_string(index);
    }
    else
    {
        const auto offset = checked_mul_u64(static_cast<std::uint64_t>(index - 8U), 8U);
        observation.location = offset ? "[sp+0x" + [&]() {
            std::ostringstream text;
            text << std::hex << offset.value();
            return text.str();
        }() + "]" : "[sp+overflow]";
    }

    const auto value = abi.integer_argument(index);
    if (!value)
    {
        observation.diagnostic = std::string(error_code_name(value.error().code)) + ": " +
                                 value.error().message;
        observation.mapping = "invalid";
        observation.permissions = "unavailable";
        observation.region_kind = "unknown";
        observation.range_validity = "invalid_argument_slot";
        return observation;
    }
    observation.readable = true;
    observation.value = value.value();
    observation.aligned_8 = (value.value() & 0x7U) == 0U;
    observation.aligned_16 = (value.value() & 0xfU) == 0U;
    const auto region = memory.region_at(value.value());
    if (!region)
    {
        observation.mapping = value.value() == 0U ? "null" : "unmapped";
        observation.permissions = "unavailable";
        observation.region_kind = "unknown";
    }
    else
    {
        observation.mapping = region.value().name;
        observation.permissions = permissions_text(region.value().permissions);
        observation.region_kind = std::string(memory::guest_region_kind_name(region.value().kind));
    }
    if (dynamic != nullptr)
    {
        add_pointer_relation(dynamic->pltgot, "DT_PLTGOT", value.value(), observation.dynamic_tags);
        add_pointer_relation(dynamic->strtab, "DT_STRTAB", value.value(), observation.dynamic_tags);
        add_pointer_relation(dynamic->symtab, "DT_SYMTAB", value.value(), observation.dynamic_tags);
        add_pointer_relation(dynamic->rela, "DT_RELA", value.value(), observation.dynamic_tags);
        add_pointer_relation(dynamic->jmprel, "DT_JMPREL", value.value(), observation.dynamic_tags);
        add_pointer_relation(dynamic->init, "DT_INIT", value.value(), observation.dynamic_tags);
        add_pointer_relation(dynamic->fini, "DT_FINI", value.value(), observation.dynamic_tags);
        add_range_relation(dynamic->rela, dynamic->relasz, "DT_RELA", value.value(),
                           observation.relocation_ranges);
        add_range_relation(dynamic->jmprel, dynamic->pltrelsz, "DT_JMPREL", value.value(),
                           observation.relocation_ranges);
    }
    add_symbol_relations(symbols, dynamic == nullptr ? 0U : dynamic->module_base,
                         value.value(), observation.dynamic_symbols);
    observation.range_validity = "point_checked;extent_not_established";
    return observation;
}

void classify_import_trampoline(const analysis::FunctionRecord* record,
                                const runtime::ExecutionResult& boundary,
                                const ImportBoundary& import,
                                const format::DynamicInfo* dynamic,
                                RuntimeImportObservation& observation)
{
    observation.trampoline_classification = "not_classified";
    if (record == nullptr || !record->cfg)
    {
        observation.trampoline_evidence.push_back("no finalized CFG for current guest function");
        return;
    }
    const auto owned = analysis::function_owns_address(*record, boundary.boundary.source_guest_pc);
    const auto single_block = record->cfg->blocks.size() == 1U;
    bool final_br = false;
    bool source_matches = false;
    if (single_block)
    {
        const auto& block = record->cfg->blocks.begin()->second;
        if (!block.instructions.empty())
        {
            const auto& instruction = block.instructions.back();
            final_br = instruction.id == aarch64::InstructionId::Br;
            source_matches = instruction.address == boundary.boundary.source_guest_pc;
        }
    }
    const bool relocation_is_plt = import.relocation.source == format::RelocationSource::JmpRel;
    const bool relocation_is_jump_slot =
        import.relocation.type == format::AArch64RelocationType::JumpSlot;
    const bool provenance_matches = boundary.boundary.has_provenance_address &&
                                    boundary.boundary.provenance_address == import.relocation_target;
    observation.trampoline_evidence.push_back(
        owned ? "exact function ownership contains the indirect-branch PC"
              : "exact function ownership does not contain the indirect-branch PC");
    observation.trampoline_evidence.push_back(
        single_block ? "finalized CFG contains one basic block" : "finalized CFG is not one block");
    observation.trampoline_evidence.push_back(
        final_br ? "last decoded instruction is BR" : "last decoded instruction is not BR");
    observation.trampoline_evidence.push_back(
        source_matches ? "BR PC matches the execution boundary source PC"
                       : "BR PC does not match the execution boundary source PC");
    observation.trampoline_evidence.push_back(
        provenance_matches ? "BR target provenance is the unresolved relocation slot"
                            : "BR target provenance does not match the relocation slot");
    observation.trampoline_evidence.push_back(
        relocation_is_plt ? "relocation source is JMPREL" : "relocation source is not JMPREL");
    observation.trampoline_evidence.push_back(
        relocation_is_jump_slot ? "relocation type is R_AARCH64_JUMP_SLOT"
                                : "relocation type is not R_AARCH64_JUMP_SLOT");
    if (dynamic != nullptr && dynamic->jmprel && dynamic->jmprel_count)
    {
        observation.trampoline_evidence.push_back("DT_JMPREL metadata and entry count are parsed");
    }
    else
    {
        observation.trampoline_evidence.push_back("DT_JMPREL metadata is unavailable");
    }
    if (dynamic != nullptr && dynamic->pltgot)
    {
        observation.trampoline_evidence.push_back(
            "DT_PLTGOT metadata is parsed; relocation slot is retained for explicit GOT comparison");
    }

    if (owned && single_block && final_br && source_matches && provenance_matches &&
        relocation_is_plt && relocation_is_jump_slot)
    {
        observation.trampoline_classification = "import_trampoline";
    }
    else
    {
        observation.trampoline_classification = "indirect_import_candidate";
    }
}

} // namespace

const char* entry_selection_kind_name(EntrySelectionKind kind) noexcept
{
    switch (kind)
    {
    case EntrySelectionKind::DynamicInit: return "dynamic_init";
    case EntrySelectionKind::DynamicFini: return "dynamic_fini";
    case EntrySelectionKind::TextStartCandidate: return "text_start_candidate";
    case EntrySelectionKind::AnalystAddress: return "analyst_address";
    case EntrySelectionKind::VerifiedProcessEntry: return "verified_process_entry";
    }
    return "unknown";
}

const char* ir_operation_limit_provenance_name(
    IrOperationLimitProvenance provenance) noexcept
{
    switch (provenance)
    {
    case IrOperationLimitProvenance::OrdinaryDefault: return "ordinary_default";
    case IrOperationLimitProvenance::ExplicitCli: return "explicit_cli";
    case IrOperationLimitProvenance::ExplicitLibraryApi: return "explicit_library_api";
    }
    return "unknown";
}

Result<EntrySelection> select_entry(const analysis::ModuleIdentity& identity,
                                    EntrySelectionKind kind,
                                    std::optional<GuestAddress> analyst_address)
{
    if (kind == EntrySelectionKind::AnalystAddress)
    {
        if (!analyst_address)
        {
            return Result<EntrySelection>::failure(make_error(
                ErrorCode::InvalidArgument,
                "analyst address entry selection requires --entry-address"));
        }
        return Result<EntrySelection>::success(EntrySelection{
            kind, analyst_address.value(), "analyst-supplied guest address",
            analysis::FunctionConfidence::Manual, false});
    }

    const auto wanted = [&]() -> std::optional<analysis::EntryPointKind> {
        switch (kind)
        {
        case EntrySelectionKind::DynamicInit: return analysis::EntryPointKind::DynamicInit;
        case EntrySelectionKind::DynamicFini: return analysis::EntryPointKind::DynamicFini;
        case EntrySelectionKind::TextStartCandidate:
            return analysis::EntryPointKind::TextStartCandidate;
        case EntrySelectionKind::VerifiedProcessEntry:
            return analysis::EntryPointKind::VerifiedProcessEntry;
        case EntrySelectionKind::AnalystAddress: return std::nullopt;
        }
        return std::nullopt;
    }();

    if (!wanted)
    {
        return Result<EntrySelection>::failure(make_error(
            ErrorCode::InvalidArgument, "entry selection kind is not supported"));
    }

    for (const auto& evidence : identity.entry_points)
    {
        if (evidence.kind == wanted.value() &&
            (kind != EntrySelectionKind::VerifiedProcessEntry || evidence.verified_runtime_entry))
        {
            return Result<EntrySelection>::success(EntrySelection{
                kind, evidence.address, evidence.provenance, evidence.confidence,
                evidence.verified_runtime_entry});
        }
    }

    if (kind == EntrySelectionKind::VerifiedProcessEntry)
    {
        return Result<EntrySelection>::failure(make_error(
            ErrorCode::InvalidArgument,
            "no verified process entry exists for this single-module prepared input"));
    }
    return Result<EntrySelection>::failure(make_error(
        ErrorCode::InvalidArgument,
        std::string("requested entry kind has no metadata candidate: ") +
            entry_selection_kind_name(kind)));
}

const char* execution_stop_reason_name(ExecutionStopReason reason) noexcept
{
    switch (reason)
    {
    case ExecutionStopReason::EntryReturned: return "entry_returned";
    case ExecutionStopReason::UnresolvedImport: return "unresolved_import";
    case ExecutionStopReason::UnknownGuestFunction: return "unknown_guest_function";
    case ExecutionStopReason::InvalidIndirectTarget: return "invalid_indirect_target";
    case ExecutionStopReason::UnresolvedIndirectControlFlow:
        return "unresolved_indirect_control_flow";
    case ExecutionStopReason::UnsupportedInstruction: return "unsupported_instruction";
    case ExecutionStopReason::UnsupportedSemantic: return "unsupported_semantic";
    case ExecutionStopReason::FunctionOwnershipConflict: return "function_ownership_conflict";
    case ExecutionStopReason::MemoryFault: return "memory_fault";
    case ExecutionStopReason::RuntimeServiceBoundary: return "runtime_service_boundary";
    case ExecutionStopReason::RuntimeImportUnimplemented: return "runtime_import_unimplemented";
    case ExecutionStopReason::RuntimeImportAbiViolation: return "runtime_import_abi_violation";
    case ExecutionStopReason::RuntimeImportMemoryFault: return "runtime_import_memory_fault";
    case ExecutionStopReason::RuntimeImportInvariantViolation:
        return "runtime_import_invariant_violation";
    case ExecutionStopReason::GuestTrap: return "guest_trap";
    case ExecutionStopReason::ReturnTargetMismatch: return "return_target_mismatch";
    case ExecutionStopReason::CallDepthExceeded: return "call_depth_exceeded";
    case ExecutionStopReason::FunctionTransitionLimitExceeded:
        return "function_transition_limit_exceeded";
    case ExecutionStopReason::IrOperationLimitExceeded: return "ir_operation_limit_exceeded";
    case ExecutionStopReason::EventLimitExceeded: return "event_limit_exceeded";
    case ExecutionStopReason::GuestBlockLimitExceeded: return "guest_block_limit_exceeded";
    case ExecutionStopReason::GuestMemoryResourceLimitExceeded:
        return "guest_memory_resource_limit_exceeded";
    case ExecutionStopReason::InvalidCrossModuleTarget: return "invalid_cross_module_target";
    case ExecutionStopReason::IndirectTargetRefinementBudgetExceeded:
        return "indirect_target_refinement_budget_exceeded";
    }
    return "unknown";
}

const char* function_transition_category_name(FunctionTransitionCategory category) noexcept
{
    switch (category)
    {
    case FunctionTransitionCategory::InitialEntry: return "initial_entry";
    case FunctionTransitionCategory::DirectCall: return "direct_call";
    case FunctionTransitionCategory::IndirectCall: return "indirect_call";
    case FunctionTransitionCategory::FunctionTransfer: return "function_transfer";
    case FunctionTransitionCategory::FunctionResume: return "function_resume";
    case FunctionTransitionCategory::Other: return "other";
    }
    return "unknown";
}

const char* function_transition_link_register_action_name(
    FunctionTransitionLinkRegisterAction action) noexcept
{
    switch (action)
    {
    case FunctionTransitionLinkRegisterAction::NotApplicable: return "not_applicable";
    case FunctionTransitionLinkRegisterAction::InitializedSyntheticReturn:
        return "initialized_synthetic_return";
    case FunctionTransitionLinkRegisterAction::WrittenArchitecturalReturnPc:
        return "written_architectural_return_pc";
    case FunctionTransitionLinkRegisterAction::Preserved: return "preserved";
    }
    return "unknown";
}

const char* function_transition_target_ownership_name(
    FunctionTransitionTargetOwnership ownership) noexcept
{
    switch (ownership)
    {
    case FunctionTransitionTargetOwnership::NotOwned: return "not_owned";
    case FunctionTransitionTargetOwnership::ExactCandidateFunctionEntry:
        return "exact_candidate_function_entry";
    case FunctionTransitionTargetOwnership::ExactTrustedFunctionEntry:
        return "exact_trusted_function_entry";
    case FunctionTransitionTargetOwnership::Conflict: return "conflict";
    }
    return "unknown";
}

const char* execution_eligibility_name(ExecutionEligibility eligibility) noexcept
{
    switch (eligibility)
    {
    case ExecutionEligibility::Runnable: return "runnable";
    case ExecutionEligibility::RunnableToBoundary: return "runnable_to_boundary";
    case ExecutionEligibility::OwnershipConflict: return "ownership_conflict";
    case ExecutionEligibility::UnsupportedInstruction: return "unsupported_instruction";
    case ExecutionEligibility::InvalidCFG: return "invalid_cfg";
    case ExecutionEligibility::IrVerificationFailure: return "ir_verification_failure";
    case ExecutionEligibility::UnknownFunction: return "unknown_function";
    case ExecutionEligibility::OtherFailure: return "other_failure";
    }
    return "unknown";
}

const char* execution_event_kind_name(ExecutionEventKind kind) noexcept
{
    switch (kind)
    {
    case ExecutionEventKind::SessionStart: return "session_start";
    case ExecutionEventKind::EntrySelected: return "entry_selected";
    case ExecutionEventKind::StackMapped: return "stack_mapped";
    case ExecutionEventKind::FunctionEnter: return "function_enter";
    case ExecutionEventKind::DirectCall: return "direct_call";
    case ExecutionEventKind::IndirectCall: return "indirect_call";
    case ExecutionEventKind::FunctionTransfer: return "function_transfer";
    case ExecutionEventKind::Return: return "return";
    case ExecutionEventKind::FunctionResume: return "function_resume";
    case ExecutionEventKind::ImportBoundary: return "import_boundary";
    case ExecutionEventKind::RuntimeImportResolved: return "runtime_import_resolved";
    case ExecutionEventKind::RuntimeImportEnter: return "runtime_import_enter";
    case ExecutionEventKind::RuntimeImportArgumentSummary: return "runtime_import_argument_summary";
    case ExecutionEventKind::RuntimeStateRegistration: return "runtime_state_registration";
    case ExecutionEventKind::RuntimeImportReturn: return "runtime_import_return";
    case ExecutionEventKind::RuntimeImportBoundary: return "runtime_import_boundary";
    case ExecutionEventKind::IndirectBoundary: return "indirect_boundary";
    case ExecutionEventKind::MemoryFault: return "memory_fault";
    case ExecutionEventKind::UnsupportedBoundary: return "unsupported_boundary";
    case ExecutionEventKind::SessionStop: return "session_stop";
    }
    return "unknown";
}

const char* event_execution_limit_provenance_name(
    EventExecutionLimitProvenance provenance) noexcept
{
    switch (provenance)
    {
    case EventExecutionLimitProvenance::NotConfigured: return "not_configured";
    case EventExecutionLimitProvenance::ExplicitCli: return "explicit_cli";
    case EventExecutionLimitProvenance::ExplicitLocalConfiguration:
        return "explicit_local_configuration";
    case EventExecutionLimitProvenance::ExplicitLibraryApi: return "explicit_library_api";
    }
    return "unknown";
}

Result<std::size_t> checked_next_execution_event_sequence(std::size_t total_generated)
{
    const auto next = checked_add(total_generated, 1U);
    if (!next)
    {
        return Result<std::size_t>::failure(make_error(
            ErrorCode::ArithmeticOverflow,
            "logical execution event sequence overflowed before emission"));
    }
    return next;
}

ImportBoundaryIndex::ImportBoundaryIndex(
    const std::vector<loader::UnresolvedRelocation>& relocations)
{
    std::vector<const loader::UnresolvedRelocation*> ordered;
    ordered.reserve(relocations.size());
    for (const auto& relocation : relocations)
    {
        ordered.push_back(&relocation);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        if (left->relocation.target_address != right->relocation.target_address)
        {
            return left->relocation.target_address < right->relocation.target_address;
        }
        return left->relocation_index < right->relocation_index;
    });
    for (const auto* unresolved : ordered)
    {
        entries_.emplace(unresolved->relocation.target_address,
                         ImportBoundary{unresolved->relocation.target_address,
                                         unresolved->relocation_index,
                                         unresolved->relocation,
                                         unresolved->symbol,
                                         {}});
    }
}

ImportBoundaryIndex::ImportBoundaryIndex(const analysis::ProcessImage& process_image)
{
    struct Item
    {
        const loader::UnresolvedRelocation* relocation = nullptr;
        std::string module;
    };
    std::vector<Item> ordered;
    for (const auto& module : process_image.modules())
    {
        for (const auto& relocation : module.unresolved_relocations)
        {
            ordered.push_back(Item{&relocation, module.identity.module});
        }
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        if (left.relocation->relocation.target_address !=
            right.relocation->relocation.target_address)
        {
            return left.relocation->relocation.target_address <
                   right.relocation->relocation.target_address;
        }
        if (left.module != right.module) return left.module < right.module;
        return left.relocation->relocation_index < right.relocation->relocation_index;
    });
    for (const auto& item : ordered)
    {
        const auto& unresolved = *item.relocation;
        entries_.emplace(unresolved.relocation.target_address,
                         ImportBoundary{unresolved.relocation.target_address,
                                         unresolved.relocation_index,
                                         unresolved.relocation,
                                         unresolved.symbol,
                                         item.module});
    }
}

const ImportBoundary* ImportBoundaryIndex::find(GuestAddress relocation_target) const noexcept
{
    const auto found = entries_.find(relocation_target);
    return found == entries_.end() ? nullptr : &found->second;
}

ExecutionSession::ExecutionSession(
    memory::GuestMemory& memory, const analysis::FinalizedFunctionMap& function_map,
    const std::vector<loader::UnresolvedRelocation>& unresolved_relocations,
    ExecutionSessionOptions options, ExecutionLoadSummary load_summary,
    runtime::RuntimeImportRegistry* runtime_imports,
    const format::ModuleMetadata* module_metadata,
    const format::DynamicSymbolTable* symbols)
    : memory_(&memory), function_map_(&function_map), imports_(unresolved_relocations),
      runtime_imports_(runtime_imports == nullptr ? &empty_runtime_imports_ : runtime_imports),
      module_metadata_(module_metadata), symbols_(symbols), options_(std::move(options)),
      load_summary_(load_summary)
{
}

ExecutionSession::ExecutionSession(
    memory::GuestMemory& memory, const analysis::ProcessFunctionMap& function_map,
    const analysis::ProcessImage& process_image, ExecutionSessionOptions options,
    ExecutionLoadSummary load_summary, runtime::RuntimeImportRegistry* runtime_imports)
    : memory_(&memory), process_function_map_(&function_map), process_image_(&process_image),
      imports_(process_image),
      runtime_imports_(runtime_imports == nullptr ? &empty_runtime_imports_ : runtime_imports),
      options_(std::move(options)), load_summary_(load_summary)
{
    for (const auto& map : function_map.maps())
    {
        if (map.identity().module == process_image.summary().primary_module)
        {
            function_map_ = &map;
            break;
        }
    }
}

ExecutionSession::ExecutionSession(
    memory::GuestMemory& memory, const analysis::ProcessFunctionMap& function_map,
    const std::vector<loader::UnresolvedRelocation>& unresolved_relocations,
    ExecutionSessionOptions options, ExecutionLoadSummary load_summary,
    runtime::RuntimeImportRegistry* runtime_imports)
    : memory_(&memory), process_function_map_(&function_map), imports_(unresolved_relocations),
      runtime_imports_(runtime_imports == nullptr ? &empty_runtime_imports_ : runtime_imports),
      options_(std::move(options)), load_summary_(load_summary)
{
    if (!function_map.maps().empty()) function_map_ = &function_map.maps().front();
}

ExecutionSession::~ExecutionSession() noexcept
{
    if (!stack_mapping_) return;
    const auto released = release_stack();
    if (!released) std::terminate();
}

Result<void> ExecutionSession::release_stack() noexcept
{
    if (!stack_mapping_) return Result<void>::success();
    if (memory_ == nullptr)
    {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidArgument, "controlled stack has no guest memory owner"));
    }
    const auto released = memory_->release_owned(stack_mapping_.value());
    if (released) stack_mapping_.reset();
    return released;
}

const analysis::FunctionRecord* ExecutionSession::function_record(GuestAddress entry) const noexcept
{
    if (process_function_map_ != nullptr) return process_function_map_->find(entry);
    return function_map_ == nullptr ? nullptr : function_map_->find(entry);
}

const analysis::FinalizedFunctionMap* ExecutionSession::function_map_for(
    GuestAddress entry) const noexcept
{
    if (process_function_map_ != nullptr) return process_function_map_->map_for(entry);
    return function_map_;
}

std::string ExecutionSession::module_name_for(GuestAddress entry) const
{
    const auto* map = function_map_for(entry);
    if (map != nullptr) return map->identity().module;
    if (process_image_ != nullptr)
    {
        if (const auto* module = process_image_->module_for_address(entry, 4U))
            return module->identity.module;
    }
    return {};
}

void ExecutionSession::prepare_observation_targets()
{
    observation_targets_.clear();
    instruction_observation_targets_.clear();
    if (process_image_ != nullptr)
    {
        // The historical observation_targets field remains the UMULH-only set.
        // M19's richer instruction observations are derived from the same parsed
        // CFG and use a separate additive target list for move-wide instructions.
        for (const auto& binding : process_image_->bindings())
        {
            if (binding.symbol != "__nnmusl_init_dso" || !binding.provider_address || !binding.applied)
                continue;
            const auto* record = function_record(binding.provider_address.value());
            if (record == nullptr || !record->cfg) continue;
            for (const auto& [unused, block] : record->cfg->blocks)
            {
                (void)unused;
                for (const auto& instruction : block.instructions)
                {
                    if (instruction.id == aarch64::InstructionId::Umulh)
                    {
                        observation_targets_.push_back(instruction.address);
                        instruction_observation_targets_.push_back(instruction.address);
                    }
                    else if (instruction.id == aarch64::InstructionId::Movz ||
                             instruction.id == aarch64::InstructionId::Movn ||
                             instruction.id == aarch64::InstructionId::Movk)
                    {
                        instruction_observation_targets_.push_back(instruction.address);
                    }
                }
            }
        }
    }

    // SMULH observations are collected from finalized guest ownership rather
    // than from a game-specific address. This keeps the typed frontier useful
    // for any process image while retaining the historical UMULH target set.
    const auto collect_smulh = [&](const analysis::FinalizedFunctionMap& map) {
        for (const auto& function : map.functions())
        {
            if (!function.cfg) continue;
            for (const auto& [unused, block] : function.cfg->blocks)
            {
                (void)unused;
                for (const auto& instruction : block.instructions)
                {
                    if (instruction.id == aarch64::InstructionId::Smulh)
                        instruction_observation_targets_.push_back(instruction.address);
                }
            }
        }
    };
    if (process_function_map_ != nullptr)
    {
        for (const auto& map : process_function_map_->maps()) collect_smulh(map);
    }
    else if (function_map_ != nullptr)
    {
        collect_smulh(*function_map_);
    }
    std::sort(observation_targets_.begin(), observation_targets_.end());
    observation_targets_.erase(
        std::unique(observation_targets_.begin(), observation_targets_.end()),
        observation_targets_.end());
    std::sort(instruction_observation_targets_.begin(), instruction_observation_targets_.end());
    instruction_observation_targets_.erase(
        std::unique(instruction_observation_targets_.begin(), instruction_observation_targets_.end()),
        instruction_observation_targets_.end());
}

std::optional<ExecutedGuestInstruction> ExecutionSession::describe_observed_instruction(
    const runtime::ObservedInstructionExecution& observation, std::size_t call_depth) const
{
    const auto guest_pc = observation.guest_pc;
    const analysis::FunctionRecord* owner = nullptr;
    const analysis::BasicBlock* owner_block = nullptr;
    const auto inspect_map = [&](const analysis::FinalizedFunctionMap& map) {
        for (const auto& candidate : map.functions())
        {
            if (!candidate.cfg) continue;
            for (const auto& [unused, block] : candidate.cfg->blocks)
            {
                (void)unused;
                const auto found = std::find_if(
                    block.instructions.begin(), block.instructions.end(),
                    [&](const auto& instruction) { return instruction.address == guest_pc; });
                if (found != block.instructions.end())
                {
                    owner = &candidate;
                    owner_block = &block;
                    return;
                }
            }
            if (owner != nullptr) return;
        }
    };
    if (process_function_map_ != nullptr)
    {
        for (const auto& map : process_function_map_->maps())
        {
            inspect_map(map);
            if (owner != nullptr) break;
        }
    }
    else if (function_map_ != nullptr)
    {
        inspect_map(*function_map_);
    }
    if (owner == nullptr || owner_block == nullptr || !owner->cfg) return std::nullopt;
    for (std::size_t index = 0U; index < owner_block->instructions.size(); ++index)
    {
        const auto& instruction = owner_block->instructions[index];
        if (instruction.address != guest_pc ||
            (instruction.id != aarch64::InstructionId::Movz &&
             instruction.id != aarch64::InstructionId::Movn &&
             instruction.id != aarch64::InstructionId::Movk &&
             instruction.id != aarch64::InstructionId::Umulh &&
             instruction.id != aarch64::InstructionId::Smulh))
            continue;
        ExecutedGuestInstruction result;
        result.guest_pc = guest_pc;
        result.module = owner->module;
        result.mnemonic = instruction.disassembly.empty()
                              ? std::string(aarch64::instruction_id_name(instruction.id))
                              : instruction.disassembly;
        result.instruction_id = std::string(aarch64::instruction_id_name(instruction.id));
        result.opcode = instruction.opcode;
        result.executed = true;
        result.call_depth = call_depth;
        if (!instruction.operands.empty() &&
            instruction.operands.front().kind == aarch64::OperandKind::Register)
        {
            result.destination_register = aarch64::register_name(instruction.operands.front().reg);
        }
        for (std::size_t operand_index = 1U; operand_index < instruction.operands.size();
             ++operand_index)
        {
            const auto& operand = instruction.operands[operand_index];
            if (operand.kind == aarch64::OperandKind::Register)
                result.source_registers.push_back(aarch64::register_name(operand.reg));
        }
        if (observation.next_guest_pc)
        {
            result.next_guest_pc = observation.next_guest_pc;
        }
        else if (index + 1U < owner_block->instructions.size())
        {
            result.next_guest_pc = owner_block->instructions[index + 1U].address;
        }
        else if (instruction.control_flow.kind == aarch64::ControlFlowKind::Fallthrough)
        {
            const auto next = checked_add_u64(guest_pc, 4U);
            if (next) result.next_guest_pc = next.value();
        }
        if ((instruction.id == aarch64::InstructionId::Umulh ||
             instruction.id == aarch64::InstructionId::Smulh) &&
            instruction.operands.size() >= 3U &&
            instruction.operands[1].kind == aarch64::OperandKind::Register &&
            instruction.operands[2].kind == aarch64::OperandKind::Register)
        {
            const auto& left = instruction.operands[1].reg;
            const auto& right = instruction.operands[2].reg;
            result.pre_registers.push_back(
                ExecutedGuestInstruction::RegisterValue{architectural_register_name(left),
                                                        architectural_register_value(observation.pre_state, left)});
            result.pre_registers.push_back(
                ExecutedGuestInstruction::RegisterValue{architectural_register_name(right),
                                                        architectural_register_value(observation.pre_state, right)});
            const auto left_value = architectural_register_value(observation.pre_state, left);
            const auto right_value = architectural_register_value(observation.pre_state, right);
            const auto expected = instruction.id == aarch64::InstructionId::Smulh
                                      ? independent_smulh_oracle(left_value, right_value)
                                      : independent_umulh_oracle(left_value, right_value);
            result.expected_high64 = expected;
            if (!instruction.operands.empty() &&
                instruction.operands.front().kind == aarch64::OperandKind::Register)
            {
                const auto& destination = instruction.operands.front().reg;
                result.post_registers.push_back(
                    ExecutedGuestInstruction::RegisterValue{
                        architectural_register_name(destination),
                        architectural_register_value(observation.post_state, destination)});
                result.actual_high64 = architectural_register_value(observation.post_state, destination);
                result.result_matches = result.actual_high64.value() == expected;
            }
        }
        else if ((instruction.id == aarch64::InstructionId::Movz ||
                  instruction.id == aarch64::InstructionId::Movn ||
                  instruction.id == aarch64::InstructionId::Movk) &&
                 !instruction.operands.empty() &&
                 instruction.operands.front().kind == aarch64::OperandKind::Register)
        {
            const auto& destination = instruction.operands.front().reg;
            const auto name = architectural_register_name(destination);
            result.pre_registers.push_back(
                ExecutedGuestInstruction::RegisterValue{name,
                                                        architectural_register_value(observation.pre_state, destination)});
            result.post_registers.push_back(
                ExecutedGuestInstruction::RegisterValue{name,
                                                        architectural_register_value(observation.post_state, destination)});
        }
        return result;
    }
    return std::nullopt;
}

ExecutionStopReason ExecutionSession::classify_error(const Error& error) const noexcept
{
    if (is_memory_error(error.code)) return ExecutionStopReason::MemoryFault;
    switch (error.code)
    {
    case ErrorCode::ExecutionTrap: return ExecutionStopReason::GuestTrap;
    case ErrorCode::ExecutionLimitExceeded: return ExecutionStopReason::IrOperationLimitExceeded;
    case ErrorCode::ResourceLimit: return ExecutionStopReason::GuestMemoryResourceLimitExceeded;
    case ErrorCode::FunctionBoundaryConflict:
        return ExecutionStopReason::FunctionOwnershipConflict;
    case ErrorCode::UnknownGuestFunction: return ExecutionStopReason::UnknownGuestFunction;
    case ErrorCode::UnresolvedIndirectFlow:
        return ExecutionStopReason::UnresolvedIndirectControlFlow;
    case ErrorCode::RuntimeBoundary: return ExecutionStopReason::RuntimeServiceBoundary;
    case ErrorCode::RuntimeImportUnimplemented:
        return ExecutionStopReason::RuntimeImportUnimplemented;
    case ErrorCode::RuntimeImportAbiViolation:
        return ExecutionStopReason::RuntimeImportAbiViolation;
    case ErrorCode::RuntimeImportMemoryFault:
        return ExecutionStopReason::RuntimeImportMemoryFault;
    case ErrorCode::RuntimeImportInvariantViolation:
        return ExecutionStopReason::RuntimeImportInvariantViolation;
    case ErrorCode::UnsupportedInstruction:
    case ErrorCode::UnsupportedOperandForm:
    case ErrorCode::Unsupported: return ExecutionStopReason::UnsupportedInstruction;
    default: return ExecutionStopReason::UnsupportedSemantic;
    }
}

Result<bool> ExecutionSession::record_event(ExecutionSessionResult& result, ExecutionEvent event)
{
    event.guest_instruction_count = result.guest_instruction_count;
    event.guest_block_count = result.guest_blocks;
    event.ir_operation_count = result.ir_operations;
    if (result.event_resource.execution_limit &&
        result.event_resource.total_generated >= result.event_resource.execution_limit.value())
    {
        result.stop_reason = ExecutionStopReason::EventLimitExceeded;
        result.diagnostic = "execution event limit exhausted (consumed=" +
                            std::to_string(result.event_resource.total_generated) +
                            " limit=" +
                            std::to_string(result.event_resource.execution_limit.value()) +
                            " attempted_sequence=" +
                            std::to_string(result.event_resource.total_generated) + ")";
        event.sequence = result.event_resource.total_generated;
        result.event_resource.terminal_attempt_sequence = event.sequence;
        result.event_resource.terminal_attempt_kind = event.kind;
        result.event_resource.terminal_attempt = event;
        result.final_cpu = cpu_;
        result.current_function = current_.function_entry;
        result.current_function_module = module_name_for(current_.function_entry);
        result.stop_module = event.target_module.empty() ? result.current_function_module
                                                         : event.target_module;
        result.stop_pc = event.guest_pc == 0U ? cpu_.pc : event.guest_pc;
        running_ = false;
        return Result<bool>::success(false);
    }
    const auto next_total = checked_next_execution_event_sequence(
        result.event_resource.total_generated);
    if (!next_total)
    {
        running_ = false;
        return Result<bool>::failure(next_total.error());
    }
    const auto kind_index = static_cast<std::size_t>(event.kind);
    if (kind_index >= execution_event_kind_count)
    {
        running_ = false;
        return Result<bool>::failure(
            make_error(ErrorCode::InvalidArgument, "execution event kind is out of range"));
    }
    const auto next_kind_count = checked_add(result.event_resource.by_kind[kind_index], 1U);
    if (!next_kind_count)
    {
        running_ = false;
        return Result<bool>::failure(make_error(
            ErrorCode::ArithmeticOverflow, "execution event kind counter overflowed"));
    }

    event.sequence = result.event_resource.total_generated;
    result.event_resource.total_generated = next_total.value();
    result.event_resource.by_kind[kind_index] = next_kind_count.value();

    const auto retention_limit = options_.budgets.event_history_limit;
    if (result.events.size() < retention_limit)
    {
        result.events.push_back(std::move(event));
    }
    else
    {
        const auto prefix_limit = retention_limit / 2U;
        if (!result.event_resource.history_truncated)
        {
            result.events.erase(result.events.begin() + static_cast<std::ptrdiff_t>(prefix_limit),
                                result.events.end());
            result.event_resource.history_truncated = true;
        }
        else
        {
            // The prefix is immutable. Once the recent window is full, evict
            // only its oldest logical event before appending the new one.
            result.events.erase(result.events.begin() + static_cast<std::ptrdiff_t>(prefix_limit));
        }
        result.events.push_back(std::move(event));
    }
    result.event_resource.retained = result.events.size();
    const auto omitted = checked_sub(result.event_resource.total_generated,
                                     result.event_resource.retained);
    if (!omitted)
    {
        running_ = false;
        return Result<bool>::failure(omitted.error());
    }
    result.event_resource.omitted = omitted.value();
    result.event_resource.first_sequence_retained = result.events.empty()
                                                        ? std::nullopt
                                                        : std::optional<std::size_t>(
                                                              result.events.front().sequence);
    result.event_resource.last_sequence_retained = result.events.empty()
                                                       ? std::nullopt
                                                       : std::optional<std::size_t>(
                                                             result.events.back().sequence);
    const auto reconciled = checked_add(result.event_resource.retained,
                                        result.event_resource.omitted);
    result.event_resource.reconciles = reconciled &&
                                       reconciled.value() == result.event_resource.total_generated;
    return Result<bool>::success(true);
}

Result<void> ExecutionSession::map_stack(ExecutionSessionResult& result)
{
    if (memory_ == nullptr || options_.synthetic_stack_size == 0U)
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument, "controlled execution requires a non-empty synthetic stack"));
    }
    // GuestMemory retains only a virtual high-water mark after an owned
    // mapping is released. This preserves the old append-only guest-visible
    // stack address sequence without retaining the old backing storage.
    const auto gap_end = checked_add_u64(memory_->accounting().virtual_address_high_water,
                                         options_.stack_guard_gap);
    if (!gap_end)
    {
        return Result<void>::failure(make_error(
            ErrorCode::ArithmeticOverflow, "synthetic stack guard gap overflows"));
    }
    constexpr GuestAddress page_size = 0x1000U;
    const auto base = align_up(gap_end.value(), page_size);
    if (!base)
    {
        return Result<void>::failure(base.error());
    }
    const auto size = align_up(options_.synthetic_stack_size, page_size);
    if (!size)
    {
        return Result<void>::failure(size.error());
    }
    const auto end = checked_add_u64(base.value(), size.value());
    if (!end)
    {
        return Result<void>::failure(make_error(
            ErrorCode::ArithmeticOverflow, "synthetic stack end overflows"));
    }
    const auto mapped = memory_->map_owned(
        base.value(), size.value(), memory::GuestMemoryPermissions::Read |
                                       memory::GuestMemoryPermissions::Write,
        "synthetic.controlled.stack", memory::GuestRegionKind::Other);
    if (!mapped)
    {
        return Result<void>::failure(mapped.error());
    }
    stack_mapping_ = std::move(mapped).value();
    result.stack_base = base.value();
    result.stack_end = end.value();
    result.initial_sp = result.stack_end & ~GuestAddress{0xfU};
    if ((result.initial_sp & 0xfU) != 0U || result.initial_sp <= result.stack_base)
    {
        const auto released = release_stack();
        if (!released) return released;
        return Result<void>::failure(make_error(
            ErrorCode::InvalidGuestAddress, "synthetic stack cannot provide an aligned initial SP"));
    }
    const auto recorded = record_event(
        result, ExecutionEvent{0U, ExecutionEventKind::StackMapped, 0U, 0U,
                               result.stack_base, true, 0U,
                               runtime::ExecutionBoundaryKind::None, {}, {}, {}, {}});
    if (!recorded)
    {
        return Result<void>::failure(recorded.error());
    }
    if (!recorded.value())
    {
        return Result<void>::success();
    }
    return Result<void>::success();
}

Result<const ir::Function*> ExecutionSession::lift_for_execution(
    GuestAddress entry, ExecutionSessionResult& result)
{
    ProfileTimer timer("execution.lift_for_execution", &profile_totals_.lift_elapsed_us,
                       &profile_totals_.lift_calls);
    const auto* map = function_map_for(entry);
    const auto* record = function_record(entry);
    const auto identity = record != nullptr && record->cfg ? cfg_identity(record->cfg.value()) : 0U;
    const auto found = lift_cache_.find(entry);
    if (found != lift_cache_.end() && found->second.function_map == map &&
        found->second.cfg_identity == identity)
    {
        ++result.performance.lift_cache_hits;
        if (found->second.function) return Result<const ir::Function*>::success(&found->second.function.value());
        return Result<const ir::Function*>::failure(found->second.error.value());
    }
    if (found != lift_cache_.end())
    {
        ++result.performance.lift_cache_invalidations;
        lift_cache_.erase(found);
    }
    ++result.performance.lift_cache_misses;

    if (record == nullptr)
    {
        const auto error = make_error(ErrorCode::UnknownGuestFunction,
                                      "guest address is not an exact finalized function entry");
        lift_cache_.emplace(entry, LiftCacheEntry{std::nullopt, error, map, identity});
        return Result<const ir::Function*>::failure(error);
    }
    if (record->translation_status == analysis::TranslationStatus::Conflict)
    {
        std::ostringstream message;
        message << "function ownership conflict for " << hex_address(entry);
        const auto* owning_map = function_map_for(entry);
        for (const auto& conflict : owning_map == nullptr
                                         ? std::vector<analysis::FunctionBoundaryConflict>{}
                                         : owning_map->conflicts())
        {
            if (conflict.first_function == entry || conflict.second_function == entry)
            {
                message << "; conflicts with "
                        << hex_address(conflict.first_function == entry
                                           ? conflict.second_function
                                           : conflict.first_function);
                for (const auto& range : conflict.overlap_ranges)
                {
                    message << " overlap " << hex_address(range.base) << "+" << range.size;
                }
            }
        }
        const auto error = make_error(ErrorCode::FunctionBoundaryConflict, message.str());
        lift_cache_.emplace(entry, LiftCacheEntry{std::nullopt, error, map, identity});
        return Result<const ir::Function*>::failure(error);
    }
    if (!record->cfg)
    {
        const auto error = make_error(
            ErrorCode::UnsupportedInstruction,
            "finalized function has no executable CFG: " + hex_address(entry));
        lift_cache_.emplace(entry, LiftCacheEntry{std::nullopt, error, map, identity});
        return Result<const ir::Function*>::failure(error);
    }
    lifter::LiftOptions lift_options;
    lift_options.stop_at_unsupported_instruction = true;
    const auto lifted = lifter::lift_function(record->cfg.value(), lift_options);
    if (!lifted)
    {
        lift_cache_.emplace(entry, LiftCacheEntry{std::nullopt, lifted.error(), map, identity});
        return Result<const ir::Function*>::failure(lifted.error());
    }
    auto inserted = lift_cache_.emplace(entry, LiftCacheEntry{lifted.value(), std::nullopt, map, identity});
    ++result.performance.functions_lifted;
    return Result<const ir::Function*>::success(&inserted.first->second.function.value());
}

FunctionTransitionEvidence ExecutionSession::make_transition_evidence(
    const TransitionRequest& request, GuestAddress target,
    const ExecutionSessionResult& result) const
{
    FunctionTransitionEvidence evidence;
    evidence.sequence = result.transition_accounting.total_function_entries + 1U;
    evidence.category = request.category;
    evidence.resource_charged = request.category == FunctionTransitionCategory::FunctionTransfer;
    if (evidence.resource_charged)
    {
        evidence.resource_sequence = result.transition_accounting.total_charged + 1U;
    }
    evidence.target_module = module_name_for(target);
    evidence.target_function = target;
    evidence.call_depth_before = request.call_depth_before;
    evidence.call_depth_after = request.call_depth_after;
    evidence.boundary = request.boundary;
    evidence.link_register_action = request.link_register_action;
    evidence.expected_return_pc = request.expected_return_pc;
    evidence.call_frame_pushed = request.call_frame_pushed;
    evidence.target_register = request.target_register;
    evidence.target_provenance = request.target_provenance;
    evidence.guest_instruction_count = result.guest_instruction_count;
    evidence.guest_block_count = result.guest_blocks;
    evidence.ir_operation_count = result.ir_operations;

    if (request.has_source)
    {
        evidence.source_function = request.source_function;
        evidence.source_module = module_name_for(request.source_function);
        evidence.target_equals_current_function = request.source_function == target;
        if (const auto* source_record = function_record(request.source_function))
        {
            evidence.source_canonical_function = source_record->canonical_entry;
            if (request.has_source_pc)
            {
                evidence.source_pc_owned_by_source_function =
                    analysis::function_owns_address(*source_record, request.source_guest_pc);
            }
        }
    }
    if (request.has_source_pc)
    {
        evidence.source_guest_pc = request.source_guest_pc;
        if (memory_ != nullptr)
        {
            const auto decoder = aarch64::AArch64Decoder::create();
            if (decoder)
            {
                const auto instruction = aarch64::fetch_and_decode(
                    *memory_, *decoder.value(), request.source_guest_pc);
                if (instruction)
                {
                    evidence.source_opcode = instruction.value().opcode;
                    evidence.source_instruction_id =
                        aarch64::instruction_id_name(instruction.value().id);
                    evidence.source_instruction = instruction.value().disassembly;
                }
            }
        }
    }

    const auto* target_record = function_record(target);
    if (target_record == nullptr)
    {
        evidence.target_ownership = FunctionTransitionTargetOwnership::NotOwned;
    }
    else if (target_record->translation_status == analysis::TranslationStatus::Conflict ||
             target_record->entry_trust_status == analysis::FunctionEntryTrustStatus::Conflict)
    {
        evidence.target_ownership = FunctionTransitionTargetOwnership::Conflict;
    }
    else if (target_record->entry_trust_status == analysis::FunctionEntryTrustStatus::Trusted)
    {
        evidence.target_ownership = FunctionTransitionTargetOwnership::ExactTrustedFunctionEntry;
    }
    else
    {
        evidence.target_ownership = FunctionTransitionTargetOwnership::ExactCandidateFunctionEntry;
    }
    if (target_record != nullptr) evidence.target_canonical_function = target_record->canonical_entry;

    const auto& history = result.transition_accounting.history;
    evidence.previous_target_count = static_cast<std::size_t>(std::count_if(
        history.begin(), history.end(), [target](const auto& prior) {
            return prior.target_function == target;
        }));
    evidence.target_previously_entered = evidence.previous_target_count != 0U;
    if (request.has_source)
    {
        evidence.previous_source_target_edge_count = static_cast<std::size_t>(std::count_if(
            history.begin(), history.end(), [&](const auto& prior) {
                return prior.source_function &&
                       prior.source_function.value() == request.source_function &&
                       prior.target_function == target;
            }));
        evidence.source_target_edge_previously_seen =
            evidence.previous_source_target_edge_count != 0U;
    }

    if (!request.has_source)
    {
        evidence.canonical_boundary_valid = true;
    }
    else if (const auto* source_record = function_record(request.source_function);
             source_record != nullptr && target_record != nullptr)
    {
        evidence.canonical_boundary_valid = request.category ==
                                                FunctionTransitionCategory::FunctionTransfer
                                            ? source_record->canonical_entry !=
                                                  target_record->canonical_entry &&
                                                  evidence.source_pc_owned_by_source_function
                                            : evidence.source_pc_owned_by_source_function;
    }
    return evidence;
}

void ExecutionSession::append_transition(const TransitionRequest& request, GuestAddress target,
                                          ExecutionSessionResult& result)
{
    auto evidence = make_transition_evidence(request, target, result);
    auto& accounting = result.transition_accounting;
    ++accounting.total_function_entries;
    if (evidence.resource_charged) ++accounting.total_charged;
    switch (request.category)
    {
    case FunctionTransitionCategory::InitialEntry: ++accounting.initial_entries; break;
    case FunctionTransitionCategory::DirectCall: ++accounting.direct_call_entries; break;
    case FunctionTransitionCategory::IndirectCall: ++accounting.indirect_call_entries; break;
    case FunctionTransitionCategory::FunctionTransfer: ++accounting.function_transfer_entries; break;
    case FunctionTransitionCategory::FunctionResume: ++accounting.function_resume_entries; break;
    case FunctionTransitionCategory::Other: ++accounting.other_entries; break;
    }

    // The forensic history includes uncharged calls as well as charged
    // transfers. Its explicit bound is derived from max_guest_blocks, so it
    // cannot become a second unbounded execution resource.
    if (accounting.history.size() >= accounting.history_limit)
    {
        accounting.history_truncated = true;
    }
    else
    {
        if (!evidence.target_previously_entered) ++accounting.unique_function_targets;
        else ++accounting.repeated_target_count;
        accounting.maximum_target_repetition = std::max(
            accounting.maximum_target_repetition, evidence.previous_target_count + 1U);
        if (evidence.source_function)
        {
            if (!evidence.source_target_edge_previously_seen) ++accounting.unique_source_target_edges;
            else ++accounting.repeated_edge_count;
            accounting.maximum_edge_repetition = std::max(
                accounting.maximum_edge_repetition,
                evidence.previous_source_target_edge_count + 1U);
            if (evidence.source_guest_pc)
            {
                const auto prior_call_site = std::find_if(
                    accounting.history.begin(), accounting.history.end(), [&](const auto& prior) {
                        return prior.source_guest_pc &&
                               prior.source_guest_pc.value() == evidence.source_guest_pc.value();
                    });
                if (prior_call_site == accounting.history.end()) ++accounting.unique_call_sites;
            }
            if (evidence.target_equals_current_function) ++accounting.same_function_transitions;
        }

        std::size_t consecutive_targets = 1U;
        std::size_t consecutive_edges = 1U;
        for (auto prior = accounting.history.rbegin(); prior != accounting.history.rend(); ++prior)
        {
            if (prior->target_function != target) break;
            ++consecutive_targets;
            if (!evidence.source_function || !prior->source_function ||
                prior->source_function.value() != evidence.source_function.value())
                break;
            ++consecutive_edges;
        }
        accounting.maximum_consecutive_target_repetition = std::max(
            accounting.maximum_consecutive_target_repetition, consecutive_targets);
        if (evidence.source_function)
        {
            accounting.maximum_consecutive_edge_repetition = std::max(
                accounting.maximum_consecutive_edge_repetition, consecutive_edges);
        }

        if (evidence.source_function)
        {
            const auto module_position = std::lower_bound(
                accounting.module_matrix.begin(), accounting.module_matrix.end(), evidence,
                [](const auto& item, const auto& value) {
                    if (item.source_module != value.source_module)
                        return item.source_module < value.source_module;
                    return item.target_module < value.target_module;
                });
            if (module_position == accounting.module_matrix.end() ||
                module_position->source_module != evidence.source_module ||
                module_position->target_module != evidence.target_module)
            {
                accounting.module_matrix.insert(
                    module_position,
                    FunctionTransitionModuleCount{evidence.source_module, evidence.target_module, 1U});
            }
            else
            {
                ++module_position->count;
            }
        }
        const auto depth_position = std::lower_bound(
            accounting.depth_counts.begin(), accounting.depth_counts.end(),
            evidence.call_depth_after, [](const auto& item, const auto depth) {
                return item.call_depth < depth;
            });
        if (depth_position == accounting.depth_counts.end() ||
            depth_position->call_depth != evidence.call_depth_after)
        {
            accounting.depth_counts.insert(
                depth_position, FunctionTransitionDepthCount{evidence.call_depth_after, 1U});
        }
        else
        {
            ++depth_position->count;
        }

        accounting.history.push_back(std::move(evidence));
    }
    result.executed_functions.push_back(target);
    executed_function_index_.insert(target);
    result.executed_function_modules.push_back(module_name_for(target));
}

void ExecutionSession::record_terminal_transition_attempt(
    const TransitionRequest& request, GuestAddress target, ExecutionSessionResult& result) const
{
    result.transition_accounting.terminal_attempt =
        make_transition_evidence(request, target, result);
}

Result<void> ExecutionSession::enter_function(GuestAddress entry,
                                               ExecutionSessionResult& result,
                                               const TransitionRequest& request)
{
    const auto eligible = lift_for_execution(entry, result);
    if (!eligible)
    {
        return stop(result, classify_error(eligible.error()), eligible.error().message, entry);
    }
    current_.function_entry = entry;
    current_.interpreter = interpreter::InterpreterFrame{};
    append_transition(request, entry, result);
    result.maximum_call_depth = std::max(result.maximum_call_depth, current_.call_depth);
    ExecutionEvent event{0U, ExecutionEventKind::FunctionEnter, entry, entry, 0U, false,
                         current_.call_depth, runtime::ExecutionBoundaryKind::None,
                         {}, {}, {}, {}};
    event.function_module = module_name_for(entry);
    const auto recorded = record_event(result, std::move(event));
    if (!recorded) return Result<void>::failure(recorded.error());
    return Result<void>::success();
}

Result<void> ExecutionSession::stop(ExecutionSessionResult& result, ExecutionStopReason reason,
                                    std::string diagnostic,
                                    std::optional<GuestAddress> target,
                                    const runtime::ExecutionResult* boundary)
{
    result.stop_reason = reason;
    result.diagnostic = std::move(diagnostic);
    result.target = target;
    result.target_register = boundary == nullptr ? "" : boundary->boundary.target_register;
    result.target_provenance = boundary == nullptr ? "" : boundary->boundary.target_provenance;
    if (boundary != nullptr && memory_ != nullptr)
    {
        const auto decoder = aarch64::AArch64Decoder::create();
        if (decoder)
        {
            const auto instruction = aarch64::fetch_and_decode(
                *memory_, *decoder.value(), boundary->boundary.source_guest_pc);
            if (instruction)
            {
                result.diagnostic_opcode = instruction.value().opcode;
                result.diagnostic_instruction_id =
                    aarch64::instruction_id_name(instruction.value().id);
                result.diagnostic_instruction = instruction.value().disassembly;
            }
        }
    }
    result.final_cpu = cpu_;
    result.current_function = current_.function_entry;
    result.current_function_module = module_name_for(current_.function_entry);
    result.stop_module = module_name_for(target.value_or(current_.function_entry));
    if (result.stop_module.empty() && result.import_boundary)
    {
        result.stop_module = result.import_boundary->consumer_module;
    }
    result.stop_pc = boundary == nullptr ? cpu_.pc : boundary->boundary.source_guest_pc;
    if (boundary != nullptr)
    {
        if (boundary->boundary.kind == runtime::ExecutionBoundaryKind::UnsupportedInstruction)
            result.diagnostic_pc = boundary->boundary.source_guest_pc;
        else
            result.source_pc = boundary->boundary.source_guest_pc;
    }
    result.call_stack.clear();
    for (const auto& frame : suspended_frames_)
    {
        result.call_stack.push_back(CallStackFrame{frame.function_entry, frame.call_site_pc,
                                                   frame.expected_return_pc, frame.call_depth});
    }
    result.call_stack.push_back(CallStackFrame{current_.function_entry, current_.call_site_pc,
                                               current_.expected_return_pc, current_.call_depth});

    ExecutionEventKind event_kind = ExecutionEventKind::UnsupportedBoundary;
    if (reason == ExecutionStopReason::UnresolvedImport) event_kind = ExecutionEventKind::ImportBoundary;
    else if (reason == ExecutionStopReason::UnresolvedIndirectControlFlow ||
             reason == ExecutionStopReason::UnknownGuestFunction ||
             reason == ExecutionStopReason::InvalidIndirectTarget ||
             reason == ExecutionStopReason::IndirectTargetRefinementBudgetExceeded)
        event_kind = ExecutionEventKind::IndirectBoundary;
    else if (reason == ExecutionStopReason::MemoryFault ||
             reason == ExecutionStopReason::GuestMemoryResourceLimitExceeded)
        event_kind = ExecutionEventKind::MemoryFault;
    else if (reason == ExecutionStopReason::RuntimeImportUnimplemented ||
             reason == ExecutionStopReason::RuntimeImportAbiViolation ||
             reason == ExecutionStopReason::RuntimeImportMemoryFault ||
             reason == ExecutionStopReason::RuntimeImportInvariantViolation)
        event_kind = ExecutionEventKind::RuntimeImportBoundary;
    const auto import_symbol = result.import_boundary
                                   ? result.import_boundary->symbol.name
                                   : std::string{};
    ExecutionEvent stop_event{0U, event_kind, current_.function_entry, result.stop_pc,
                               target.value_or(0U), target.has_value(), current_.call_depth,
                               boundary == nullptr ? runtime::ExecutionBoundaryKind::None
                                                   : boundary->boundary.kind,
                               execution_stop_reason_name(reason), import_symbol, {}, {}};
    stop_event.function_module = result.current_function_module;
    stop_event.target_module = module_name_for(target.value_or(current_.function_entry));
    const auto recorded_stop = record_event(result, std::move(stop_event));
    if (!recorded_stop)
    {
        return Result<void>::failure(recorded_stop.error());
    }
    if (!recorded_stop.value())
    {
        return Result<void>::success();
    }
    ExecutionEvent session_stop{0U, ExecutionEventKind::SessionStop, current_.function_entry,
                                result.stop_pc, target.value_or(0U), target.has_value(),
                                current_.call_depth,
                                boundary == nullptr ? runtime::ExecutionBoundaryKind::None
                                                    : boundary->boundary.kind,
                                execution_stop_reason_name(reason), import_symbol, {}, {}};
    session_stop.function_module = result.current_function_module;
    session_stop.target_module = module_name_for(target.value_or(current_.function_entry));
    const auto recorded_session_stop = record_event(result, std::move(session_stop));
    if (!recorded_session_stop)
    {
        return Result<void>::failure(recorded_session_stop.error());
    }
    running_ = false;
    return Result<void>::success();
}

Result<void> ExecutionSession::classify_target(const runtime::ExecutionResult& boundary,
                                               ExecutionSessionResult& result, bool call)
{
    const auto& payload = boundary.boundary;
    const auto target = payload.target_guest_address;
    if (payload.has_provenance_address)
    {
        if (const auto* import = imports_.find(payload.provenance_address))
        {
            result.import_boundary = *import;
            const auto invocation = call ? runtime::ExternalInvocationKind::Call
                                         : runtime::ExternalInvocationKind::TailTransfer;
            return dispatch_runtime_import(boundary, *import, invocation, result);
        }
    }
    if ((target & 0x3U) != 0U || target == 0U)
    {
        return stop(result, ExecutionStopReason::InvalidIndirectTarget,
                    "indirect target is zero or not AArch64 aligned", target, &boundary);
    }
    analysis::ObservedIndirectTarget observed;
    observed.source_module = module_name_for(current_.function_entry);
    observed.source_function = current_.function_entry;
    observed.source_pc = payload.source_guest_pc;
    observed.control_flow = call ? analysis::IndirectControlFlowKind::Call
                                 : analysis::IndirectControlFlowKind::Branch;
    observed.target_register = payload.target_register;
    observed.target = target;
    observed.target_module = module_name_for(target);
    observed.pointer_provenance = payload.has_provenance_address
                                      ? analysis::IndirectTargetPointerProvenanceKind::GuestLoad
                                      : analysis::IndirectTargetPointerProvenanceKind::Unknown;
    if (payload.has_provenance_address) observed.guest_load_address = payload.provenance_address;
    const auto assessment = analysis::assess_indirect_target(
        observed, *memory_, function_map_, process_function_map_, process_image_);
    if (assessment)
        result.indirect_target_discovery.push_back(assessment.value());
    const auto* record = function_record(target);
    if (process_image_ != nullptr && process_image_->module_for_address(target, 4U) == nullptr)
    {
        return stop(result, ExecutionStopReason::InvalidCrossModuleTarget,
                    "target is not owned by exactly one loaded process module", target, &boundary);
    }
    if (record != nullptr &&
        (record->translation_status == analysis::TranslationStatus::Conflict ||
         record->entry_trust_status == analysis::FunctionEntryTrustStatus::Conflict))
    {
        return stop(result, ExecutionStopReason::FunctionOwnershipConflict,
                    "target belongs to a precise function ownership conflict", target, &boundary);
    }
    const auto executable = memory_->is_executable(target, 4U);
    if (!executable || !executable.value())
    {
        return stop(result, ExecutionStopReason::InvalidIndirectTarget,
                    "indirect target is not mapped executable guest code", target, &boundary);
    }
    if (record == nullptr || record->entry_trust_status != analysis::FunctionEntryTrustStatus::Trusted)
    {
        return stop(result, ExecutionStopReason::UnknownGuestFunction,
                    "aligned executable target is not an exact trusted function entry", target, &boundary);
    }
    const auto eligible = lift_for_execution(target, result);
    if (!eligible)
    {
        return stop(result, classify_error(eligible.error()), eligible.error().message, target, &boundary);
    }
    const auto dispatched = call ? dispatch_call(boundary, result)
                                 : dispatch_transfer(boundary, result);
    if (dispatched && !result.indirect_target_discovery.empty())
    {
        auto& assessment = result.indirect_target_discovery.back();
        assessment.guest_code_entered = true;
        if (assessment.validation.cfg)
        {
            const auto block = assessment.validation.cfg->blocks.find(target);
            if (block != assessment.validation.cfg->blocks.end() &&
                !block->second.instructions.empty())
            {
                const auto& first = block->second.instructions.front();
                assessment.first_guest_pc = first.address;
                assessment.first_guest_opcode = first.opcode;
                const auto next = checked_add_u64(first.address, 4U);
                if (next) assessment.next_guest_pc = next.value();
            }
        }
    }
    return dispatched;
}

Result<void> ExecutionSession::dispatch_call(const runtime::ExecutionResult& boundary,
                                              ExecutionSessionResult& result)
{
    if (boundary.boundary.continuation_block == ir::invalid_block ||
        boundary.boundary.continuation_guest_pc == 0U)
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidControlFlow, "guest call boundary has no explicit continuation"));
    }
    if (suspended_frames_.size() + 1U > options_.budgets.max_call_depth)
    {
        return stop(result, ExecutionStopReason::CallDepthExceeded,
                    "guest call depth limit exhausted", boundary.boundary.target_guest_address,
                    &boundary);
    }
    const auto event_kind = boundary.boundary.kind == runtime::ExecutionBoundaryKind::IndirectCall
                                ? ExecutionEventKind::IndirectCall
                                : ExecutionEventKind::DirectCall;
    ExecutionEvent call_event{0U, event_kind, current_.function_entry,
                              boundary.boundary.source_guest_pc,
                              boundary.boundary.target_guest_address, true,
                              current_.call_depth, boundary.boundary.kind, {}, {}, {}, {}};
    call_event.function_module = module_name_for(current_.function_entry);
    call_event.target_module = module_name_for(boundary.boundary.target_guest_address);
    const auto recorded = record_event(result, std::move(call_event));
    if (!recorded)
    {
        return Result<void>::failure(recorded.error());
    }
    if (!recorded.value())
    {
        return Result<void>::success();
    }
    TransitionRequest request;
    request.category = boundary.boundary.kind == runtime::ExecutionBoundaryKind::IndirectCall
                           ? FunctionTransitionCategory::IndirectCall
                           : FunctionTransitionCategory::DirectCall;
    request.has_source = true;
    request.source_function = current_.function_entry;
    request.has_source_pc = true;
    request.source_guest_pc = boundary.boundary.source_guest_pc;
    request.target_function = boundary.boundary.target_guest_address;
    request.call_depth_before = current_.call_depth;
    request.call_depth_after = current_.call_depth + 1U;
    request.boundary = boundary.boundary.kind;
    request.target_register = boundary.boundary.target_register;
    request.target_provenance = boundary.boundary.target_provenance;
    request.link_register_action =
        FunctionTransitionLinkRegisterAction::WrittenArchitecturalReturnPc;
    request.expected_return_pc = boundary.boundary.continuation_guest_pc;
    request.call_frame_pushed = true;
    current_.interpreter.resume_at(boundary.boundary.continuation_block);
    suspended_frames_.push_back(std::move(current_));
    current_ = SessionFrame{};
    current_.call_site_pc = boundary.boundary.source_guest_pc;
    current_.expected_return_pc = boundary.boundary.continuation_guest_pc;
    current_.call_depth = suspended_frames_.size();
    current_.function_entry = boundary.boundary.target_guest_address;
    return enter_function(current_.function_entry, result, request);
}

Result<void> ExecutionSession::dispatch_transfer(const runtime::ExecutionResult& boundary,
                                                 ExecutionSessionResult& result)
{
    ExecutionEvent transfer_event{0U, ExecutionEventKind::FunctionTransfer,
                                  current_.function_entry,
                                  boundary.boundary.source_guest_pc,
                                  boundary.boundary.target_guest_address, true,
                                  current_.call_depth, boundary.boundary.kind, {}, {}, {}, {}};
    transfer_event.function_module = module_name_for(current_.function_entry);
    transfer_event.target_module = module_name_for(boundary.boundary.target_guest_address);
    const auto recorded_transfer = record_event(result, std::move(transfer_event));
    if (!recorded_transfer)
    {
        return Result<void>::failure(recorded_transfer.error());
    }
    if (!recorded_transfer.value())
    {
        return Result<void>::success();
    }
    TransitionRequest request;
    request.category = FunctionTransitionCategory::FunctionTransfer;
    request.has_source = true;
    request.source_function = current_.function_entry;
    request.has_source_pc = true;
    request.source_guest_pc = boundary.boundary.source_guest_pc;
    request.target_function = boundary.boundary.target_guest_address;
    request.call_depth_before = current_.call_depth;
    request.call_depth_after = current_.call_depth;
    request.boundary = boundary.boundary.kind;
    request.target_register = boundary.boundary.target_register;
    request.target_provenance = boundary.boundary.target_provenance;
    request.link_register_action = FunctionTransitionLinkRegisterAction::Preserved;
    request.expected_return_pc = current_.expected_return_pc;
    if (result.transition_accounting.total_charged >=
        options_.budgets.max_function_transitions)
    {
        record_terminal_transition_attempt(request, boundary.boundary.target_guest_address, result);
        return stop(result, ExecutionStopReason::FunctionTransitionLimitExceeded,
                    "function transition limit exhausted", boundary.boundary.target_guest_address,
                    &boundary);
    }
    current_.function_entry = boundary.boundary.target_guest_address;
    current_.interpreter = interpreter::InterpreterFrame{};
    ++result.function_transfers;
    append_transition(request, current_.function_entry, result);
    result.maximum_call_depth = std::max(result.maximum_call_depth, current_.call_depth);
    ExecutionEvent event{0U, ExecutionEventKind::FunctionEnter, current_.function_entry,
                         current_.function_entry, 0U, false, current_.call_depth,
                         runtime::ExecutionBoundaryKind::None, {}, {}, {}, {}};
    event.function_module = module_name_for(current_.function_entry);
    const auto recorded = record_event(result, std::move(event));
    if (!recorded) return Result<void>::failure(recorded.error());
    return Result<void>::success();
}

Result<void> ExecutionSession::dispatch_runtime_import(
    const runtime::ExecutionResult& boundary, const ImportBoundary& import,
    runtime::ExternalInvocationKind invocation, ExecutionSessionResult& result)
{
    ++result.runtime.imports_encountered;
    const analysis::ProcessBinding* process_binding = nullptr;
    bool runtime_fallback_eligible = true;
    std::string guest_provider_resolution = "not_available_without_process_image";
    if (process_image_ != nullptr)
    {
        const auto binding_it = std::find_if(
            process_image_->bindings().begin(), process_image_->bindings().end(),
            [&](const auto& binding) {
                return binding.consumer_module == import.consumer_module &&
                       binding.relocation_index == import.relocation_index &&
                       binding.relocation.target_address == import.relocation_target;
            });
        if (binding_it != process_image_->bindings().end()) process_binding = &*binding_it;
        if (process_binding == nullptr)
        {
            runtime_fallback_eligible = false;
            guest_provider_resolution = "binding_missing";
        }
        else
        {
            guest_provider_resolution = std::string(
                analysis::provider_resolution_status_name(process_binding->provider.status));
            runtime_fallback_eligible = process_binding->provider.status ==
                                        analysis::ProviderResolutionStatus::NotFoundInSuppliedModules;
        }
    }
    const auto* registered_descriptor = runtime_imports_ == nullptr
                                            ? nullptr
                                            : runtime_imports_->find(import.symbol.name);
    const auto* descriptor = runtime_fallback_eligible ? registered_descriptor : nullptr;

    RuntimeImportObservation observation;
    observation.provenance = import;
    observation.invocation = invocation;
    observation.source_guest_pc = boundary.boundary.source_guest_pc;
    observation.guest_provider_resolution = std::move(guest_provider_resolution);
    observation.runtime_fallback_eligible = runtime_fallback_eligible;
    if (registered_descriptor != nullptr)
    {
        observation.descriptor = *registered_descriptor;
        if (descriptor != nullptr) ++result.runtime.imports_resolved;
    }
    else
    {
        observation.descriptor.symbol_name = import.symbol.name;
        observation.descriptor.support = runtime::RuntimeSupportStatus::Unknown;
        observation.descriptor.subsystem = runtime::RuntimeSubsystem::Unknown;
        observation.descriptor.evidence.confidence = "not_registered";
    }

    const format::ModuleMetadata* import_metadata = module_metadata_;
    const format::DynamicSymbolTable* import_symbols = symbols_;
    if (process_image_ != nullptr && !import.consumer_module.empty())
    {
        if (const auto* module = process_image_->module(import.consumer_module))
        {
            import_metadata = &module->metadata;
            import_symbols = module->symbols ? &module->symbols.value() : nullptr;
        }
    }
    const auto* dynamic = import_metadata != nullptr && import_metadata->dynamic
                              ? &import_metadata->dynamic.value()
                              : nullptr;
    classify_import_trampoline(function_record(current_.function_entry), boundary, import, dynamic,
                               observation);
    if (dynamic != nullptr && dynamic->pltgot)
    {
        observation.dynamic_pltgot = dynamic->pltgot->address;
        if (import.relocation_target >= dynamic->pltgot->address)
        {
            observation.dynamic_pltgot_slot_delta =
                import.relocation_target - dynamic->pltgot->address;
        }
    }

    const runtime::AbiSignature empty_signature;
    const auto& signature = descriptor == nullptr ? empty_signature : descriptor->signature;
    runtime::AArch64GuestCall abi(cpu_, *memory_, signature);
    observation.abi = abi.snapshot();
    const auto observed_count = descriptor == nullptr ? 0U : signature.observed_argument_count;
    observation.arguments.reserve(observed_count);
    for (std::size_t index = 0U; index < observed_count; ++index)
    {
        observation.arguments.push_back(
            observe_argument(abi, index, *memory_, dynamic, import_symbols));
    }

    const auto runtime_event = [&](ExecutionEventKind kind, std::string code) {
        return record_event(result, ExecutionEvent{0U, kind, current_.function_entry,
                                                     boundary.boundary.source_guest_pc,
                                                     boundary.boundary.target_guest_address, true,
                                                     current_.call_depth, boundary.boundary.kind,
                                                     std::move(code), import.symbol.name, {}, {}});
    };

    if (!runtime_fallback_eligible)
    {
        observation.abi_validation = "not_attempted";
        observation.outcome = "guest_provider_resolution_blocked";
        observation.outcome_diagnostic =
            "runtime fallback is forbidden until complete guest-provider resolution reports "
            "provider_not_found_complete (resolution=" + observation.guest_provider_resolution + ")";
        result.runtime.imports.push_back(std::move(observation));
        return stop(result, ExecutionStopReason::UnresolvedImport,
                    "guest provider resolution did not authorize runtime fallback for '" +
                        import.symbol.name + "'",
                    boundary.boundary.target_guest_address, &boundary);
    }

    if (descriptor == nullptr)
    {
        observation.abi_validation = "not_attempted";
        observation.outcome = "unresolved";
        observation.outcome_diagnostic =
            "no runtime descriptor is registered for the provenance-backed import";
        result.runtime.imports.push_back(std::move(observation));
        return stop(result, ExecutionStopReason::UnresolvedImport,
                    "indirect target originates at unresolved relocation " +
                        hex_address(import.relocation_target),
                    boundary.boundary.target_guest_address, &boundary);
    }

    const auto resolved_event = runtime_event(
        ExecutionEventKind::RuntimeImportResolved,
        std::string(runtime::runtime_support_status_name(descriptor->support)));
    if (!resolved_event)
    {
        return Result<void>::failure(resolved_event.error());
    }
    if (!resolved_event.value())
    {
        observation.abi_validation = "not_attempted";
        observation.outcome = "event_limit";
        result.runtime.imports.push_back(std::move(observation));
        return Result<void>::success();
    }
    const auto enter_event = runtime_event(
        ExecutionEventKind::RuntimeImportEnter,
        std::string(runtime::external_invocation_kind_name(invocation)));
    if (!enter_event)
    {
        return Result<void>::failure(enter_event.error());
    }
    if (!enter_event.value())
    {
        observation.abi_validation = "not_attempted";
        observation.outcome = "event_limit";
        result.runtime.imports.push_back(std::move(observation));
        return Result<void>::success();
    }
    const auto argument_event = runtime_event(
        ExecutionEventKind::RuntimeImportArgumentSummary,
        "observed_argument_slots=" + std::to_string(observed_count));
    if (!argument_event)
    {
        return Result<void>::failure(argument_event.error());
    }
    if (!argument_event.value())
    {
        observation.abi_validation = "not_attempted";
        observation.outcome = "event_limit";
        result.runtime.imports.push_back(std::move(observation));
        return Result<void>::success();
    }

    runtime::RuntimeImportContext context{abi, *memory_, runtime_state_, *descriptor,
                                          runtime::ImportProvenance{
                                              import.relocation_target, import.relocation_index,
                                              import.relocation, import.symbol},
                                          invocation, import_metadata};
    const auto invoked = runtime_imports_->invoke(context);
    runtime::RuntimeImportOutcome outcome = invoked
                                                ? std::move(invoked).value()
                                                : runtime::RuntimeImportOutcome::invariant_violation(
                                                      invoked.error());
    if (outcome.abi_validated)
    {
        observation.abi_validation = outcome.abi_signature_known
                                          ? "valid"
                                          : "observed_slots_valid_signature_unknown";
    }
    else
    {
        observation.abi_validation = "invalid";
    }
    observation.abi_diagnostic = outcome.error ?
                                     std::string(error_code_name(outcome.error->code)) + ": " +
                                         outcome.error->message
                                                     : std::string{};
    observation.outcome = std::string(runtime::runtime_import_outcome_name(outcome.kind));
    observation.outcome_diagnostic = outcome.diagnostic;
    result.runtime.dso_modules_registered = runtime_state_.dso_modules_registered();
    result.runtime.imports.push_back(std::move(observation));

    if (outcome.kind != runtime::RuntimeImportOutcomeKind::Handled)
    {
        switch (outcome.kind)
        {
        case runtime::RuntimeImportOutcomeKind::Unimplemented:
            ++result.runtime.imports_unimplemented;
            return stop(result, ExecutionStopReason::RuntimeImportUnimplemented,
                        outcome.diagnostic, boundary.boundary.target_guest_address, &boundary);
        case runtime::RuntimeImportOutcomeKind::AbiViolation:
            ++result.runtime.imports_abi_violations;
            return stop(result, ExecutionStopReason::RuntimeImportAbiViolation,
                        outcome.diagnostic, boundary.boundary.target_guest_address, &boundary);
        case runtime::RuntimeImportOutcomeKind::MemoryFault:
            ++result.runtime.imports_memory_faults;
            return stop(result, ExecutionStopReason::RuntimeImportMemoryFault,
                        outcome.diagnostic, boundary.boundary.target_guest_address, &boundary);
        case runtime::RuntimeImportOutcomeKind::InvariantViolation:
            ++result.runtime.imports_invariant_violations;
            return stop(result, ExecutionStopReason::RuntimeImportInvariantViolation,
                        outcome.diagnostic, boundary.boundary.target_guest_address, &boundary);
        case runtime::RuntimeImportOutcomeKind::Handled:
            break;
        }
    }

    const auto return_event = runtime_event(
        ExecutionEventKind::RuntimeImportReturn,
        std::string(runtime::runtime_import_outcome_name(outcome.kind)));
    if (!return_event)
    {
        return Result<void>::failure(return_event.error());
    }
    if (!return_event.value())
    {
        return Result<void>::success();
    }
    ++result.runtime.imports_handled;
    ++result.runtime.import_returns;

    if (invocation == runtime::ExternalInvocationKind::Call)
    {
        if (boundary.boundary.continuation_block == ir::invalid_block ||
            boundary.boundary.continuation_guest_pc == 0U ||
            (boundary.boundary.continuation_guest_pc & 0x3U) != 0U)
        {
            ++result.runtime.imports_invariant_violations;
            return stop(result, ExecutionStopReason::RuntimeImportInvariantViolation,
                        "handled external call has no valid guest continuation",
                        boundary.boundary.target_guest_address, &boundary);
        }
        current_.interpreter.resume_at(boundary.boundary.continuation_block);
        cpu_.pc = boundary.boundary.continuation_guest_pc;
        return Result<void>::success();
    }

    if (current_.expected_return_pc == 0U || (current_.expected_return_pc & 0x3U) != 0U)
    {
        ++result.runtime.imports_invariant_violations;
        return stop(result, ExecutionStopReason::RuntimeImportInvariantViolation,
                    "handled tail external transfer has no inherited guest return contract",
                    boundary.boundary.target_guest_address, &boundary);
    }
    if (suspended_frames_.empty())
    {
        return stop(result, ExecutionStopReason::EntryReturned,
                    "tail runtime import returned through the inherited entry contract",
                    std::nullopt, &boundary);
    }

    const auto expected_return = current_.expected_return_pc;
    current_ = std::move(suspended_frames_.back());
    suspended_frames_.pop_back();
    cpu_.pc = expected_return;
    const auto recorded = record_event(
        result, ExecutionEvent{0U, ExecutionEventKind::FunctionResume,
                               current_.function_entry, expected_return, 0U,
                               false, current_.call_depth,
                               runtime::ExecutionBoundaryKind::Return,
                               "runtime_tail_transfer_resume",
                               import.symbol.name, {}, {}});
    if (!recorded) return Result<void>::failure(recorded.error());
    return Result<void>::success();
}

Result<ExecutionSessionResult> ExecutionSession::run(const EntrySelection& entry)
{
    if (running_)
    {
        return Result<ExecutionSessionResult>::failure(
            make_error(ErrorCode::InvalidArgument, "execution session is already running"));
    }
    if (memory_ == nullptr || function_map_ == nullptr || options_.budgets.slice_ir_operations == 0U ||
        (options_.budgets.max_ir_operations && options_.budgets.max_ir_operations.value() == 0U) ||
        options_.budgets.max_function_transitions == 0U || options_.budgets.max_call_depth == 0U ||
        (options_.budgets.max_events && options_.budgets.max_events.value() == 0U) ||
        options_.budgets.event_history_limit == 0U || options_.budgets.max_guest_blocks == 0U)
    {
        return Result<ExecutionSessionResult>::failure(
            make_error(ErrorCode::InvalidArgument, "execution budgets and session inputs must be non-zero"));
    }
    if (entry.address == 0U || (entry.address & 0x3U) != 0U)
    {
        return Result<ExecutionSessionResult>::failure(
            make_error(ErrorCode::InvalidGuestAddress, "selected execution entry is invalid"));
    }
    ProfileTimer timer("execution.run");

    // A second run on the same session replaces the previous logical
    // generation. The first run's stack was kept alive through its caller's
    // assessment window; releasing it here is the replacement boundary.
    const auto released_previous_stack = release_stack();
    if (!released_previous_stack)
    {
        return Result<ExecutionSessionResult>::failure(released_previous_stack.error());
    }

    running_ = true;
    suspended_frames_.clear();
    current_ = SessionFrame{};
    lift_cache_.clear();
    profile_totals_ = ProfileTotals{};
    executed_function_index_.clear();
    instruction_evidence_index_.clear();
    runtime_state_.reset();
    ExecutionSessionResult result;
    result.identity = function_map_->identity();
    result.entry = entry;
    result.options = options_;
    result.event_resource.retention_limit = options_.budgets.event_history_limit;
    result.event_resource.execution_limit = options_.budgets.max_events;
    result.event_resource.execution_limit_provenance =
        options_.budgets.max_events
            ? (options_.budgets.event_execution_limit_provenance ==
                       EventExecutionLimitProvenance::NotConfigured
                   ? EventExecutionLimitProvenance::ExplicitLibraryApi
                   : options_.budgets.event_execution_limit_provenance)
            : EventExecutionLimitProvenance::NotConfigured;
    result.transition_accounting.configured_limit =
        options_.budgets.max_function_transitions;
    result.transition_accounting.history_limit =
        options_.budgets.max_guest_blocks == std::numeric_limits<std::size_t>::max()
            ? options_.budgets.max_guest_blocks
            : options_.budgets.max_guest_blocks + 1U;
    result.relocations = load_summary_;
    result.guest_memory = memory_->accounting();
    if (process_image_ != nullptr)
    {
        result.process = process_image_->summary();
        result.relocations.guest_bindings_attempted = result.process->bindings.size();
        for (const auto& binding : result.process->bindings)
        {
            if (binding.provider.status == analysis::ProviderResolutionStatus::ResolvedGuestModule)
            {
                ++result.relocations.guest_bindings_resolved;
                if (binding.provider_module && binding.provider_module.value() !=
                                                     binding.consumer_module)
                {
                    ++result.relocations.cross_module_relocations;
                }
            }
            else if (binding.provider.status == analysis::ProviderResolutionStatus::AmbiguousGuestProvider)
            {
                ++result.relocations.guest_bindings_ambiguous;
            }
            else
            {
                ++result.relocations.guest_bindings_unresolved;
            }
        }
    }
    if (module_metadata_ != nullptr)
    {
        result.module_metadata = *module_metadata_;
    }
    if (process_function_map_ != nullptr)
    {
        for (const auto& map : process_function_map_->maps())
        {
            result.analysis.push_back(map.accounting());
            result.analyzed_functions += map.functions().size();
            result.precise_conflicts += map.conflicts().size();
        }
    }
    else
    {
        result.analysis.push_back(function_map_->accounting());
        result.analyzed_functions = function_map_->functions().size();
        result.precise_conflicts = function_map_->conflicts().size();
    }
    std::set<GuestAddress> conflicting_functions;
    std::vector<const analysis::FunctionBoundaryConflict*> conflicts;
    if (process_function_map_ != nullptr)
    {
        for (const auto& map : process_function_map_->maps())
            for (const auto& conflict : map.conflicts()) conflicts.push_back(&conflict);
    }
    else
    {
        for (const auto& conflict : function_map_->conflicts()) conflicts.push_back(&conflict);
    }
    for (const auto* conflict : conflicts)
    {
        conflicting_functions.insert(conflict->first_function);
        conflicting_functions.insert(conflict->second_function);
    }
    result.conflicting_functions = conflicting_functions.size();
    const auto add_owned_bytes = [&](const auto& map) -> Result<void> {
        for (const auto& function : map.functions())
        {
            const auto owned = analysis::precise_owned_byte_count(function.owned_code_ranges);
            if (!owned || owned.value() > std::numeric_limits<memory::GuestSize>::max() -
                                  result.precise_owned_bytes)
            {
                return Result<void>::failure(make_error(
                    ErrorCode::AnalysisBudgetExceeded, "precise ownership byte count overflows"));
            }
            result.precise_owned_bytes += owned.value();
        }
        return Result<void>::success();
    };
    if (process_function_map_ != nullptr)
    {
        for (const auto& map : process_function_map_->maps())
        {
            const auto owned = add_owned_bytes(map);
            if (!owned)
            {
                running_ = false;
                return Result<ExecutionSessionResult>::failure(owned.error());
            }
        }
    }
    else
    {
        const auto owned = add_owned_bytes(*function_map_);
        if (!owned)
        {
            running_ = false;
            return Result<ExecutionSessionResult>::failure(owned.error());
        }
    }
    prepare_observation_targets();
    result.observation_targets = observation_targets_;
    result.instruction_observation_targets = instruction_observation_targets_;
    const auto session_start = record_event(
        result, ExecutionEvent{0U, ExecutionEventKind::SessionStart, 0U, 0U, 0U,
                               false, 0U,
                               runtime::ExecutionBoundaryKind::None, {}, {}, {}, {}});
    if (!session_start)
    {
        running_ = false;
        return Result<ExecutionSessionResult>::failure(session_start.error());
    }
    const auto entry_selected = session_start.value()
                                    ? record_event(
                                          result, ExecutionEvent{0U, ExecutionEventKind::EntrySelected, 0U,
                                                                 entry.address, entry.address, true, 0U,
                                                                 runtime::ExecutionBoundaryKind::None,
                                                                 entry_selection_kind_name(entry.kind), {}, {}, {}})
                                    : Result<bool>::success(false);
    if (!entry_selected)
    {
        running_ = false;
        return Result<ExecutionSessionResult>::failure(entry_selected.error());
    }
    if (!session_start.value() || !entry_selected.value())
    {
        result.final_cpu = cpu_;
        return Result<ExecutionSessionResult>::success(std::move(result));
    }
    const auto stack = map_stack(result);
    if (!stack)
    {
        const auto stopped = stop(result, classify_error(stack.error()), stack.error().message);
        if (!stopped)
        {
            running_ = false;
            return Result<ExecutionSessionResult>::failure(stopped.error());
        }
        result.guest_memory = memory_->accounting();
        return Result<ExecutionSessionResult>::success(std::move(result));
    }
    if (!running_)
    {
        result.final_cpu = cpu_;
        result.guest_memory = memory_->accounting();
        return Result<ExecutionSessionResult>::success(std::move(result));
    }
    const auto sentinel = checked_add_u64(result.stack_end, 0x1000U);
    if (!sentinel)
    {
        running_ = false;
        return Result<ExecutionSessionResult>::failure(sentinel.error());
    }
    result.synthetic_lr_sentinel = sentinel.value();
    cpu_ = runtime::CpuState{};
    cpu_.sp = result.initial_sp;
    cpu_.pc = entry.address;
    cpu_.x[30U] = result.synthetic_lr_sentinel;
    runtime_ = runtime::RuntimeContext{memory_};
    shared_runtime_ = std::make_unique<runtime::SharedRuntimeState>(*memory_);
    runtime_.shared = shared_runtime_.get();
    runtime_.cpu = &cpu_;
    current_.function_entry = entry.address;
    current_.expected_return_pc = result.synthetic_lr_sentinel;
    TransitionRequest initial_request;
    initial_request.category = FunctionTransitionCategory::InitialEntry;
    initial_request.target_function = entry.address;
    initial_request.call_depth_after = current_.call_depth;
    initial_request.link_register_action =
        FunctionTransitionLinkRegisterAction::InitializedSyntheticReturn;
    const auto entered = enter_function(entry.address, result, initial_request);
    if (!entered)
    {
        running_ = false;
        return Result<ExecutionSessionResult>::failure(entered.error());
    }
    bool former_blocker_seen = false;
    std::optional<memory::GuestAddress> former_blocker_pc;
    bool smulh_seen = false;
    std::optional<memory::GuestAddress> smulh_pc;
    bool resumable_continuation = false;
    const auto add_counter = [](std::size_t& total, std::size_t delta) -> Result<void> {
        const auto updated = checked_add(total, delta);
        if (!updated)
        {
            return Result<void>::failure(make_error(
                ErrorCode::ArithmeticOverflow, "execution accounting counter overflowed"));
        }
        total = updated.value();
        return Result<void>::success();
    };
    while (running_)
    {
        const auto function_result = lift_for_execution(current_.function_entry, result);
        if (!function_result)
        {
            const auto stopped = stop(result, classify_error(function_result.error()),
                                       function_result.error().message, current_.function_entry);
            if (!stopped)
            {
                running_ = false;
                return Result<ExecutionSessionResult>::failure(stopped.error());
            }
            break;
        }
        const auto* function = function_result.value();
        runtime::ExecutionOptions interpreter_options;
        interpreter_options.max_ir_operations = options_.budgets.max_ir_operations
                                                    ? std::optional<std::size_t>(
                                                          options_.budgets.max_ir_operations.value() -
                                                          result.ir_operations)
                                                    : std::nullopt;
        interpreter_options.slice_ir_operations = options_.budgets.slice_ir_operations;
        interpreter_options.observed_guest_pcs =
            std::span<const memory::GuestAddress>(instruction_observation_targets_);
        interpreter_options.max_observed_guest_pcs = 32U;
        const auto verification_elapsed_before =
            current_.interpreter.profile_ir_verification_elapsed_us;
        const auto verification_calls_before = current_.interpreter.profile_ir_verification_calls;
        ProfileTimer interpreter_timer("execute_until_boundary", &profile_totals_.boundary_elapsed_us,
                                      &profile_totals_.boundary_calls);
        const auto step = interpreter::execute_until_boundary(
            *function, cpu_, runtime_, current_.interpreter, interpreter_options);
        if (profiling_enabled())
        {
            result.performance.ir_verification_elapsed_us +=
                current_.interpreter.profile_ir_verification_elapsed_us - verification_elapsed_before;
            result.performance.ir_verification_calls +=
                current_.interpreter.profile_ir_verification_calls - verification_calls_before;
        }
        if (!step)
        {
            const auto stopped = stop(result, classify_error(step.error()), step.error().message);
            if (!stopped)
            {
                running_ = false;
                return Result<ExecutionSessionResult>::failure(stopped.error());
            }
            break;
        }
        if (const auto added = add_counter(result.ir_operations, step.value().executed_operations);
            !added)
        {
            running_ = false;
            return Result<ExecutionSessionResult>::failure(added.error());
        }
        if (const auto added = add_counter(result.guest_blocks, step.value().executed_blocks);
            !added)
        {
            running_ = false;
            return Result<ExecutionSessionResult>::failure(added.error());
        }
        if (const auto added = add_counter(result.guest_instruction_count,
                                           step.value().executed_guest_instructions);
            !added)
        {
            running_ = false;
            return Result<ExecutionSessionResult>::failure(added.error());
        }
        if (const auto added = add_counter(result.execution_slices, 1U); !added)
        {
            running_ = false;
            return Result<ExecutionSessionResult>::failure(added.error());
        }
        if (resumable_continuation)
        {
            if (const auto added = add_counter(result.resumes, 1U); !added)
            {
                running_ = false;
                return Result<ExecutionSessionResult>::failure(added.error());
            }
            if (current_.interpreter.ir_operation_index != 0U)
            {
                if (const auto added = add_counter(result.mid_block_resumes, 1U); !added)
                {
                    running_ = false;
                    return Result<ExecutionSessionResult>::failure(added.error());
                }
            }
        }
        result.maximum_ir_operations_in_slice = std::max(
            result.maximum_ir_operations_in_slice, step.value().executed_operations);
        if (step.value().status == runtime::ExecutionStatus::Yielded)
        {
            if (const auto added = add_counter(result.resumable_yields, 1U); !added)
            {
                running_ = false;
                return Result<ExecutionSessionResult>::failure(added.error());
            }
            resumable_continuation = true;
        }
        else
        {
            resumable_continuation = false;
        }
        for (const auto& observed_execution : step.value().observed_instruction_executions)
        {
            if (instruction_evidence_index_.find(observed_execution.guest_pc) !=
                instruction_evidence_index_.end())
                continue;
            if (const auto observed = describe_observed_instruction(observed_execution,
                                                                    current_.call_depth))
            {
                result.instruction_evidence.push_back(observed.value());
                instruction_evidence_index_.insert(observed->guest_pc);
                if (observed->instruction_id == "umulh" || observed->instruction_id == "smulh")
                    result.executed_guest_instructions.push_back(observed.value());
                if (observed->instruction_id == "smulh" && !result.smulh_frontier.reached &&
                    observed->pre_registers.size() >= 2U && observed->post_registers.size() >= 1U &&
                    observed->expected_high64 && observed->actual_high64)
                {
                    const auto& lhs = observed->pre_registers[0];
                    const auto& rhs = observed->pre_registers[1];
                    result.smulh_frontier.reached = true;
                    result.smulh_frontier.module = observed->module;
                    result.smulh_frontier.pc = observed->guest_pc;
                    result.smulh_frontier.opcode = observed->opcode;
                    result.smulh_frontier.instruction = observed->mnemonic;
                    result.smulh_frontier.destination_register = observed->destination_register;
                    result.smulh_frontier.lhs_register = lhs.name;
                    result.smulh_frontier.rhs_register = rhs.name;
                    result.smulh_frontier.lhs_raw = lhs.value;
                    result.smulh_frontier.lhs_signed = std::bit_cast<std::int64_t>(lhs.value);
                    result.smulh_frontier.rhs_raw = rhs.value;
                    result.smulh_frontier.rhs_signed = std::bit_cast<std::int64_t>(rhs.value);
                    result.smulh_frontier.expected_high64 = observed->expected_high64.value();
                    result.smulh_frontier.actual_high64 = observed->actual_high64.value();
                    result.smulh_frontier.match = observed->result_matches.value_or(false);
                    result.smulh_frontier.next_guest_pc = observed->next_guest_pc;
                    smulh_pc = observed->guest_pc;
                }
                if (observed->module == "sdk" && observed->instruction_id == "movz" &&
                    observed->destination_register == "w0")
                    former_blocker_pc = observed->guest_pc;
            }
        }
        for (const auto guest_pc : step.value().executed_guest_pcs)
        {
            if (former_blocker_pc && guest_pc == former_blocker_pc.value())
            {
                former_blocker_seen = true;
                continue;
            }
            if (former_blocker_seen)
                ++result.instructions_after_former_blocker;
            if (smulh_seen)
                ++result.instructions_after_smulh;
            if (smulh_pc && guest_pc == smulh_pc.value())
            {
                smulh_seen = true;
                continue;
            }
        }
        if (result.guest_blocks > options_.budgets.max_guest_blocks)
        {
            const auto stopped = stop(result, ExecutionStopReason::GuestBlockLimitExceeded,
                                      "guest block execution limit exhausted");
            if (!stopped)
            {
                running_ = false;
                return Result<ExecutionSessionResult>::failure(stopped.error());
            }
            break;
        }
        const auto& boundary = step.value().boundary;
        if (step.value().status == runtime::ExecutionStatus::LimitExceeded)
        {
            result.terminal_ir_block = step.value().resume_block;
            result.terminal_ir_operation_index = step.value().resume_operation_index;
            const auto stopped = stop(result, ExecutionStopReason::IrOperationLimitExceeded,
                                      "session IR operation limit exhausted", std::nullopt,
                                      &step.value());
            if (!stopped)
            {
                running_ = false;
                return Result<ExecutionSessionResult>::failure(stopped.error());
            }
            break;
        }
        if (step.value().status == runtime::ExecutionStatus::Yielded)
        {
            continue;
        }
        switch (boundary.kind)
        {
        case runtime::ExecutionBoundaryKind::Return:
        {
            ++result.returns;
            const auto return_event = record_event(
                result, ExecutionEvent{0U, ExecutionEventKind::Return,
                                       current_.function_entry,
                                       boundary.source_guest_pc,
                                       boundary.target_guest_address,
                                       boundary.target_known, current_.call_depth,
                                       boundary.kind, {}, {}, {}, {}});
            if (!return_event)
            {
                running_ = false;
                return Result<ExecutionSessionResult>::failure(return_event.error());
            }
            if (!return_event.value())
                break;
            if (suspended_frames_.empty())
            {
                const auto stopped = stop(result, ExecutionStopReason::EntryReturned,
                                          "controlled entry component returned", std::nullopt,
                                          &step.value());
                if (!stopped)
                {
                    running_ = false;
                    return Result<ExecutionSessionResult>::failure(stopped.error());
                }
                break;
            }
            if (boundary.target_guest_address != current_.expected_return_pc)
            {
                const auto stopped = stop(
                    result, ExecutionStopReason::ReturnTargetMismatch,
                    "guest return target does not match the caller continuation",
                    boundary.target_guest_address, &step.value());
                if (!stopped)
                {
                    running_ = false;
                    return Result<ExecutionSessionResult>::failure(stopped.error());
                }
                break;
            }
            current_ = std::move(suspended_frames_.back());
            suspended_frames_.pop_back();
            cpu_.pc = current_.expected_return_pc;
            const auto resume_event = record_event(
                result, ExecutionEvent{0U, ExecutionEventKind::FunctionResume,
                                       current_.function_entry,
                                       current_.expected_return_pc, 0U, false,
                                       current_.call_depth,
                                       runtime::ExecutionBoundaryKind::Return, {}, {}, {}, {}});
            if (!resume_event)
            {
                running_ = false;
                return Result<ExecutionSessionResult>::failure(resume_event.error());
            }
            if (!resume_event.value())
                break;
            break;
        }
        case runtime::ExecutionBoundaryKind::DirectCall:
        case runtime::ExecutionBoundaryKind::IndirectCall:
        {
            if (boundary.kind == runtime::ExecutionBoundaryKind::DirectCall) ++result.direct_calls;
            else ++result.indirect_calls;
            const auto dispatched = classify_target(step.value(), result, true);
            if (!dispatched)
            {
                running_ = false;
                return Result<ExecutionSessionResult>::failure(dispatched.error());
            }
            break;
        }
        case runtime::ExecutionBoundaryKind::FunctionTransfer:
        case runtime::ExecutionBoundaryKind::IndirectBranch:
        {
            const auto dispatched = classify_target(step.value(), result, false);
            if (!dispatched)
            {
                running_ = false;
                return Result<ExecutionSessionResult>::failure(dispatched.error());
            }
            break;
        }
        case runtime::ExecutionBoundaryKind::Trap:
        {
            const auto stopped = stop(result, ExecutionStopReason::GuestTrap,
                                      boundary.target_provenance, std::nullopt, &step.value());
            if (!stopped)
            {
                running_ = false;
                return Result<ExecutionSessionResult>::failure(stopped.error());
            }
            break;
        }
        case runtime::ExecutionBoundaryKind::UnsupportedInstruction:
        {
            const auto stopped = stop(result, ExecutionStopReason::UnsupportedInstruction,
                                      boundary.target_provenance, std::nullopt, &step.value());
            if (!stopped)
            {
                running_ = false;
                return Result<ExecutionSessionResult>::failure(stopped.error());
            }
            break;
        }
        case runtime::ExecutionBoundaryKind::BudgetExhaustion:
        {
            const auto stopped = stop(result, ExecutionStopReason::IrOperationLimitExceeded,
                                      "session IR operation limit exhausted", std::nullopt,
                                      &step.value());
            if (!stopped)
            {
                running_ = false;
                return Result<ExecutionSessionResult>::failure(stopped.error());
            }
            break;
        }
        case runtime::ExecutionBoundaryKind::SliceExhaustion:
            // A yielded step is consumed above. Reaching this case would mean
            // an inconsistent interpreter status, so fail closed as an
            // unsupported semantic boundary rather than publishing progress.
        {
            const auto stopped = stop(result, ExecutionStopReason::UnsupportedSemantic,
                                      "interpreter returned an untyped slice boundary",
                                      std::nullopt, &step.value());
            if (!stopped)
            {
                running_ = false;
                return Result<ExecutionSessionResult>::failure(stopped.error());
            }
            break;
        }
        case runtime::ExecutionBoundaryKind::None:
        {
            const auto stopped = stop(result, ExecutionStopReason::UnsupportedSemantic,
                                      "interpreter returned without a semantic boundary",
                                      std::nullopt, &step.value());
            if (!stopped)
            {
                running_ = false;
                return Result<ExecutionSessionResult>::failure(stopped.error());
            }
            break;
        }
        }
    }
    result.final_cpu = cpu_;
    if (process_image_ != nullptr)
    {
        for (const auto& binding : process_image_->bindings())
        {
            if (!binding.provider_address || !binding.applied) continue;
            result.provider_guest_code_entered |=
                executed_function_index_.find(binding.provider_address.value()) !=
                executed_function_index_.end();
        }
    }
    result.runtime.dso_modules_registered = runtime_state_.dso_modules_registered();
    if (running_)
    {
        running_ = false;
    }
    result.guest_memory = memory_->accounting();
    if (profiling_enabled())
    {
        std::cerr << "[switchrecomp profile] cache hits=" << result.performance.lift_cache_hits
                  << " misses=" << result.performance.lift_cache_misses
                  << " invalidations=" << result.performance.lift_cache_invalidations << "\n";
        std::cerr << "[switchrecomp profile] phase=execution.lift_for_execution elapsed_us="
                  << profile_totals_.lift_elapsed_us << " calls=" << profile_totals_.lift_calls
                  << "\n";
        std::cerr << "[switchrecomp profile] phase=execute_until_boundary elapsed_us="
                  << profile_totals_.boundary_elapsed_us << " calls="
                  << profile_totals_.boundary_calls << "\n";
        std::cerr << "[switchrecomp profile] phase=ir_verification elapsed_us="
                  << result.performance.ir_verification_elapsed_us << " calls="
                  << result.performance.ir_verification_calls << "\n";
        profile_counter("functions_lifted", result.performance.functions_lifted);
        profile_counter("execution_slices", result.execution_slices);
        profile_counter("target_assessments", result.indirect_target_discovery.size());
    }
    return Result<ExecutionSessionResult>::success(std::move(result));
}

std::string render_execution_report_json(const ExecutionSessionResult& result)
{
    ProfileTimer timer("report_generation");
    json executable_ranges = json::array();
    for (const auto& range : result.identity.executable_ranges)
    {
        executable_ranges.push_back(range_json(range));
    }
    const auto event_json = [](const ExecutionEvent& event) {
        json item{{"sequence", event.sequence}, {"kind", execution_event_kind_name(event.kind)},
                  {"function_entry", hex_address(event.function_entry)},
                  {"guest_pc", hex_address(event.guest_pc)}, {"call_depth", event.call_depth},
                  {"guest_instruction_count", event.guest_instruction_count},
                  {"guest_block_count", event.guest_block_count},
                  {"ir_operation_count", event.ir_operation_count},
                  {"boundary", runtime::execution_boundary_kind_name(event.boundary)},
                  {"code", event.code}};
        if (event.has_target) item["target"] = hex_address(event.target);
        if (!event.import_symbol.empty()) item["import_symbol"] = event.import_symbol;
        if (!event.function_module.empty()) item["function_module"] = event.function_module;
        if (!event.target_module.empty()) item["target_module"] = event.target_module;
        return item;
    };
    json events = json::array();
    for (const auto& event : result.events)
    {
        events.push_back(event_json(event));
    }
    json event_counts = json::object();
    for (std::size_t index = 0U; index < execution_event_kind_count; ++index)
    {
        event_counts[execution_event_kind_name(static_cast<ExecutionEventKind>(index))] =
            result.event_resource.by_kind[index];
    }
    const auto terminal_attempt = result.event_resource.terminal_attempt
                                      ? event_json(result.event_resource.terminal_attempt.value())
                                      : json(nullptr);
    const json event_resource{
        {"model", result.event_resource.model},
        {"total_generated", result.event_resource.total_generated},
        {"retained", result.event_resource.retained},
        {"omitted", result.event_resource.omitted},
        {"history_truncated", result.event_resource.history_truncated},
        {"history_policy", result.event_resource.history_policy},
        {"retention_limit", result.event_resource.retention_limit},
        {"execution_limit", result.event_resource.execution_limit
                                 ? json(result.event_resource.execution_limit.value())
                                 : json(nullptr)},
        {"execution_limit_provenance",
         event_execution_limit_provenance_name(
             result.event_resource.execution_limit_provenance)},
        {"by_kind", std::move(event_counts)},
        {"first_sequence_retained", result.event_resource.first_sequence_retained
                                         ? json(result.event_resource.first_sequence_retained.value())
                                         : json(nullptr)},
        {"last_sequence_retained", result.event_resource.last_sequence_retained
                                        ? json(result.event_resource.last_sequence_retained.value())
                                        : json(nullptr)},
        {"reconciles", result.event_resource.reconciles},
        {"terminal_attempt_sequence", result.event_resource.terminal_attempt_sequence
                                           ? json(result.event_resource.terminal_attempt_sequence.value())
                                           : json(nullptr)},
        {"terminal_attempt_kind", result.event_resource.terminal_attempt_kind
                                       ? json(execution_event_kind_name(
                                             result.event_resource.terminal_attempt_kind.value()))
                                       : json(nullptr)},
        {"terminal_attempt", terminal_attempt}};
    json stack = json::array();
    for (const auto& frame : result.call_stack)
    {
        stack.push_back(json{{"function_entry", hex_address(frame.function_entry)},
                             {"call_site_pc", hex_address(frame.call_site_pc)},
                             {"expected_return_pc", hex_address(frame.expected_return_pc)},
                             {"call_depth", frame.call_depth}});
    }
    json executed = json::array();
    for (const auto entry : result.executed_functions) executed.push_back(hex_address(entry));
    json executed_with_modules = json::array();
    for (std::size_t index = 0U; index < result.executed_functions.size(); ++index)
    {
        executed_with_modules.push_back(json{
            {"address", hex_address(result.executed_functions[index])},
            {"module", index < result.executed_function_modules.size()
                            ? result.executed_function_modules[index]
                            : std::string{}}});
    }
    const auto instruction_json = [](const ExecutedGuestInstruction& instruction) {
        json source_registers = json::array();
        for (const auto& register_name : instruction.source_registers)
            source_registers.push_back(register_name);
        const auto register_values_json = [](const auto& values) {
            json result = json::array();
            for (const auto& value : values)
                result.push_back(json{{"name", value.name}, {"value", hex_address(value.value)}});
            return result;
        };
        return json{
            {"guest_pc", hex_address(instruction.guest_pc)},
            {"module", instruction.module},
            {"mnemonic", instruction.mnemonic},
            {"instruction_id", instruction.instruction_id},
            {"opcode", hex_opcode(instruction.opcode)},
            {"executed", instruction.executed},
            {"next_guest_pc", instruction.next_guest_pc
                                  ? json(hex_address(instruction.next_guest_pc.value()))
                                  : json(nullptr)},
            {"destination_register", instruction.destination_register},
            {"source_registers", std::move(source_registers)},
            {"pre_registers", register_values_json(instruction.pre_registers)},
            {"post_registers", register_values_json(instruction.post_registers)},
            {"expected_high64", instruction.expected_high64
                                    ? json(hex_address(instruction.expected_high64.value()))
                                    : json(nullptr)},
            {"actual_high64", instruction.actual_high64
                                  ? json(hex_address(instruction.actual_high64.value()))
                                  : json(nullptr)},
            {"result_matches", instruction.result_matches
                                    ? json(instruction.result_matches.value())
                                    : json(nullptr)},
            {"call_depth", instruction.call_depth}};
    };
    json executed_guest_instructions = json::array();
    for (const auto& instruction : result.executed_guest_instructions)
    {
        executed_guest_instructions.push_back(instruction_json(instruction));
    }
    json instruction_evidence = json::array();
    for (const auto& instruction : result.instruction_evidence)
        instruction_evidence.push_back(instruction_json(instruction));
    json observation_targets = json::array();
    for (const auto target : result.observation_targets)
        observation_targets.push_back(hex_address(target));
    json indirect_target_discovery = json::array();
    for (const auto& assessment : result.indirect_target_discovery)
    {
        indirect_target_discovery.push_back(indirect_target_assessment_json(assessment));
    }
    json process = nullptr;
    if (result.process)
    {
        json modules = json::array();
        for (const auto& module : result.process->modules)
        {
            json ranges = json::array();
            for (const auto& range : module.mapped_ranges) ranges.push_back(range_json(range));
            json segments = json::array();
            for (const auto& segment : module.segments)
            {
                segments.push_back(json{{"kind", segment.kind},
                                        {"file_offset", segment.file_offset},
                                        {"memory_offset", segment.memory_offset},
                                        {"memory_size", segment.memory_size},
                                        {"stored_size", segment.stored_size},
                                        {"compressed", segment.compressed},
                                        {"hash_required", segment.hash_required}});
            }
            json mappings = json::array();
            for (const auto& mapping : module.mappings)
            {
                mappings.push_back(json{{"base", hex_address(mapping.base)},
                                        {"size", mapping.size},
                                        {"permissions", permissions_text(mapping.permissions)},
                                        {"read", memory::has_permission(
                                                      mapping.permissions,
                                                      memory::GuestMemoryPermissions::Read)},
                                        {"write", memory::has_permission(
                                                       mapping.permissions,
                                                       memory::GuestMemoryPermissions::Write)},
                                        {"execute", memory::has_permission(
                                                         mapping.permissions,
                                                         memory::GuestMemoryPermissions::Execute)},
                                        {"kind", memory::guest_region_kind_name(mapping.kind)}});
            }
            modules.push_back(json{
                {"logical_name", module.logical_name},
                {"name_provenance", module.name_provenance},
                {"input_size", module.input_size},
                {"sha256", module.sha256},
                {"build_id", module.build_id},
                {"nso_version", module.nso_version},
                {"nso_flags", module.nso_flags},
                {"base", hex_address(module.base)},
                {"base_provenance", analysis::module_base_provenance_name(module.base_provenance)},
                {"runtime_base_verified", module.runtime_base_verified},
                {"segments", std::move(segments)},
                {"bss_size", module.bss_size},
                {"mapped_ranges", std::move(ranges)},
                {"mappings", std::move(mappings)},
                {"metadata", json{{"mod0", module.mod0_available},
                                   {"mod0_status", module.mod0_status},
                                   {"dynamic", module.dynamic_available},
                                   {"dynamic_status", module.dynamic_status},
                                   {"provider_index_eligible", module.provider_index_eligible},
                                   {"dynamic_tags", module.dynamic_tags}}},
                {"dynamic_symbols", json{{"total", module.dynamic_symbol_count},
                                          {"defined", module.defined_symbol_count},
                                          {"undefined", module.undefined_symbol_count}}},
                {"relocations", json{{"total", module.relocation_count},
                                      {"applied", module.applied_relocations},
                                      {"unresolved", module.unresolved_relocations}}}});
        }
        json bindings = json::array();
        for (const auto& binding : result.process->bindings)
        {
            json candidates = json::array();
            for (const auto& candidate : binding.provider.candidates)
            {
                candidates.push_back(json{{"module", candidate.module},
                                          {"symbol_index", candidate.symbol_index},
                                          {"symbol", candidate.symbol},
                                          {"binding", format::symbol_binding_name(candidate.binding)},
                                          {"type", format::symbol_type_name(candidate.type)},
                                          {"visibility", format::symbol_visibility_name(candidate.visibility)},
                                          {"section_index", candidate.section_index},
                                          {"value", hex_address(candidate.value)},
                                          {"address", hex_address(candidate.address)},
                                          {"executable", candidate.executable}});
            }
            json occurrences = json::array();
            for (const auto& occurrence : binding.provider.occurrences)
            {
                occurrences.push_back(json{{"module", occurrence.module},
                                           {"symbol_index", occurrence.symbol_index},
                                           {"symbol", occurrence.symbol},
                                           {"defined", occurrence.defined},
                                           {"binding", format::symbol_binding_name(occurrence.binding)},
                                           {"type", format::symbol_type_name(occurrence.type)},
                                           {"visibility", format::symbol_visibility_name(occurrence.visibility)},
                                           {"section_index", occurrence.section_index},
                                           {"value", hex_address(occurrence.value)},
                                           {"address", occurrence.address
                                                            ? json(hex_address(occurrence.address.value()))
                                                            : json(nullptr)},
                                           {"executable", occurrence.executable},
                                           {"eligible", occurrence.eligible},
                                           {"eligibility", analysis::provider_eligibility_name(
                                                                occurrence.eligibility)}});
            }
            bindings.push_back(json{
                {"consumer_module", binding.consumer_module},
                {"consumer_symbol_index", binding.consumer_symbol_index},
                {"symbol", binding.symbol},
                {"relocation_index", binding.relocation_index},
                {"relocation", json{{"type", binding.relocation.raw_type},
                                     {"type_name", format::aarch64_relocation_type_name(binding.relocation.type)},
                                     {"source", format::relocation_source_name(binding.relocation.source)},
                                     {"target", hex_address(binding.relocation.target_address)},
                                     {"offset", hex_address(binding.relocation.offset)}}},
                {"candidate_count", binding.provider.candidates.size()},
                {"result", analysis::provider_resolution_status_name(binding.provider.status)},
                {"provider_module", binding.provider_module ? json(*binding.provider_module) : json(nullptr)},
                {"provider_symbol_index", binding.provider_symbol_index
                                                ? json(*binding.provider_symbol_index) : json(nullptr)},
                {"provider_base", binding.provider_base
                                      ? json(hex_address(*binding.provider_base)) : json(nullptr)},
                {"provider_symbol_value", binding.provider_symbol_value
                                                ? json(hex_address(*binding.provider_symbol_value)) : json(nullptr)},
                {"provider_address", binding.provider_address
                                          ? json(hex_address(*binding.provider_address)) : json(nullptr)},
                {"resolved_value", binding.resolved_value
                                        ? json(hex_address(*binding.resolved_value)) : json(nullptr)},
                {"slot_value_verified", binding.slot_value_verified},
                {"resolution_basis", binding.resolution_basis},
                {"confidence", binding.confidence},
                {"applied", binding.applied},
                {"candidates", std::move(candidates)},
                {"occurrences", std::move(occurrences)},
                {"completeness", analysis::module_set_completeness_name(binding.provider.completeness)},
                {"completeness_basis", analysis::module_set_completeness_basis_name(
                                            binding.provider.completeness_basis)}});
        }
        const auto focus = [&]() {
            json focus_occurrences = json::array();
            json focus_candidates = json::array();
            const auto lookup = result.process->focus_provider.value_or(analysis::ProviderLookup{});
            for (const auto& occurrence : lookup.occurrences)
            {
                focus_occurrences.push_back(json{{"module", occurrence.module},
                                                 {"symbol_index", occurrence.symbol_index},
                                                 {"symbol", occurrence.symbol},
                                                 {"defined", occurrence.defined},
                                                 {"binding", format::symbol_binding_name(occurrence.binding)},
                                                 {"type", format::symbol_type_name(occurrence.type)},
                                                 {"visibility", format::symbol_visibility_name(occurrence.visibility)},
                                                 {"section_index", occurrence.section_index},
                                                 {"value", hex_address(occurrence.value)},
                                                 {"guest_address", occurrence.address
                                                                       ? json(hex_address(occurrence.address.value()))
                                                                       : json(nullptr)},
                                                 {"executable", occurrence.executable},
                                                 {"eligible", occurrence.eligible},
                                                 {"eligibility", analysis::provider_eligibility_name(
                                                                      occurrence.eligibility)}});
            }
            for (const auto& candidate : lookup.candidates)
                focus_candidates.push_back(json{{"module", candidate.module},
                                                {"symbol_index", candidate.symbol_index},
                                                {"symbol", candidate.symbol},
                                                {"binding", format::symbol_binding_name(candidate.binding)},
                                                {"type", format::symbol_type_name(candidate.type)},
                                                {"visibility", format::symbol_visibility_name(candidate.visibility)},
                                                {"value", hex_address(candidate.value)},
                                                {"guest_address", hex_address(candidate.address)},
                                                {"executable", candidate.executable}});
            json provider = nullptr;
            if (lookup.selected_candidate && lookup.selected_candidate.value() < lookup.candidates.size())
            {
                const auto& selected = lookup.candidates[lookup.selected_candidate.value()];
                provider = json{{"module", selected.module},
                                {"symbol_index", selected.symbol_index},
                                {"guest_address", hex_address(selected.address)}};
            }
            bool host_handler_used = false;
            for (const auto& import : result.runtime.imports)
                host_handler_used |= import.provenance.symbol.name == "__nnmusl_init_dso";
            return json{{"__nnmusl_init_dso", json{
                {"occurrences", std::move(focus_occurrences)},
                {"eligible_provider_candidates", std::move(focus_candidates)},
                {"result", analysis::provider_resolution_status_name(lookup.status)},
                {"search_complete", lookup.completeness != analysis::ModuleSetCompleteness::Incomplete},
                {"completeness", analysis::module_set_completeness_name(lookup.completeness)},
                {"completeness_basis", analysis::module_set_completeness_basis_name(lookup.completeness_basis)},
                {"provider", std::move(provider)},
                {"selected_candidate", lookup.selected_candidate
                                             ? json(lookup.selected_candidate.value()) : json(nullptr)},
                {"provider_guest_code_entered", result.provider_guest_code_entered},
                {"host_handler_used", host_handler_used}}}};
        }();
        process = json{{"primary_module", result.process->primary_module},
                       {"layout_mode", result.process->layout_mode},
                       {"provider_search_complete", result.process->provider_search_complete},
                       {"module_set", json{{"source", result.process->source},
                                            {"completeness", analysis::module_set_completeness_name(
                                                                  result.process->completeness)},
                                            {"completeness_basis", analysis::module_set_completeness_basis_name(
                                                                  result.process->completeness_basis)},
                                            {"coherence", analysis::module_set_coherence_name(
                                                                  result.process->coherence)},
                                            {"coherence_basis", result.process->coherence_basis},
                                            {"module_load_order", result.process->module_load_order},
                                            {"module_load_order_basis", result.process->module_load_order_basis},
                                            {"module_count", result.process->module_count},
                                            {"executable_module_count", result.process->executable_module_count},
                                            {"relocations_planned", result.process->relocations_planned},
                                            {"transactional_relocation_success",
                                             result.process->transactional_relocation_success},
                                            {"ignored_entries", result.process->ignored_module_entries}}},
                       {"modules", std::move(modules)}, {"bindings", std::move(bindings)},
                       {"focus_symbols", std::move(focus)}};
    }
    json value{{"schema_version", ExecutionSessionResult::schema_version},
                {"module", json{{"logical_name", result.identity.module},
                                  {"sha256", result.identity.input_sha256},
                                  {"build_id", result.identity.build_id},
                                  {"analysis_base", hex_address(result.identity.guest_base)},
                                  {"guest_base_verified", result.identity.guest_base_verified},
                                  {"base_provenance", analysis::module_base_provenance_name(
                                                          result.identity.guest_base_provenance)},
                                  {"executable_ranges", std::move(executable_ranges)}}},
                {"relocations", json{{"parsed", result.relocations.relocations_parsed},
                                      {"applied", result.relocations.relocations_applied},
                                      {"unresolved", result.relocations.unresolved_relocations},
                                      {"guest_bindings_attempted", result.relocations.guest_bindings_attempted},
                                      {"guest_bindings_resolved", result.relocations.guest_bindings_resolved},
                                      {"guest_bindings_ambiguous", result.relocations.guest_bindings_ambiguous},
                                      {"guest_bindings_unresolved", result.relocations.guest_bindings_unresolved},
                                      {"cross_module_relocations", result.relocations.cross_module_relocations}}},
                {"analysis", [&]() {
                     auto analysis = analysis_report_json(result.analysis);
                     analysis["legacy_totals"] = json{{"functions", result.analyzed_functions},
                                   {"conflicting_functions", result.conflicting_functions},
                                   {"conflict_records", result.precise_conflicts},
                                   {"precise_conflicts", result.precise_conflicts},
                                   {"precise_owned_bytes", result.precise_owned_bytes}};
                     return analysis;
                 }()},
                {"metadata", module_metadata_json(result.module_metadata)},
                {"entry", json{{"kind", entry_selection_kind_name(result.entry.kind)},
                                {"address", hex_address(result.entry.address)},
                                {"source", result.entry.source},
                                {"confidence", analysis::function_confidence_name(result.entry.confidence)},
                                {"verified_process_entry", result.entry.verified_process_entry}}},
                {"launch_context", json{{"synthetic_stack", true},
                                         {"synthetic_stack_size", result.options.synthetic_stack_size},
                                         {"stack_base", hex_address(result.stack_base)},
                                         {"stack_end", hex_address(result.stack_end)},
                                         {"initial_sp", hex_address(result.initial_sp)},
                                         {"synthetic_lr_sentinel", hex_address(result.synthetic_lr_sentinel)},
                                         {"tls", "synthetic_zero"},
                                         {"state_description", "deterministic controlled-run state"}}},
                {"guest_memory", [&]() {
                     const auto& accounting = result.guest_memory;
                     const auto non_stack_bytes =
                         accounting.live_mapped_bytes >= accounting.live_owned_bytes
                             ? accounting.live_mapped_bytes - accounting.live_owned_bytes
                             : 0U;
                     return json{{"max_individual_region_size", accounting.max_region_size},
                                 {"max_total_size", accounting.max_total_size},
                                 {"max_regions", accounting.max_regions},
                                 {"live_mapped_bytes", accounting.live_mapped_bytes},
                                 {"non_stack_live_mapped_bytes", non_stack_bytes},
                                 {"peak_live_mapped_bytes", accounting.peak_live_mapped_bytes},
                                 {"final_live_mapped_bytes", accounting.live_mapped_bytes},
                                 {"live_region_count", accounting.live_region_count},
                                 {"peak_region_count", accounting.peak_region_count},
                                 {"controlled_stack_mappings_created",
                                  accounting.owned_mappings_created},
                                 {"controlled_stack_mappings_reclaimed",
                                  accounting.owned_mappings_reclaimed},
                                 {"controlled_stack_mappings_live", accounting.live_owned_mappings},
                                 {"peak_simultaneously_live_controlled_stacks",
                                  accounting.peak_live_owned_mappings},
                                 {"live_controlled_stack_bytes", accounting.live_owned_bytes},
                                 {"peak_live_controlled_stack_bytes",
                                  accounting.peak_live_owned_bytes},
                                 {"cumulative_mapped_bytes", accounting.cumulative_mapped_bytes},
                                 {"cumulative_controlled_stack_bytes",
                                  accounting.cumulative_owned_bytes},
                                 {"virtual_stack_allocation_high_water",
                                  hex_address(accounting.virtual_address_high_water)}};
                 }()},
                {"execution", json{{"stop_reason", execution_stop_reason_name(result.stop_reason)},
                                    {"stop_pc", hex_address(result.stop_pc)},
                                    {"source_pc", result.source_pc
                                                       ? json(hex_address(result.source_pc.value()))
                                                       : json(nullptr)},
                                    {"diagnostic_pc", result.diagnostic_pc
                                                           ? json(hex_address(result.diagnostic_pc.value()))
                                                           : json(nullptr)},
                                    {"current_function", hex_address(result.current_function)},
                                    {"current_function_module", result.current_function_module},
                                    {"stop_module", result.stop_module},
                                    {"target", result.target ? json(hex_address(result.target.value())) : json(nullptr)},
                                    {"target_register", result.target_register},
                                    {"target_provenance", result.target_provenance},
                                    {"diagnostic_opcode", result.diagnostic_opcode
                                                                ? json(result.diagnostic_opcode.value())
                                                                : json(nullptr)},
                                    {"diagnostic_instruction_id", result.diagnostic_instruction_id},
                                    {"diagnostic_instruction", result.diagnostic_instruction},
                                    {"executed_functions", std::move(executed)},
                                    {"executed_functions_with_modules", std::move(executed_with_modules)},
                                    {"direct_calls", result.direct_calls},
                                    {"indirect_calls", result.indirect_calls},
                                    {"function_transfers", result.function_transfers},
                                    {"returns", result.returns},
                                    {"ir_operations", result.ir_operations},
                                    {"guest_blocks", result.guest_blocks},
                                    {"guest_instruction_count", result.guest_instruction_count},
                                    {"execution_slices", result.execution_slices},
                                    {"resumable_yields", result.resumable_yields},
                                    {"resumes", result.resumes},
                                    {"mid_block_resumes", result.mid_block_resumes},
                                    {"maximum_ir_operations_in_slice",
                                     result.maximum_ir_operations_in_slice},
                                    {"terminal_ir_cursor", result.terminal_ir_block
                                                                  ? json{{"block", result.terminal_ir_block.value()},
                                                                         {"operation_index", result.terminal_ir_operation_index.value_or(0U)}}
                                                                  : json(nullptr)},
                                    {"instructions_after_former_blocker",
                                     result.instructions_after_former_blocker},
                                    {"instructions_after_smulh", result.instructions_after_smulh},
                                    {"maximum_call_depth", result.maximum_call_depth},
                                    {"transition_accounting",
                                     transition_accounting_json(result.transition_accounting)},
                                    {"provider_guest_code_entered", result.provider_guest_code_entered},
                                    {"runtime_fallbacks_invoked", result.runtime.imports_handled},
                                    {"observation_targets", std::move(observation_targets)},
                                    {"instruction_observation_targets", [&]() {
                                         json targets = json::array();
                                         for (const auto target : result.instruction_observation_targets)
                                             targets.push_back(hex_address(target));
                                         return targets;
                                     }()},
                                    {"executed_guest_instructions",
                                     std::move(executed_guest_instructions)},
                                    {"instruction_evidence", std::move(instruction_evidence)},
                                    {"smulh_frontier", json{
                                         {"reached", result.smulh_frontier.reached},
                                         {"module", result.smulh_frontier.module},
                                         {"pc", hex_address(result.smulh_frontier.pc)},
                                         {"opcode", hex_opcode(result.smulh_frontier.opcode)},
                                         {"instruction", result.smulh_frontier.instruction},
                                         {"destination_register", result.smulh_frontier.destination_register},
                                         {"lhs_register", result.smulh_frontier.lhs_register},
                                         {"lhs_raw", hex_address(result.smulh_frontier.lhs_raw)},
                                         {"lhs_signed", result.smulh_frontier.lhs_signed},
                                         {"rhs_register", result.smulh_frontier.rhs_register},
                                         {"rhs_raw", hex_address(result.smulh_frontier.rhs_raw)},
                                         {"rhs_signed", result.smulh_frontier.rhs_signed},
                                         {"expected_high64", hex_address(result.smulh_frontier.expected_high64)},
                                         {"actual_high64", hex_address(result.smulh_frontier.actual_high64)},
                                         {"match", result.smulh_frontier.match},
                                         {"next_guest_pc", result.smulh_frontier.next_guest_pc
                                                               ? json(hex_address(result.smulh_frontier.next_guest_pc.value()))
                                                               : json(nullptr)}}},
                                    {"indirect_target_refinement",
                                     indirect_target_refinement_json(result.indirect_target_refinement)},
                                    {"indirect_target_discovery", std::move(indirect_target_discovery)},
                                    {"diagnostic", result.diagnostic}}},
                {"budgets", json{{"max_ir_operations", result.options.budgets.max_ir_operations
                                                               ? json(result.options.budgets.max_ir_operations.value())
                                                               : json(nullptr)},
                                  {"ir_operation_limit_provenance",
                                   result.options.budgets.max_ir_operations
                                       ? ir_operation_limit_provenance_name(
                                             result.options.budgets.ir_operation_limit_provenance ==
                                                     IrOperationLimitProvenance::OrdinaryDefault
                                                 ? IrOperationLimitProvenance::ExplicitLibraryApi
                                                 : result.options.budgets.ir_operation_limit_provenance)
                                       : ir_operation_limit_provenance_name(
                                             IrOperationLimitProvenance::OrdinaryDefault)},
                                  {"slice_ir_operations", result.options.budgets.slice_ir_operations},
                                  {"max_function_transitions", result.options.budgets.max_function_transitions},
                                  {"max_call_depth", result.options.budgets.max_call_depth},
                                  {"max_events", result.options.budgets.max_events
                                                      ? json(result.options.budgets.max_events.value())
                                                      : json(nullptr)},
                                  {"event_execution_limit_provenance",
                                   result.options.budgets.max_events
                                       ? event_execution_limit_provenance_name(
                                             result.event_resource.execution_limit_provenance)
                                       : "not_configured"},
                                  {"event_history_limit", result.options.budgets.event_history_limit},
                                  {"max_guest_blocks", result.options.budgets.max_guest_blocks}}},
                {"call_stack_snapshot", std::move(stack)},
                {"registers", cpu_json(result.final_cpu)},
                {"runtime", json{{"imports_encountered", result.runtime.imports_encountered},
                                  {"imports_resolved", result.runtime.imports_resolved},
                                  {"imports_handled", result.runtime.imports_handled},
                                  {"imports_unimplemented", result.runtime.imports_unimplemented},
                                  {"imports_abi_violations", result.runtime.imports_abi_violations},
                                  {"imports_memory_faults", result.runtime.imports_memory_faults},
                                  {"imports_invariant_violations",
                                   result.runtime.imports_invariant_violations},
                                  {"import_returns", result.runtime.import_returns},
                                  {"dso_modules_registered",
                                   result.runtime.dso_modules_registered},
                                  {"imports", [&]() {
                                      json imports = json::array();
                                      for (const auto& observation : result.runtime.imports)
                                      {
                                          imports.push_back(runtime_import_json(observation));
                                      }
                                      return imports;
                                  }()}}},
                {"event_resource", event_resource},
                {"events", std::move(events)},
                {"process", std::move(process)}};
    if (result.import_boundary)
    {
        const auto& import = result.import_boundary.value();
        value["execution"]["import"] = json{{"consumer_module", import.consumer_module},
                                               {"relocation_target", hex_address(import.relocation_target)},
                                               {"relocation_offset", hex_address(import.relocation.offset)},
                                               {"relocation_index", import.relocation_index},
                                               {"symbol_index", import.symbol.symbol_index},
                                               {"symbol", import.symbol.name},
                                               {"binding", format::symbol_binding_name(import.symbol.binding)},
                                               {"symbol_type", format::symbol_type_name(import.symbol.type)},
                                               {"visibility", format::symbol_visibility_name(import.symbol.visibility)},
                                               {"section_index", import.symbol.section_index},
                                               {"undefined_in_main", import.symbol.section_index == 0U},
                                               {"relocation_type", import.relocation.raw_type},
                                               {"relocation_type_name", format::aarch64_relocation_type_name(import.relocation.type)},
                                               {"relocation_source", format::relocation_source_name(import.relocation.source)},
                                               {"addend", import.relocation.addend}};
    }
    return value.dump(2) + "\n";
}

} // namespace switchrecomp::execution
