#include "switchrecomp/analysis/function_map.hpp"
#include "switchrecomp/analysis/indirect_target.hpp"
#include "switchrecomp/execution/session.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace
{

using namespace switchrecomp;
using analysis::FunctionConfidence;
using analysis::FunctionDiscoverySource;
using analysis::FunctionSeed;
using analysis::IndirectControlFlowKind;
using analysis::IndirectTargetDecisionKind;
using analysis::IndirectTargetRefinementBudgetDimension;
using analysis::IndirectTargetRefinementBudgets;
using analysis::IndirectTargetRefinementWorklist;
using analysis::ModuleIdentity;
using analysis::ObservedIndirectTarget;
using memory::GuestAddress;

constexpr std::uint32_t ret_instruction = 0xd65f03c0U;
constexpr std::uint32_t unconditional_branch_instruction_base = 0x14000000U;

void write_word(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value)
{
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[offset + 2U] = static_cast<std::byte>((value >> 16U) & 0xffU);
    bytes[offset + 3U] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

[[nodiscard]] std::uint32_t unconditional_branch(GuestAddress pc, GuestAddress target)
{
    const auto displacement = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pc);
    return unconditional_branch_instruction_base |
           (static_cast<std::uint32_t>(displacement / 4) & 0x03ffffffU);
}

struct SyntheticModule
{
    memory::GuestMemory memory;
    ModuleIdentity identity;
};

[[nodiscard]] Result<SyntheticModule> make_ret_module(std::string name, GuestAddress base,
                                                       std::size_t function_count)
{
    SyntheticModule result;
    if (function_count > std::numeric_limits<std::size_t>::max() / 4U)
        return Result<SyntheticModule>::failure(
            make_error(ErrorCode::ArithmeticOverflow, "synthetic function image size overflows"));
    std::vector<std::byte> code(function_count * 4U, std::byte{});
    for (std::size_t offset = 0U; offset < code.size(); offset += 4U)
        write_word(code, offset, ret_instruction);
    const auto mapped = result.memory.map(
        base, std::span<const std::byte>(code.data(), code.size()),
        memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Execute,
        "synthetic.m33.text", memory::GuestRegionKind::Text);
    if (!mapped) return Result<SyntheticModule>::failure(mapped.error());
    result.identity.module = std::move(name);
    result.identity.build_id = "synthetic-m33-build";
    result.identity.input_sha256 = "synthetic-m33-sha";
    result.identity.guest_base = base;
    result.identity.executable_ranges.push_back(
        analysis::GuestAddressRange{base, static_cast<memory::GuestSize>(code.size())});
    return Result<SyntheticModule>::success(std::move(result));
}

[[nodiscard]] FunctionSeed seed(GuestAddress entry, FunctionDiscoverySource source,
                                FunctionConfidence confidence, std::string note = {})
{
    return FunctionSeed{entry, source, confidence, std::nullopt, std::nullopt, std::move(note)};
}

[[nodiscard]] ObservedIndirectTarget observed(const std::string& module, GuestAddress target,
                                              GuestAddress source_pc = 0x400U)
{
    ObservedIndirectTarget result;
    result.source_module = module;
    result.source_function = 0x300U;
    result.source_pc = source_pc;
    result.control_flow = IndirectControlFlowKind::Call;
    result.target_register = "x8";
    result.target = target;
    result.target_module = module;
    result.pointer_provenance = analysis::IndirectTargetPointerProvenanceKind::GuestLoad;
    result.guest_load_address = 0x8000U + target;
    return result;
}

[[nodiscard]] analysis::IndirectTargetAssessment assessment(
    const ObservedIndirectTarget& item)
{
    analysis::IndirectTargetAssessment result;
    result.observed = item;
    result.validation.target_module = item.target_module;
    result.validation.structurally_eligible = true;
    result.decision.kind = IndirectTargetDecisionKind::TrustedNewEntry;
    result.decision.eligible_for_promotion = true;
    return result;
}

[[nodiscard]] std::string map_fingerprint(const analysis::FinalizedFunctionMap& map)
{
    std::string result = analysis::render_analysis_accounting_json(map.accounting());
    for (const auto& function : map.functions())
    {
        result += ':' + std::to_string(function.canonical_entry);
        result += ':' + std::to_string(function.entries.size());
        result += ':' + std::to_string(function.owned_code_ranges.size());
        result += ':' + std::to_string(function.cfg ? function.cfg->instruction_count : 0U);
    }
    return result;
}

[[nodiscard]] analysis::FinalizedFunctionMap make_initial_map(
    const SyntheticModule& module, GuestAddress root)
{
    const auto result = analysis::FunctionMapBuilder::build(
        analysis::ModuleAnalysisInput{
            module.identity, &module.memory,
            {seed(root, FunctionDiscoverySource::ModuleEntry, FunctionConfidence::Confirmed,
                  "synthetic M33 root")} });
    REQUIRE(result);
    return std::move(result).value();
}

} // namespace

