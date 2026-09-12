#include "switchrecomp/analysis/indirect_target.hpp"
#include "switchrecomp/analysis/process_image.hpp"
#include "switchrecomp/execution/session.hpp"
#include "switchrecomp/version.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace switchrecomp;
using analysis::FunctionConfidence;
using analysis::FunctionDiscoverySource;
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
    result.source_module = "synthetic-m26";
    result.source_function = 0x0f00U;
    result.source_pc = source_pc;
    result.control_flow = IndirectControlFlowKind::Call;
    result.target_register = "x8";
    result.target = target;
    result.target_module = "synthetic-m26";
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
    result.decision.kind = eligible ? IndirectTargetDecisionKind::TrustedNewEntry
                                    : IndirectTargetDecisionKind::InsufficientEvidence;
    result.decision.eligible_for_promotion = eligible;
    result.validation.structurally_eligible = true;
    return result;
}

void promote_next(IndirectTargetRefinementWorklist& worklist,
                  std::size_t rebuilt = 0U, std::size_t reused = 0U)
{
    const auto pending = worklist.pending_candidates();
    REQUIRE_FALSE(pending.empty());
    const auto identity = analysis::indirect_target_candidate_identity(pending.front());
    REQUIRE(worklist.begin_candidate_assessment(identity));
    REQUIRE(worklist.can_promote());
    worklist.record_promotion(identity, rebuilt, reused);
}

TEST_CASE("M26 more than 64 productive promotions are not a round-limit failure")
{
    IndirectTargetRefinementWorklist worklist;
    constexpr std::size_t promotion_count = 96U;
    for (std::size_t index = 0U; index < promotion_count; ++index)
    {
        REQUIRE(worklist.begin_round());
        REQUIRE(worklist.observe(assessment(observed(0x2000U + index * 4U))).accepted);
        promote_next(worklist, 1U, 2U);
        worklist.end_round();
    }

    const auto summary = worklist.summary();
    REQUIRE(summary.total_execution_attempts == promotion_count);
    REQUIRE(summary.productive_rounds == promotion_count);
    REQUIRE(summary.stagnant_rounds == 0U);
    REQUIRE(summary.unique_candidates == promotion_count);
    REQUIRE(summary.candidate_assessments == promotion_count);
    REQUIRE(summary.successful_promotions == promotion_count);
    REQUIRE(summary.map_rebuilds == promotion_count);
    REQUIRE(summary.module_maps_rebuilt == promotion_count);
    REQUIRE(summary.module_maps_reused == promotion_count * 2U);
    REQUIRE(summary.exhaustion.dimension == IndirectTargetRefinementBudgetDimension::None);
}

TEST_CASE("M26 genuine no-progress work exhausts the exact stagnation limit")
{
    IndirectTargetRefinementBudgets budgets;
    budgets.max_stagnant_rounds = 3U;
    IndirectTargetRefinementWorklist worklist(budgets);
    for (std::size_t index = 0U; index < budgets.max_stagnant_rounds; ++index)
    {
        REQUIRE(worklist.begin_round());
        worklist.end_round();
    }
    REQUIRE_FALSE(worklist.begin_round());

    const auto summary = worklist.summary();
    REQUIRE(summary.total_execution_attempts == 3U);
    REQUIRE(summary.productive_rounds == 0U);
    REQUIRE(summary.stagnant_rounds == 3U);
    REQUIRE(summary.exhaustion.dimension ==
            IndirectTargetRefinementBudgetDimension::StagnantRounds);
    REQUIRE(summary.exhaustion.consumed == 3U);
    REQUIRE(summary.exhaustion.limit == 3U);
    REQUIRE(summary.pending_candidate_count == 0U);
}

TEST_CASE("M26 productive progress resets the stagnation charge")
{
    IndirectTargetRefinementBudgets budgets;
    budgets.max_stagnant_rounds = 2U;
    IndirectTargetRefinementWorklist worklist(budgets);

    REQUIRE(worklist.begin_round());
    worklist.end_round();
    REQUIRE(worklist.begin_round());
    REQUIRE(worklist.observe(assessment(observed(0x3000U))).newly_unique_candidate);
    promote_next(worklist);
    worklist.end_round();
    REQUIRE(worklist.begin_round());
    worklist.end_round();
    REQUIRE(worklist.begin_round());
    worklist.end_round();

    const auto summary = worklist.summary();
    REQUIRE(summary.productive_rounds == 1U);
    REQUIRE(summary.stagnant_rounds == 2U);
    REQUIRE(summary.exhaustion.dimension ==
            IndirectTargetRefinementBudgetDimension::StagnantRounds);
    REQUIRE(summary.exhaustion.consumed == 2U);
    REQUIRE(summary.exhaustion.limit == 2U);
}

