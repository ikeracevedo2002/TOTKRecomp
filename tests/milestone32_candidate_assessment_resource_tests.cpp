#include "switchrecomp/analysis/function_map.hpp"
#include "switchrecomp/analysis/indirect_target.hpp"
#include "switchrecomp/execution/session.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
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

void write_word(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value)
{
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[offset + 2U] = static_cast<std::byte>((value >> 16U) & 0xffU);
    bytes[offset + 3U] = static_cast<std::byte>((value >> 24U) & 0xffU);
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
    std::vector<std::byte> code(function_count * 4U, std::byte{});
    for (std::size_t offset = 0U; offset < code.size(); offset += 4U)
        write_word(code, offset, ret_instruction);
    const auto mapped = result.memory.map(
        base, std::span<const std::byte>(code.data(), code.size()),
        memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Execute,
        "synthetic.m32.text", memory::GuestRegionKind::Text);
    if (!mapped) return Result<SyntheticModule>::failure(mapped.error());
    result.identity.module = std::move(name);
    result.identity.build_id = "synthetic-m32-build";
    result.identity.input_sha256 = "synthetic-m32-sha";
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

[[nodiscard]] ObservedIndirectTarget observed(GuestAddress target,
                                              GuestAddress source_pc = 0x1000U)
{
    ObservedIndirectTarget result;
    result.source_module = "synthetic-m32";
    result.source_function = 0x0f00U;
    result.source_pc = source_pc;
    result.control_flow = IndirectControlFlowKind::Call;
    result.target_register = "x8";
    result.target = target;
    result.target_module = "synthetic-m32";
    result.pointer_provenance = analysis::IndirectTargetPointerProvenanceKind::GuestLoad;
    result.guest_load_address = 0x8000U + target;
    return result;
}

[[nodiscard]] analysis::IndirectTargetAssessment assessment(
    const ObservedIndirectTarget& item, bool eligible = true)
{
    analysis::IndirectTargetAssessment result;
    result.observed = item;
    result.validation.target_module = item.target_module;
    result.validation.structurally_eligible = true;
    result.decision.kind = eligible ? IndirectTargetDecisionKind::TrustedNewEntry
                                    : IndirectTargetDecisionKind::InsufficientEvidence;
    result.decision.eligible_for_promotion = eligible;
    return result;
}

void promote(IndirectTargetRefinementWorklist& worklist,
             const analysis::IndirectTargetCandidateIdentity& identity,
             const analysis::IndirectTargetRefinementAnalysisWork& work = {})
{
    REQUIRE(worklist.begin_candidate_assessment(identity));
    REQUIRE(worklist.can_commit_refinement(identity, work));
    worklist.record_promotion(identity, 1U, 0U, work);
}

[[nodiscard]] std::string rendered_worklist(const std::vector<GuestAddress>& order)
{
    IndirectTargetRefinementBudgets budgets;
    budgets.candidate_universe.executable_instruction_slots = order.size();
    IndirectTargetRefinementWorklist worklist(budgets);
    for (const auto target : order) (void)worklist.observe(assessment(observed(target)));

    execution::ExecutionSessionResult result;
    result.indirect_target_refinement = worklist.summary();
    return execution::render_execution_report_json(result);
}

} // namespace

