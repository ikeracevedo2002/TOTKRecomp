#include "switchrecomp/analysis/indirect_target.hpp"
#include "switchrecomp/execution/session.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace
{

using namespace switchrecomp;
using analysis::IndirectControlFlowKind;
using analysis::IndirectTargetDecisionKind;
using analysis::IndirectTargetRefinementBudgetDimension;
using analysis::IndirectTargetRefinementBudgets;
using analysis::IndirectTargetRefinementWorklist;
using analysis::ObservedIndirectTarget;
using memory::GuestAddress;

[[nodiscard]] ObservedIndirectTarget observed(GuestAddress target,
                                              GuestAddress source_pc = 0x1000U)
{
    ObservedIndirectTarget result;
    result.source_module = "synthetic-m23";
    result.source_function = 0x0f00U;
    result.source_pc = source_pc;
    result.control_flow = IndirectControlFlowKind::Call;
    result.target_register = "x8";
    result.target = target;
    result.target_module = "synthetic-m23";
    result.pointer_provenance = analysis::IndirectTargetPointerProvenanceKind::GuestLoad;
    result.guest_load_address = 0x8000U + target;
    return result;
}

[[nodiscard]] analysis::IndirectTargetAssessment assessment(
    const ObservedIndirectTarget& observed_target, bool eligible = true)
{
    analysis::IndirectTargetAssessment result;
    result.observed = observed_target;
    result.decision.kind = eligible ? IndirectTargetDecisionKind::TrustedNewEntry
                                    : IndirectTargetDecisionKind::InsufficientEvidence;
    result.decision.eligible_for_promotion = eligible;
    result.validation.target_module = observed_target.target_module;
    return result;
}

void promote_next(IndirectTargetRefinementWorklist& worklist)
{
    const auto pending = worklist.pending_candidates();
    REQUIRE_FALSE(pending.empty());
    const auto identity = analysis::indirect_target_candidate_identity(pending.front());
    REQUIRE(worklist.begin_candidate_assessment(identity));
    REQUIRE(worklist.can_promote());
    worklist.record_promotion(identity);
}

} // namespace

TEST_CASE("M23 legacy eight-promotion workload remains finite and progresses")
{
    IndirectTargetRefinementWorklist worklist;
    REQUIRE(worklist.begin_round());
    for (GuestAddress target = 0x2000U; target < 0x2030U; target += 4U)
        REQUIRE(worklist.observe(assessment(observed(target))).accepted);

    for (std::size_t index = 0U; index < 12U; ++index) promote_next(worklist);

    const auto summary = worklist.summary();
    REQUIRE(summary.unique_candidates == 12U);
    REQUIRE(summary.successful_promotions == 12U);
    REQUIRE(summary.map_rebuilds == 12U);
    REQUIRE(summary.exhaustion.dimension == IndirectTargetRefinementBudgetDimension::None);
}

TEST_CASE("M23 duplicate observations coalesce without new capacity or rebuilds")
{
    IndirectTargetRefinementWorklist worklist;
    const auto item = assessment(observed(0x3000U));
    REQUIRE(worklist.observe(item).newly_unique_candidate);
    REQUIRE_FALSE(worklist.observe(item).newly_unique_observation);
    REQUIRE_FALSE(worklist.observe(item).newly_unique_observation);
    const auto before = worklist.summary();
    REQUIRE(before.observations_received == 3U);
    REQUIRE(before.unique_observations == 1U);
    REQUIRE(before.unique_candidates == 1U);
    REQUIRE(before.duplicate_coalesced_observations == 2U);
    REQUIRE(before.map_rebuilds == 0U);
    REQUIRE(before.pending_candidate_count == 1U);

    promote_next(worklist);
    const auto after = worklist.summary();
    REQUIRE(after.unique_candidates == 1U);
    REQUIRE(after.successful_promotions == 1U);
    REQUIRE(after.map_rebuilds == 1U);
}