TEST_CASE("M26 terminal rejection is fail-closed and does not remain pending")
{
    IndirectTargetRefinementWorklist worklist;
    REQUIRE(worklist.begin_round());
    REQUIRE(worklist.observe(assessment(observed(0x4000U), false)).accepted);
    worklist.end_round();

    const auto summary = worklist.summary();
    REQUIRE(summary.rejected_candidates == 1U);
    REQUIRE(summary.terminal_resolutions == 1U);
    REQUIRE(summary.successful_promotions == 0U);
    REQUIRE(summary.pending_candidate_count == 0U);
    REQUIRE(summary.productive_rounds == 1U);
}

TEST_CASE("M26 duplicate observations preserve provenance without artificial progress")
{
    IndirectTargetRefinementWorklist worklist;
    const auto item = assessment(observed(0x5000U));
    REQUIRE(worklist.begin_round());
    REQUIRE(worklist.observe(item).newly_unique_candidate);
    REQUIRE_FALSE(worklist.observe(item).newly_unique_candidate);
    REQUIRE_FALSE(worklist.observe(item).newly_unique_observation);
    promote_next(worklist, 1U, 1U);
    worklist.end_round();

    const auto summary = worklist.summary();
    REQUIRE(summary.unique_candidates == 1U);
    REQUIRE(summary.candidate_assessments == 1U);
    REQUIRE(summary.successful_promotions == 1U);
    REQUIRE(summary.map_rebuilds == 1U);
    REQUIRE(summary.module_maps_rebuilt == 1U);
    REQUIRE(summary.module_maps_reused == 1U);
    REQUIRE(summary.duplicate_coalesced_observations == 2U);
    REQUIRE(summary.productive_rounds == 1U);
}

TEST_CASE("M26 map changes reconsider only the observed candidate")
{
    IndirectTargetRefinementWorklist worklist;
    const auto first = assessment(observed(0x6000U));
    const auto unrelated = assessment(observed(0x6010U));
    REQUIRE(worklist.observe(first).newly_unique_candidate);
    REQUIRE(worklist.observe(unrelated).newly_unique_candidate);
    promote_next(worklist);
    promote_next(worklist);
    const auto reconsidered = worklist.observe(first);
    REQUIRE(reconsidered.reconsidered_after_map_change);
    REQUIRE(worklist.summary().candidates_reconsidered_after_map_change == 1U);
    REQUIRE(worklist.pending_candidates().size() == 1U);
    REQUIRE(worklist.pending_candidates().front().target == 0x6000U);
}

[[nodiscard]] std::string serialized_worklist(std::vector<ObservedIndirectTarget> items)
{
    IndirectTargetRefinementWorklist worklist;
    REQUIRE(worklist.begin_round());
    for (const auto& item : items) (void)worklist.observe(assessment(item));
    while (!worklist.pending_candidates().empty()) promote_next(worklist);
    worklist.end_round();
    execution::ExecutionSessionResult report;
    report.indirect_target_refinement = worklist.summary();
    return execution::render_execution_report_json(report);
}

TEST_CASE("M26 candidate permutations produce identical serialized state")
{
    std::vector<ObservedIndirectTarget> first{observed(0x7010U, 0x1010U),
                                              observed(0x7000U, 0x1000U),
                                              observed(0x7008U, 0x1008U)};
    auto second = first;
    std::reverse(second.begin(), second.end());
    REQUIRE(serialized_worklist(std::move(first)) == serialized_worklist(std::move(second)));
}

[[nodiscard]] std::vector<std::byte> words(std::initializer_list<std::uint32_t> values)
{
    std::vector<std::byte> result;
    result.reserve(values.size() * 4U);
    for (const auto value : values)
    {
        result.push_back(static_cast<std::byte>(value & 0xffU));
        result.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
        result.push_back(static_cast<std::byte>((value >> 16U) & 0xffU));
        result.push_back(static_cast<std::byte>((value >> 24U) & 0xffU));
    }
    return result;
}