TEST_CASE("M32 ordinary semantic accounting crosses the former 512 assessment frontier")
{
    // 514 executable slots provide 513 actual candidate/refinement operations.
    // Each operation goes through structural assessment, CFG/function-map
    // refinement, immutable publication, and worklist accounting.
    constexpr std::size_t candidate_count = 514U;
    constexpr GuestAddress base = 0x100000U;
    auto module = make_ret_module("synthetic-m32", base, candidate_count);
    REQUIRE(module);
    const std::array<ModuleIdentity, 1U> identities{module.value().identity};
    const auto universe = analysis::derive_indirect_target_candidate_universe(
        std::span<const ModuleIdentity>(identities.data(), identities.size()));
    REQUIRE(universe);
    REQUIRE(universe.value().executable_instruction_slots == candidate_count);

    const std::vector<FunctionSeed> roots{
        seed(base, FunctionDiscoverySource::ModuleEntry, FunctionConfidence::Confirmed, "root")};
    const auto input = analysis::ModuleAnalysisInput{module.value().identity, &module.value().memory,
                                                     roots};
    const auto initial_map = analysis::FunctionMapBuilder::build(input);
    REQUIRE(initial_map);
    auto map = initial_map.value();

    IndirectTargetRefinementBudgets budgets;
    budgets.candidate_universe = universe.value();
    // Preserve the historical M32 compatibility-boundary assertion. M33's
    // ordinary profile leaves this ceiling disengaged; the dedicated M33
    // suite proves that the same production workload crosses 512.
    budgets.analysis.max_transactions = 512U;
    IndirectTargetRefinementWorklist worklist(budgets);
    analysis::IndirectTargetDiscoveryOptions options;

    for (std::size_t index = 1U; index < candidate_count; ++index)
    {
        REQUIRE(worklist.begin_round());
        const auto item = observed(base + index * 4U);
        const auto assessed = analysis::assess_indirect_target(
            item, module.value().memory, &map, nullptr, nullptr, options);
        REQUIRE(assessed);
        REQUIRE(assessed.value().validation.structurally_eligible);
        REQUIRE(assessed.value().decision.kind == IndirectTargetDecisionKind::TrustedNewEntry);
        REQUIRE(worklist.observe(assessed.value()).newly_unique_candidate);
        const auto pending = worklist.pending_candidates();
        REQUIRE(pending.size() == 1U);
        const auto identity = analysis::indirect_target_candidate_identity(pending.front());
        REQUIRE(worklist.begin_candidate_assessment(identity));
        const auto refined = analysis::refine_function_map(
            map, input, item, options);
        REQUIRE(refined);
        REQUIRE(refined.value().assessment.decision.promoted);
        if (!worklist.can_commit_refinement(identity, refined.value().analysis_work))
        {
            // The first 512 refinements consume the existing aggregate
            // transaction resource. The 513th candidate is still fully
            // assessed through CFG/refinement, but its immutable publication
            // is correctly refused at that independent frontier.
            REQUIRE(index == 513U);
            worklist.record_rollback_assessment(identity);
            worklist.end_round();
            break;
        }
        map = std::move(refined.value().map);
        worklist.record_promotion(identity, 1U, 0U, refined.value().analysis_work);
        worklist.end_round();
    }

    const auto summary = worklist.summary();
    REQUIRE_FALSE(summary.configured.max_candidate_assessments.has_value());
    REQUIRE(summary.unique_candidates == candidate_count - 1U);
    REQUIRE(summary.candidate_records == candidate_count - 1U);
    REQUIRE(summary.first_candidate_assessments == candidate_count - 1U);
    REQUIRE(summary.generation_reassessments == 0U);
    REQUIRE(summary.candidate_assessments == candidate_count - 1U);
    REQUIRE(summary.successful_promotions == 512U);
    REQUIRE(summary.map_generation == 512U);
    REQUIRE(summary.pending_candidate_count == 1U);
    REQUIRE(summary.rollback_assessments == 1U);
    REQUIRE(summary.exhaustion.dimension ==
            IndirectTargetRefinementBudgetDimension::RefinementAnalysisTransactions);
    REQUIRE(summary.exhaustion.consumed == 512U);
    REQUIRE(summary.exhaustion.limit == 512U);
    REQUIRE(summary.analysis.transactions <= summary.analysis.configured.max_transactions.value());
    REQUIRE(map.find_exact_entry(base + 512U * 4U) != nullptr);
    REQUIRE(map.find_exact_entry(base + 513U * 4U) == nullptr);
}