TEST_CASE("M23 equivalent observation permutations normalize to one worklist")
{
    std::vector<ObservedIndirectTarget> left{observed(0x4010U, 0x1008U),
                                             observed(0x4000U, 0x1004U),
                                             observed(0x4000U, 0x1000U)};
    auto right = left;
    std::reverse(right.begin(), right.end());
    IndirectTargetRefinementWorklist first;
    IndirectTargetRefinementWorklist second;
    for (const auto& item : left) (void)first.observe(assessment(item));
    for (const auto& item : right) (void)second.observe(assessment(item));

    REQUIRE(first.pending_candidates() == second.pending_candidates());
    REQUIRE(first.summary().unique_candidates == second.summary().unique_candidates);
    REQUIRE(first.summary().unique_observations == second.summary().unique_observations);
    REQUIRE(first.summary().duplicate_coalesced_observations ==
            second.summary().duplicate_coalesced_observations);
    REQUIRE(first.summary().next_pending_candidate == second.summary().next_pending_candidate);
}

TEST_CASE("M23 trusted entries do not consume promotion capacity")
{
    IndirectTargetRefinementWorklist worklist;
    auto trusted = assessment(observed(0x5000U));
    trusted.decision.kind = IndirectTargetDecisionKind::TrustedExistingEntry;
    trusted.decision.eligible_for_promotion = false;
    for (std::size_t index = 0U; index < 20U; ++index)
        REQUIRE(worklist.observe(trusted).accepted);
    const auto summary = worklist.summary();
    REQUIRE(summary.existing_trusted_hits == 20U);
    REQUIRE(summary.unique_candidates == 0U);
    REQUIRE(summary.successful_promotions == 0U);
    REQUIRE(summary.pending_candidate_count == 0U);
}

TEST_CASE("M23 genuine candidate, promotion, and map-rebuild exhaustion remain typed")
{
    SECTION("candidate limit")
    {
        IndirectTargetRefinementBudgets budgets;
        budgets.max_unique_candidates = 2U;
        IndirectTargetRefinementWorklist worklist(budgets);
        REQUIRE(worklist.observe(assessment(observed(0x6000U))).accepted);
        REQUIRE(worklist.observe(assessment(observed(0x6004U))).accepted);
        REQUIRE_FALSE(worklist.observe(assessment(observed(0x6008U))).accepted);
        const auto summary = worklist.summary();
        REQUIRE(summary.exhaustion.dimension ==
                IndirectTargetRefinementBudgetDimension::UniqueCandidates);
        REQUIRE(summary.exhaustion.consumed == 2U);
        REQUIRE(summary.exhaustion.limit == 2U);
        REQUIRE(summary.pending_candidate_count == 3U);
    }

    SECTION("map rebuild limit")
    {
        IndirectTargetRefinementBudgets budgets;
        budgets.max_map_rebuilds = 1U;
        IndirectTargetRefinementWorklist worklist(budgets);
        REQUIRE(worklist.observe(assessment(observed(0x6100U))).accepted);
        REQUIRE(worklist.observe(assessment(observed(0x6104U))).accepted);
        promote_next(worklist);
        const auto pending = worklist.pending_candidates();
        REQUIRE(pending.size() == 1U);
        const auto identity = analysis::indirect_target_candidate_identity(pending.front());
        REQUIRE(worklist.begin_candidate_assessment(identity));
        REQUIRE_FALSE(worklist.can_promote());
        REQUIRE(worklist.summary().exhaustion.dimension ==
                IndirectTargetRefinementBudgetDimension::MapRebuilds);
    }

    SECTION("promotion limit")
    {
        IndirectTargetRefinementBudgets budgets;
        budgets.max_promotions = 1U;
        IndirectTargetRefinementWorklist worklist(budgets);
        REQUIRE(worklist.observe(assessment(observed(0x6150U))).accepted);
        REQUIRE(worklist.observe(assessment(observed(0x6154U))).accepted);
        promote_next(worklist);
        const auto pending = worklist.pending_candidates();
        REQUIRE(pending.size() == 1U);
        const auto identity = analysis::indirect_target_candidate_identity(pending.front());
        REQUIRE(worklist.begin_candidate_assessment(identity));
        REQUIRE_FALSE(worklist.can_promote());
        REQUIRE(worklist.summary().exhaustion.dimension ==
                IndirectTargetRefinementBudgetDimension::Promotions);
        REQUIRE(worklist.summary().exhaustion.consumed == 1U);
        REQUIRE(worklist.summary().exhaustion.limit == 1U);
    }

    SECTION("assessment limit")
    {
        IndirectTargetRefinementBudgets budgets;
        budgets.max_candidate_assessments = 1U;
        IndirectTargetRefinementWorklist worklist(budgets);
        REQUIRE(worklist.observe(assessment(observed(0x6200U))).accepted);
        REQUIRE(worklist.observe(assessment(observed(0x6204U))).accepted);
        promote_next(worklist);
        const auto pending = worklist.pending_candidates();
        REQUIRE(pending.size() == 1U);
        const auto identity = analysis::indirect_target_candidate_identity(pending.front());
        REQUIRE_FALSE(worklist.begin_candidate_assessment(identity));
        REQUIRE(worklist.summary().exhaustion.dimension ==
                IndirectTargetRefinementBudgetDimension::CandidateAssessments);
    }

    SECTION("round limit")
    {
        IndirectTargetRefinementBudgets budgets;
        budgets.max_stagnant_rounds = 1U;
        IndirectTargetRefinementWorklist worklist(budgets);
        REQUIRE(worklist.begin_round());
        worklist.end_round();
        REQUIRE_FALSE(worklist.begin_round());
        const auto summary = worklist.summary();
        REQUIRE(summary.exhaustion.dimension ==
                IndirectTargetRefinementBudgetDimension::StagnantRounds);
        REQUIRE(summary.exhaustion.consumed == 1U);
        REQUIRE(summary.exhaustion.limit == 1U);
    }
}