TEST_CASE("M33 ordinary defaults cross 512 real reusable immutable transactions")
{
    constexpr std::size_t function_count = 514U;
    constexpr GuestAddress base = 0x100000U;
    auto module = make_ret_module("synthetic-m33", base, function_count);
    REQUIRE(module);
    const std::array<ModuleIdentity, 1U> identities{module.value().identity};
    const auto universe = analysis::derive_indirect_target_candidate_universe(
        std::span<const ModuleIdentity>(identities.data(), identities.size()));
    REQUIRE(universe);
    REQUIRE(universe.value().executable_instruction_slots == function_count);

    const std::vector<FunctionSeed> roots{
        seed(base, FunctionDiscoverySource::ModuleEntry, FunctionConfidence::Confirmed,
             "synthetic M33 root")};
    auto map = make_initial_map(module.value(), base);
    REQUIRE(map.accounting().refinement_transactions == 0U);
    REQUIRE(map.accounting().boundary_finalization_passes >= 1U);

    IndirectTargetRefinementBudgets budgets;
    budgets.candidate_universe = universe.value();
    IndirectTargetRefinementWorklist worklist(budgets);
    analysis::IndirectTargetDiscoveryOptions options;

    for (std::size_t index = 1U; index < function_count; ++index)
    {
        REQUIRE(worklist.begin_round());
        const auto item = observed("synthetic-m33", base + index * 4U);
        const auto assessed = analysis::assess_indirect_target(item, module.value().memory, &map,
                                                               nullptr, nullptr, options);
        REQUIRE(assessed);
        REQUIRE(assessed.value().decision.kind == IndirectTargetDecisionKind::TrustedNewEntry);
        REQUIRE(worklist.observe(assessed.value()).newly_unique_candidate);
        const auto pending = worklist.pending_candidates();
        REQUIRE(pending.size() == 1U);
        const auto identity = analysis::indirect_target_candidate_identity(pending.front());
        REQUIRE(worklist.begin_candidate_assessment(identity));

        const auto refined = analysis::refine_function_map(
            map, analysis::ModuleAnalysisInput{module.value().identity, &module.value().memory,
                                                roots},
            item, options);
        REQUIRE(refined);
        REQUIRE(refined.value().assessment.decision.promoted);
        REQUIRE(refined.value().analysis_work.transactions == 1U);
        REQUIRE(refined.value().analysis_work.boundary_finalization_passes >= 1U);
        REQUIRE(refined.value().analysis_work.transactions <=
                refined.value().analysis_work.boundary_finalization_passes);
        REQUIRE(worklist.can_commit_refinement(identity, refined.value().analysis_work));
        map = std::move(refined.value().map);
        worklist.record_promotion(identity, 1U, 0U, refined.value().analysis_work);
        worklist.end_round();
    }

    const auto summary = worklist.summary();
    REQUIRE_FALSE(summary.configured.analysis.max_transactions.has_value());
    REQUIRE(summary.configured.analysis.transactions_provenance.detail == "not_configured");
    REQUIRE(summary.successful_promotions == function_count - 1U);
    REQUIRE(summary.map_generation == function_count - 1U);
    REQUIRE(summary.analysis.transactions == function_count - 1U);
    REQUIRE(summary.analysis.boundary_finalization_passes >= summary.analysis.transactions);
    REQUIRE(summary.exhaustion.dimension == IndirectTargetRefinementBudgetDimension::None);
    REQUIRE(summary.pending_candidate_count == 0U);
    REQUIRE(map.find_exact_entry(base + (function_count - 1U) * 4U) != nullptr);
}