TEST_CASE("M32 duplicate observations preserve sparse records and do not charge first assessment")
{
    IndirectTargetRefinementBudgets budgets;
    budgets.candidate_universe.executable_instruction_slots = 8U;
    IndirectTargetRefinementWorklist worklist(budgets);
    const auto first = assessment(observed(0x2000U));
    const auto second = assessment(observed(0x2004U));
    REQUIRE(worklist.observe(first).newly_unique_candidate);
    REQUIRE(worklist.observe(first).newly_unique_observation == false);
    REQUIRE(worklist.observe(second).newly_unique_candidate);

    const auto summary = worklist.summary();
    REQUIRE(summary.observations_received == 3U);
    REQUIRE(summary.unique_observations == 2U);
    REQUIRE(summary.duplicate_coalesced_observations == 1U);
    REQUIRE(summary.unique_candidates == 2U);
    REQUIRE(summary.candidate_records == 2U);
    REQUIRE(summary.candidate_assessments == 0U);
    REQUIRE(summary.first_candidate_assessments == 0U);
    REQUIRE(summary.pending_candidate_count == 2U);
}

TEST_CASE("M32 one candidate cannot be assessed twice in one immutable generation")
{
    IndirectTargetRefinementWorklist worklist;
    const auto item = observed(0x3000U);
    const auto identity = analysis::indirect_target_candidate_identity(item);
    REQUIRE(worklist.observe(assessment(item)).accepted);
    REQUIRE(worklist.begin_candidate_assessment(identity));
    REQUIRE_FALSE(worklist.begin_candidate_assessment(identity));
    const auto summary = worklist.summary();
    REQUIRE(summary.candidate_assessments == 1U);
    REQUIRE(summary.first_candidate_assessments == 1U);
    REQUIRE(summary.generation_reassessments == 0U);
    REQUIRE(summary.same_generation_assessment_attempts == 1U);
    REQUIRE(summary.pending_candidate_count == 1U);
    REQUIRE(worklist.pending_candidates().empty());
    REQUIRE(summary.exhaustion.dimension == IndirectTargetRefinementBudgetDimension::None);
}

TEST_CASE("M32 reassessment is charged only after a published map generation change")
{
    IndirectTargetRefinementWorklist worklist;
    const auto item = observed(0x4000U);
    const auto identity = analysis::indirect_target_candidate_identity(item);
    REQUIRE(worklist.observe(assessment(item)).accepted);
    promote(worklist, identity);
    REQUIRE(worklist.summary().map_generation == 1U);

    const auto reconsidered = worklist.observe(assessment(item));
    REQUIRE(reconsidered.reconsidered_after_map_change);
    REQUIRE(worklist.summary().candidates_reconsidered_after_map_change == 1U);
    REQUIRE(worklist.summary().generation_reassessments == 0U);
    REQUIRE(worklist.pending_candidates().size() == 1U);
    REQUIRE(worklist.begin_candidate_assessment(identity));
    REQUIRE_FALSE(worklist.begin_candidate_assessment(identity));
    const auto summary = worklist.summary();
    REQUIRE(summary.candidate_assessments == 2U);
    REQUIRE(summary.first_candidate_assessments == 1U);
    REQUIRE(summary.generation_reassessments == 1U);
    REQUIRE(summary.same_generation_assessment_attempts == 1U);
}

TEST_CASE("M32 terminal and trusted-existing resolutions remove pending work")
{
    SECTION("terminal rejection")
    {
        IndirectTargetRefinementWorklist worklist;
        const auto item = observed(0x5000U);
        const auto identity = analysis::indirect_target_candidate_identity(item);
        REQUIRE(worklist.observe(assessment(item)).accepted);
        REQUIRE(worklist.begin_candidate_assessment(identity));
        worklist.record_terminal_candidate(identity);
        const auto summary = worklist.summary();
        REQUIRE(summary.candidate_assessments == 1U);
        REQUIRE(summary.terminal_resolutions == 1U);
        REQUIRE(summary.pending_candidate_count == 0U);
        REQUIRE(worklist.pending_candidates().empty());
    }

    SECTION("trusted existing")
    {
        IndirectTargetRefinementWorklist worklist;
        const auto item = observed(0x5004U);
        REQUIRE(worklist.observe(assessment(item)).accepted);
        auto trusted = assessment(item);
        trusted.decision.kind = IndirectTargetDecisionKind::TrustedExistingEntry;
        trusted.decision.eligible_for_promotion = false;
        REQUIRE(worklist.observe(trusted).accepted);
        const auto summary = worklist.summary();
        REQUIRE(summary.existing_trusted_hits == 1U);
        REQUIRE(summary.terminal_resolutions == 1U);
        REQUIRE(summary.candidate_assessments == 0U);
        REQUIRE(summary.pending_candidate_count == 0U);
        REQUIRE(worklist.pending_candidates().empty());
    }
}