TEST_CASE("M23 ownership changes only requeue an observed candidate")
{
    IndirectTargetRefinementWorklist worklist;
    const auto item = assessment(observed(0x7000U));
    REQUIRE(worklist.observe(item).accepted);
    promote_next(worklist);
    REQUIRE(worklist.pending_candidates().empty());

    const auto reconsidered = worklist.observe(item);
    REQUIRE(reconsidered.reconsidered_after_map_change);
    REQUIRE(worklist.summary().candidates_reconsidered_after_map_change == 1U);
    REQUIRE(worklist.pending_candidates().size() == 1U);
}

TEST_CASE("M23 rejected certification remains terminal and fail closed")
{
    IndirectTargetRefinementWorklist worklist;
    REQUIRE(worklist.observe(assessment(observed(0x8000U), false)).accepted);
    const auto summary = worklist.summary();
    REQUIRE(summary.rejected_candidates == 1U);
    REQUIRE(summary.pending_candidate_count == 0U);
    REQUIRE(summary.successful_promotions == 0U);
    REQUIRE(summary.exhaustion.dimension == IndirectTargetRefinementBudgetDimension::None);
}

TEST_CASE("M23 refinement report is typed, provenance-preserving, and deterministic")
{
    auto item = assessment(observed(0x9000U, 0x1010U));
    item.observed.observation_provenance.push_back(ObservedIndirectTarget::Provenance{
        "synthetic-m23", 0x0f00U, 0x1010U, IndirectControlFlowKind::Call, "x8",
        analysis::IndirectTargetPointerProvenanceKind::GuestLoad, 0x1100U});
    execution::ExecutionSessionResult first;
    first.indirect_target_discovery.push_back(item);
    first.indirect_target_refinement.configured.max_stagnant_rounds = 64U;
    first.indirect_target_refinement.observations_received = 2U;
    first.indirect_target_refinement.unique_candidates = 1U;
    first.indirect_target_refinement.duplicate_coalesced_observations = 1U;
    first.indirect_target_refinement.exhaustion = {
        IndirectTargetRefinementBudgetDimension::CandidateAssessments, 4U, 4U};
    const auto second = first;
    const auto first_report = execution::render_execution_report_json(first);
    const auto second_report = execution::render_execution_report_json(second);
    REQUIRE(first_report == second_report);
    REQUIRE(first_report.find("\"schema_version\"") != std::string::npos);
    REQUIRE(first_report.find("\"configured_limits\"") != std::string::npos);
    REQUIRE(first_report.find("\"exhausted_dimension\"") != std::string::npos);
    REQUIRE(first_report.find("candidate_assessments") != std::string::npos);
    REQUIRE(first_report.find("\"observation_provenance\"") != std::string::npos);
}