void write_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value)
{
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[offset + 2U] = static_cast<std::byte>((value >> 16U) & 0xffU);
    bytes[offset + 3U] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

[[nodiscard]] std::vector<std::byte> minimal_nso()
{
    constexpr std::size_t text_file = 0x100U;
    constexpr std::size_t rodata_file = 0x200U;
    constexpr std::size_t data_file = 0x300U;
    std::vector<std::byte> file(0x310U, std::byte{0});
    file[0] = std::byte{'N'};
    file[1] = std::byte{'S'};
    file[2] = std::byte{'O'};
    file[3] = std::byte{'0'};
    write_u32(file, 0x10U, text_file);
    write_u32(file, 0x18U, 0x20U);
    write_u32(file, 0x20U, rodata_file);
    write_u32(file, 0x24U, 0x100U);
    write_u32(file, 0x28U, 0x10U);
    write_u32(file, 0x30U, data_file);
    write_u32(file, 0x34U, 0x200U);
    write_u32(file, 0x38U, 0x10U);
    write_u32(file, 0x60U, 0x20U);
    write_u32(file, 0x64U, 0x10U);
    write_u32(file, 0x68U, 0x10U);
    const auto code = words({0xd65f03c0U, 0xd65f03c0U});
    std::copy(code.begin(), code.end(), file.begin() + static_cast<std::ptrdiff_t>(text_file + 8U));
    return file;
}

[[nodiscard]] analysis::ObservedIndirectTarget process_target(GuestAddress target,
                                                               GuestAddress base)
{
    auto result = observed(target, base + 8U);
    result.source_module = "main";
    result.source_function = base + 8U;
    result.target_module = "main";
    result.pointer_provenance = analysis::IndirectTargetPointerProvenanceKind::Unknown;
    result.guest_load_address.reset();
    return result;
}

struct MinimalProcessFixture
{
    analysis::ProcessImage image;
    analysis::ProcessFunctionMap map;
};

[[nodiscard]] Result<MinimalProcessFixture> minimal_process_fixture()
{
    const auto bytes = minimal_nso();
    const std::array<analysis::ProcessModuleInput, 2> inputs{
        analysis::ProcessModuleInput{"main", bytes, 0x100000U},
        analysis::ProcessModuleInput{"provider", bytes, 0x200000U}};
    analysis::ProcessImageOptions image_options;
    image_options.primary_module = "main";
    image_options.provider_search_complete = true;
    image_options.module_options.seed_text_entry = false;
    const auto image = analysis::load_process_image(inputs, image_options);
    if (!image) return Result<MinimalProcessFixture>::failure(image.error());

    std::vector<analysis::FinalizedFunctionMap> maps;
    for (const auto& module : image.value().modules())
    {
        auto identity = module.identity;
        const auto entry = module.identity.guest_base + 8U;
        identity.entry_points.push_back({entry, analysis::EntryPointKind::DynamicInit,
                                         "synthetic M36 entry", FunctionConfidence::High, false,
                                         "synthetic entry"});
        auto seeds = module.seeds;
        seeds.push_back({entry, FunctionDiscoverySource::ModuleEntry,
                         FunctionConfidence::Confirmed, std::nullopt, std::nullopt,
                         "synthetic M36 entry"});
        const auto map = analysis::FunctionMapBuilder::build(
            analysis::ModuleAnalysisInput{std::move(identity), &image.value().memory(),
                                          std::move(seeds)});
        if (!map) return Result<MinimalProcessFixture>::failure(map.error());
        maps.push_back(std::move(map).value());
    }
    const auto process = analysis::ProcessFunctionMap::build(std::move(maps));
    if (!process) return Result<MinimalProcessFixture>::failure(process.error());
    return Result<MinimalProcessFixture>::success(
        MinimalProcessFixture{std::move(image).value(), std::move(process).value()});
}

TEST_CASE("M26 target-only map refinement reuses unchanged frozen modules transactionally")
{
    const auto bytes = minimal_nso();
    const std::array<analysis::ProcessModuleInput, 2> inputs{
        analysis::ProcessModuleInput{"main", bytes, 0x100000U},
        analysis::ProcessModuleInput{"provider", bytes, 0x200000U}};
    analysis::ProcessImageOptions image_options;
    image_options.primary_module = "main";
    image_options.provider_search_complete = true;
    image_options.module_options.seed_text_entry = false;
    const auto image = analysis::load_process_image(inputs, image_options);
    REQUIRE(image);

    std::vector<analysis::FinalizedFunctionMap> maps;
    for (const auto& module : image.value().modules())
    {
        auto identity = module.identity;
        const auto entry = module.identity.guest_base + 8U;
        identity.entry_points.push_back({entry, analysis::EntryPointKind::DynamicInit,
                                         "synthetic M26 entry", FunctionConfidence::High, false,
                                         "synthetic entry"});
        auto seeds = module.seeds;
        seeds.push_back({entry, FunctionDiscoverySource::ModuleEntry,
                         FunctionConfidence::Confirmed, std::nullopt, std::nullopt,
                         "synthetic M26 entry"});
        const auto map = analysis::FunctionMapBuilder::build(
            analysis::ModuleAnalysisInput{std::move(identity), &image.value().memory(),
                                          std::move(seeds)});
        REQUIRE(map);
        maps.push_back(std::move(map).value());
    }
    const auto process = analysis::ProcessFunctionMap::build(std::move(maps));
    REQUIRE(process);

    std::vector<analysis::FunctionRecord> provider_before;
    const analysis::FinalizedFunctionMap* provider_map_before = nullptr;
    for (const auto& map : process.value().maps())
        if (map.identity().module == "provider")
        {
            provider_map_before = &map;
            provider_before = map.functions();
        }
    REQUIRE(provider_map_before != nullptr);

    analysis::IndirectTargetDiscoveryOptions options;
    const auto refined = analysis::refine_process_function_map(
        process.value(), image.value(), process_target(0x100000U + 0x0cU, 0x100000U), options);
    REQUIRE(refined);
    REQUIRE(refined.value().assessment.decision.promoted);
    REQUIRE(refined.value().module_maps_rebuilt == 1U);
    REQUIRE(refined.value().module_maps_reused == 1U);
    REQUIRE(refined.value().map.maps().size() == 2U);
    REQUIRE(refined.value().map.find(0x100000U + 0x0cU) != nullptr);
    for (const auto& map : refined.value().map.maps())
        if (map.identity().module == "provider")
        {
            REQUIRE(&map == provider_map_before);
            REQUIRE(map.functions().size() == provider_before.size());
            for (std::size_t index = 0U; index < provider_before.size(); ++index)
            {
                REQUIRE(map.functions()[index].canonical_entry ==
                        provider_before[index].canonical_entry);
                REQUIRE(map.functions()[index].owned_code_ranges ==
                        provider_before[index].owned_code_ranges);
            }
        }

    const auto main_assessment = analysis::assess_indirect_target(
        process_target(0x100000U + 0x0cU, 0x100000U), image.value().memory(), nullptr,
        &process.value(), &image.value());
    REQUIRE(main_assessment);
    auto provider_observed = process_target(0x200000U + 0x0cU, 0x100000U);
    provider_observed.target_module.clear();
    const auto provider_assessment = analysis::assess_indirect_target(
        provider_observed, image.value().memory(), nullptr, &process.value(), &image.value());
    REQUIRE(provider_assessment);
    REQUIRE(main_assessment.value().decision.eligible_for_promotion);
    REQUIRE(provider_assessment.value().decision.eligible_for_promotion);
    const std::array<analysis::IndirectTargetAssessment, 2U> batch_assessments{
        main_assessment.value(), provider_assessment.value()};
    const auto batched = analysis::refine_process_function_map_batch(
        process.value(), image.value(), batch_assessments);
    REQUIRE(batched);
    REQUIRE(batched.value().all_promoted);
    REQUIRE(batched.value().module_maps_rebuilt == 2U);
    REQUIRE(batched.value().module_maps_reused == 0U);
    REQUIRE(batched.value().map.find(0x100000U + 0x0cU) != nullptr);
    REQUIRE(batched.value().map.find(0x200000U + 0x0cU) != nullptr);

    const auto failed = analysis::refine_process_function_map(
        process.value(), image.value(), process_target(0xdead0000U, 0x100000U), options);
    REQUIRE(failed);
    REQUIRE_FALSE(failed.value().assessment.decision.promoted);
    REQUIRE(failed.value().module_maps_rebuilt == 0U);
    REQUIRE(failed.value().module_maps_reused == 0U);
    REQUIRE(failed.value().map.maps().size() == process.value().maps().size());
    REQUIRE(failed.value().map.find(0x100000U + 0x0cU) == nullptr);
}

TEST_CASE("M36 worker counts produce byte-identical synthetic assessments")
{
    const auto fixture = minimal_process_fixture();
    REQUIRE(fixture);
    auto main_candidate = process_target(0x100000U + 0x0cU, 0x100000U);
    main_candidate.target_module.clear();
    auto provider_candidate = process_target(0x200000U + 0x0cU, 0x100000U);
    provider_candidate.target_module.clear();
    const std::array<ObservedIndirectTarget, 2U> candidates{
        main_candidate, provider_candidate};
    analysis::IndirectTargetDiscoveryOptions options;
    const auto serial = analysis::assess_indirect_targets(
        candidates, fixture.value().image.memory(), fixture.value().map,
        fixture.value().image, options, 1U);
    const auto parallel = analysis::assess_indirect_targets(
        candidates, fixture.value().image.memory(), fixture.value().map,
        fixture.value().image, options, 2U);
    const auto higher_parallel = analysis::assess_indirect_targets(
        candidates, fixture.value().image.memory(), fixture.value().map,
        fixture.value().image, options, 4U);
    REQUIRE(serial);
    REQUIRE(parallel);
    REQUIRE(higher_parallel);
    const auto render = [](const auto& assessments) {
        execution::ExecutionSessionResult result;
        result.indirect_target_discovery = assessments;
        return execution::render_execution_report_json(result);
    };
    REQUIRE(render(serial.value()) == render(parallel.value()));
    REQUIRE(render(serial.value()) == render(higher_parallel.value()));
}

} // namespace