TEST_CASE("M32 aggregate exhaustion rolls back without changing the published map")
{
    constexpr GuestAddress base = 0x6000U;
    auto module = make_ret_module("synthetic-m32-rollback", base, 3U);
    REQUIRE(module);
    const std::vector<FunctionSeed> roots{
        seed(base, FunctionDiscoverySource::ModuleEntry, FunctionConfidence::Confirmed, "root")};
    const auto input = analysis::ModuleAnalysisInput{module.value().identity, &module.value().memory,
                                                     roots};
    auto published = analysis::FunctionMapBuilder::build(input);
    REQUIRE(published);
    const auto before = published.value();

    IndirectTargetRefinementBudgets budgets;
    budgets.candidate_universe.executable_instruction_slots = 3U;
    budgets.analysis.max_transactions = 0U;
    IndirectTargetRefinementWorklist worklist(budgets);
    analysis::IndirectTargetDiscoveryOptions options;
    auto item = observed(base + 4U);
    item.source_module = module.value().identity.module;
    item.target_module = module.value().identity.module;
    const auto identity = analysis::indirect_target_candidate_identity(item);
    REQUIRE(worklist.observe(assessment(item)).accepted);
    REQUIRE(worklist.begin_candidate_assessment(identity));
    const auto refined = analysis::refine_function_map(published.value(), input, item, options);
    REQUIRE(refined);
    REQUIRE(refined.value().assessment.decision.promoted);
    REQUIRE_FALSE(worklist.can_commit_refinement(identity, refined.value().analysis_work));
    worklist.record_rollback_assessment(identity);

    const auto summary = worklist.summary();
    REQUIRE(summary.exhaustion.dimension ==
            IndirectTargetRefinementBudgetDimension::RefinementAnalysisTransactions);
    REQUIRE(summary.rollback_assessments == 1U);
    REQUIRE(summary.map_generation == 0U);
    REQUIRE(summary.pending_candidate_count == 1U);
    REQUIRE(before.frozen());
    REQUIRE(before.find_exact_entry(base + 4U) == nullptr);
    REQUIRE(published.value().functions().size() == before.functions().size());
    REQUIRE(published.value().find_exact_entry(base + 4U) == nullptr);
}

TEST_CASE("M32 explicit legacy assessment ceilings remain exact and typed")
{
    IndirectTargetRefinementBudgets budgets;
    budgets.candidate_universe.executable_instruction_slots = 8U;
    budgets.max_candidate_assessments = 2U;
    IndirectTargetRefinementWorklist worklist(budgets);
    REQUIRE(worklist.observe(assessment(observed(0x7000U))).accepted);
    REQUIRE(worklist.observe(assessment(observed(0x7004U))).accepted);
    REQUIRE(worklist.observe(assessment(observed(0x7008U))).accepted);

    for (std::size_t index = 0U; index < 2U; ++index)
    {
        const auto pending = worklist.pending_candidates();
        REQUIRE_FALSE(pending.empty());
        promote(worklist, analysis::indirect_target_candidate_identity(pending.front()));
    }
    const auto pending = worklist.pending_candidates();
    REQUIRE(pending.size() == 1U);
    const auto identity = analysis::indirect_target_candidate_identity(pending.front());
    REQUIRE_FALSE(worklist.begin_candidate_assessment(identity));
    const auto summary = worklist.summary();
    REQUIRE(summary.candidate_assessments == 2U);
    REQUIRE(summary.first_candidate_assessments == 2U);
    REQUIRE(summary.generation_reassessments == 0U);
    REQUIRE(summary.exhaustion.dimension ==
            IndirectTargetRefinementBudgetDimension::CandidateAssessments);
    REQUIRE(summary.exhaustion.consumed == 2U);
    REQUIRE(summary.exhaustion.limit == 2U);
    REQUIRE(summary.exhaustion.next_work == identity);
    REQUIRE(summary.configured.candidate_assessment_limit_provenance.kind ==
            analysis::AnalysisBudgetProvenanceKind::ExplicitApiOverride);
}