TEST_CASE("M33 transaction accounting covers initial, reusable, and invalidating builds")
{
    constexpr GuestAddress base = 0x200000U;
    auto module = make_ret_module("synthetic-m33-accounting", base, 4U);
    REQUIRE(module);
    auto initial = make_initial_map(module.value(), base);
    REQUIRE(initial.accounting().refinement_transactions == 0U);
    REQUIRE(initial.accounting().boundary_finalization_passes >= 1U);

    const std::vector<FunctionSeed> roots{
        seed(base, FunctionDiscoverySource::ModuleEntry, FunctionConfidence::Confirmed,
             "synthetic M33 root")};
    const auto no_invalidation = analysis::refine_function_map(
        initial, analysis::ModuleAnalysisInput{module.value().identity, &module.value().memory,
                                                roots},
        observed("synthetic-m33-accounting", base + 4U), {});
    REQUIRE(no_invalidation);
    REQUIRE(no_invalidation.value().assessment.decision.promoted);
    REQUIRE(no_invalidation.value().analysis_work.transactions == 1U);
    REQUIRE(no_invalidation.value().analysis_work.boundary_finalization_passes >= 1U);
    REQUIRE(no_invalidation.value().analysis_work.invalidated_records == 0U);
    REQUIRE(no_invalidation.value().analysis_work.functions_reanalyzed == 0U);

    constexpr GuestAddress boundary_base = 0x210000U;
    auto boundary_module = make_ret_module("synthetic-m33-boundary", boundary_base, 16U);
    REQUIRE(boundary_module);
    // The initial CFG owns the branch target. Introducing that target as a
    // callable boundary forces production invalidation and reanalysis.
    std::vector<std::byte> branch_code(0x40U, std::byte{});
    for (std::size_t offset = 0U; offset < branch_code.size(); offset += 4U)
        write_word(branch_code, offset, ret_instruction);
    write_word(branch_code, 0U, unconditional_branch(boundary_base, boundary_base + 8U));
    boundary_module.value().memory = memory::GuestMemory{};
    REQUIRE(boundary_module.value().memory.map(
        boundary_base, std::span<const std::byte>(branch_code.data(), branch_code.size()),
        memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Execute,
        "synthetic.m33.boundary", memory::GuestRegionKind::Text));
    boundary_module.value().identity.executable_ranges = {
        analysis::GuestAddressRange{boundary_base, branch_code.size()}};
    auto boundary_initial = make_initial_map(boundary_module.value(), boundary_base);
    analysis::FunctionMapOptions rebuild_options;
    rebuild_options.reuse_map = &boundary_initial;
    rebuild_options.newly_introduced_function_entries.insert(boundary_base + 8U);
    const auto rebuilt = analysis::FunctionMapBuilder::build(
        analysis::ModuleAnalysisInput{
            boundary_module.value().identity, &boundary_module.value().memory,
            {seed(boundary_base, FunctionDiscoverySource::ModuleEntry,
                  FunctionConfidence::Confirmed, "synthetic M33 root"),
             seed(boundary_base + 8U, FunctionDiscoverySource::ObservedIndirectTarget,
                  FunctionConfidence::High, "synthetic M33 boundary")}},
        rebuild_options);
    REQUIRE(rebuilt);
    REQUIRE(rebuilt.value().accounting().refinement_transactions == 1U);
    REQUIRE(rebuilt.value().accounting().boundary_finalization_passes >= 1U);
    REQUIRE(rebuilt.value().accounting().refinement_transactions <=
            rebuilt.value().accounting().boundary_finalization_passes);
    REQUIRE(rebuilt.value().accounting().invalidated_records >= 1U);
    REQUIRE(rebuilt.value().accounting().reanalyzed_functions >= 1U);
}

TEST_CASE("M33 explicit transaction ceiling is exact and rollback is immutable")
{
    constexpr GuestAddress base = 0x300000U;
    auto module = make_ret_module("synthetic-m33-ceiling", base, 4U);
    REQUIRE(module);
    const std::vector<FunctionSeed> roots{
        seed(base, FunctionDiscoverySource::ModuleEntry, FunctionConfidence::Confirmed,
             "synthetic M33 root")};
    auto map = make_initial_map(module.value(), base);
    analysis::IndirectTargetDiscoveryOptions options;
    IndirectTargetRefinementBudgets budgets;
    budgets.candidate_universe.executable_instruction_slots = 4U;
    budgets.analysis.max_transactions = 2U;
    IndirectTargetRefinementWorklist worklist(budgets);

    for (std::size_t index = 1U; index <= 2U; ++index)
    {
        REQUIRE(worklist.begin_round());
        const auto item = observed("synthetic-m33-ceiling", base + index * 4U);
        const auto assessed = analysis::assess_indirect_target(item, module.value().memory, &map,
                                                               nullptr, nullptr, options);
        REQUIRE(assessed);
        REQUIRE(worklist.observe(assessed.value()).newly_unique_candidate);
        const auto identity = analysis::indirect_target_candidate_identity(item);
        REQUIRE(worklist.begin_candidate_assessment(identity));
        const auto refined = analysis::refine_function_map(
            map, analysis::ModuleAnalysisInput{module.value().identity, &module.value().memory,
                                                roots},
            item, options);
        REQUIRE(refined);
        REQUIRE(refined.value().assessment.decision.promoted);
        REQUIRE(worklist.can_commit_refinement(identity, refined.value().analysis_work));
        map = std::move(refined.value().map);
        worklist.record_promotion(identity, 1U, 0U, refined.value().analysis_work);
        worklist.end_round();
    }

    REQUIRE(worklist.summary().analysis.transactions == 2U);
    REQUIRE(worklist.summary().map_generation == 2U);
    const auto third = observed("synthetic-m33-ceiling", base + 12U);
    const auto third_identity = analysis::indirect_target_candidate_identity(third);
    REQUIRE(worklist.begin_round());
    const auto assessed = analysis::assess_indirect_target(third, module.value().memory, &map,
                                                           nullptr, nullptr, options);
    REQUIRE(assessed);
    REQUIRE(worklist.observe(assessed.value()).newly_unique_candidate);
    REQUIRE(worklist.begin_candidate_assessment(third_identity));
    const auto before = map_fingerprint(map);
    const auto speculative = analysis::refine_function_map(
        map, analysis::ModuleAnalysisInput{module.value().identity, &module.value().memory, roots},
        third, options);
    REQUIRE(speculative);
    REQUIRE(speculative.value().assessment.decision.promoted);
    REQUIRE(speculative.value().analysis_work.transactions <=
            speculative.value().analysis_work.boundary_finalization_passes);
    REQUIRE_FALSE(worklist.can_commit_refinement(third_identity,
                                                  speculative.value().analysis_work));
    worklist.record_rollback_assessment(third_identity);
    worklist.end_round();

    const auto summary = worklist.summary();
    REQUIRE(summary.exhaustion.dimension ==
            IndirectTargetRefinementBudgetDimension::RefinementAnalysisTransactions);
    REQUIRE(summary.exhaustion.consumed == 2U);
    REQUIRE(summary.exhaustion.limit == 2U);
    REQUIRE(summary.exhaustion.module == "synthetic-m33-ceiling");
    REQUIRE(summary.exhaustion.generation == 2U);
    REQUIRE(summary.exhaustion.next_work == third_identity);
    REQUIRE(summary.rollback_assessments == 1U);
    REQUIRE(summary.map_generation == 2U);
    REQUIRE(summary.pending_candidate_count == 1U);
    REQUIRE(summary.same_generation_assessment_attempts == 0U);
    REQUIRE_FALSE(worklist.begin_candidate_assessment(third_identity));
    REQUIRE(map_fingerprint(map) == before);
    REQUIRE(map.find_exact_entry(base + 12U) == nullptr);
    REQUIRE(speculative.value().map.find_exact_entry(base + 12U) != nullptr);
}