TEST_CASE("M32 finite candidate and reconsideration cycles stop at an existing aggregate resource")
{
    IndirectTargetRefinementBudgets budgets;
    budgets.candidate_universe.executable_instruction_slots = 1U;
    budgets.analysis.max_transactions = 3U;
    IndirectTargetRefinementWorklist worklist(budgets);
    const auto item = observed(0x8000U);
    const auto identity = analysis::indirect_target_candidate_identity(item);
    const analysis::IndirectTargetRefinementAnalysisWork work{"synthetic-m32", 0U, 0U, 0U,
                                                               0U, 0U, 0U, 0U, 0U, 0U, 1U};

    for (std::size_t generation = 0U; generation < 3U; ++generation)
    {
        REQUIRE(worklist.observe(assessment(item)).accepted);
        REQUIRE(worklist.begin_candidate_assessment(identity));
        REQUIRE(worklist.can_commit_refinement(identity, work));
        worklist.record_promotion(identity, 1U, 0U, work);
    }
    REQUIRE(worklist.observe(assessment(item)).accepted);
    REQUIRE(worklist.begin_candidate_assessment(identity));
    REQUIRE_FALSE(worklist.can_commit_refinement(identity, work));
    worklist.record_rollback_assessment(identity);

    const auto summary = worklist.summary();
    REQUIRE(summary.map_generation == 3U);
    REQUIRE(summary.first_candidate_assessments == 1U);
    REQUIRE(summary.generation_reassessments == 3U);
    REQUIRE(summary.candidate_assessments == 4U);
    REQUIRE(summary.candidates_reconsidered_after_map_change == 3U);
    REQUIRE(summary.successful_promotions == 3U);
    REQUIRE(summary.rollback_assessments == 1U);
    REQUIRE(summary.exhaustion.dimension ==
            IndirectTargetRefinementBudgetDimension::RefinementAnalysisTransactions);
    REQUIRE(summary.exhaustion.consumed == 3U);
    REQUIRE(summary.exhaustion.limit == 3U);
    REQUIRE(summary.pending_candidate_count == 1U);
}

TEST_CASE("M32 checked accounting rejects counter overflow without wrapping")
{
    IndirectTargetRefinementWorklist worklist;
    auto item = observed(0x9000U);
    item.observation_count = std::numeric_limits<std::size_t>::max();
    REQUIRE(worklist.observe(assessment(item)).accepted);
    auto duplicate = item;
    duplicate.observation_count = 1U;
    REQUIRE_FALSE(worklist.observe(assessment(duplicate)).accepted);
    const auto summary = worklist.summary();
    REQUIRE(summary.observations_received == std::numeric_limits<std::size_t>::max());
    REQUIRE(summary.exhaustion.dimension == IndirectTargetRefinementBudgetDimension::CounterOverflow);
    REQUIRE(summary.exhaustion.consumed == std::numeric_limits<std::size_t>::max());
    REQUIRE(summary.exhaustion.limit == std::numeric_limits<std::size_t>::max());
    REQUIRE(summary.pending_candidate_count == 1U);
}

TEST_CASE("M32 assessment reports are deterministic and expose the semantic model")
{
    const std::vector<GuestAddress> first{0xa00cU, 0xa000U, 0xa008U, 0xa004U};
    const auto second = std::vector<GuestAddress>{0xa004U, 0xa008U, 0xa000U, 0xa00cU};
    const auto first_report = rendered_worklist(first);
    const auto second_report = rendered_worklist(second);
    REQUIRE(first_report == second_report);
    REQUIRE(execution::ExecutionSessionResult::schema_version == 19U);
    REQUIRE(first_report.find("\"schema_version\"") != std::string::npos);
    REQUIRE(first_report.find("\"assessment_accounting\"") != std::string::npos);
    REQUIRE(first_report.find("structural_candidate_generation_v1") != std::string::npos);
    REQUIRE(first_report.find("legacy_assessment_limit_configured") != std::string::npos);
    REQUIRE(first_report.find("first_time_assessments") != std::string::npos);
}