TEST_CASE("M33 reports ordinary and explicit transaction semantics deterministically")
{
    const auto ordinary = IndirectTargetRefinementWorklist{}.summary();
    execution::ExecutionSessionResult ordinary_result;
    ordinary_result.indirect_target_refinement = ordinary;
    const auto ordinary_report = execution::render_execution_report_json(ordinary_result);
    REQUIRE(execution::ExecutionSessionResult::schema_version == 20U);
    const auto ordinary_json = nlohmann::json::parse(ordinary_report);
    const auto& ordinary_resource = ordinary_json.at("execution")
                                        .at("indirect_target_refinement")
                                        .at("transaction_resource");
    REQUIRE(ordinary_json.at("schema_version").get<std::uint32_t>() == 20U);
    REQUIRE(ordinary_resource.at("ordinary_termination_mode") ==
            "semantic_aggregate_resources");
    REQUIRE_FALSE(ordinary_resource.at("transaction_ceiling_configured").get<bool>());
    REQUIRE(ordinary_resource.at("configured_transaction_limit").is_null());
    REQUIRE(ordinary_resource.at("dominating_resource") == "boundary_finalization_passes");
    REQUIRE(ordinary_resource.at("termination_invariant") ==
            "transactions <= boundary_finalization_passes");

    IndirectTargetRefinementBudgets explicit_budgets;
    explicit_budgets.analysis.max_transactions = 2U;
    const auto explicit_summary = IndirectTargetRefinementWorklist(explicit_budgets).summary();
    execution::ExecutionSessionResult explicit_result;
    explicit_result.indirect_target_refinement = explicit_summary;
    const auto explicit_report = execution::render_execution_report_json(explicit_result);
    const auto explicit_json = nlohmann::json::parse(explicit_report);
    const auto& explicit_resource = explicit_json.at("execution")
                                        .at("indirect_target_refinement")
                                        .at("transaction_resource");
    REQUIRE(explicit_resource.at("transaction_ceiling_configured").get<bool>());
    REQUIRE(explicit_resource.at("configured_transaction_limit").get<std::size_t>() == 2U);
    REQUIRE(explicit_resource.at("transaction_limit_provenance").at("kind") ==
            "explicit_api_override");
}

TEST_CASE("M33 transaction accounting remains checked at the maximum representable count")
{
    IndirectTargetRefinementBudgets budgets;
    budgets.candidate_universe.executable_instruction_slots = 2U;
    budgets.analysis.max_transactions = std::numeric_limits<std::size_t>::max();
    IndirectTargetRefinementWorklist worklist(budgets);
    const auto first = observed("synthetic-m33-overflow", 0x4000U);
    const auto second = observed("synthetic-m33-overflow", 0x4004U);
    const auto first_identity = analysis::indirect_target_candidate_identity(first);
    const auto second_identity = analysis::indirect_target_candidate_identity(second);
    const analysis::IndirectTargetRefinementAnalysisWork max_work{
        "synthetic-m33-overflow", 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 0U,
        std::numeric_limits<std::size_t>::max()};
    REQUIRE(worklist.observe(assessment(first)).accepted);
    REQUIRE(worklist.begin_candidate_assessment(first_identity));
    REQUIRE(worklist.can_commit_refinement(first_identity, max_work));
    worklist.record_promotion(first_identity, 0U, 0U, max_work);
    REQUIRE(worklist.observe(assessment(second)).accepted);
    REQUIRE(worklist.begin_candidate_assessment(second_identity));
    REQUIRE_FALSE(worklist.can_commit_refinement(second_identity, max_work));
    const auto summary = worklist.summary();
    REQUIRE(summary.analysis.transactions == std::numeric_limits<std::size_t>::max());
    REQUIRE(summary.exhaustion.dimension ==
            IndirectTargetRefinementBudgetDimension::RefinementAnalysisTransactions);
    REQUIRE(summary.exhaustion.consumed == std::numeric_limits<std::size_t>::max());
    REQUIRE(summary.exhaustion.limit == std::numeric_limits<std::size_t>::max());
    REQUIRE(summary.exhaustion.next_work == second_identity);
}
