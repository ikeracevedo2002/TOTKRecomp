#include "switchrecomp/analysis/indirect_target.hpp"
#include "switchrecomp/analysis/function_map.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
using analysis::FunctionMapOptions;
using analysis::FunctionSeed;
using analysis::IndirectControlFlowKind;
using analysis::IndirectTargetCandidateUniverse;
using analysis::IndirectTargetDecisionKind;
using analysis::IndirectTargetRefinementBudgetDimension;
using analysis::IndirectTargetRefinementBudgets;
using analysis::IndirectTargetRefinementWorklist;
using analysis::ModuleIdentity;
using analysis::ObservedIndirectTarget;
using memory::GuestAddress;

constexpr std::uint32_t ret_instruction = 0xd65f03c0U;
constexpr std::uint32_t nop_instruction = 0xd503201fU;

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
        "synthetic.m28.text", memory::GuestRegionKind::Text);
    if (!mapped) return Result<SyntheticModule>::failure(mapped.error());
    result.identity.module = std::move(name);
    result.identity.build_id = "synthetic-m28-build";
    result.identity.input_sha256 = "synthetic-m28-sha";
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

[[nodiscard]] ObservedIndirectTarget observed(std::string module, GuestAddress target,
                                              GuestAddress source_pc = 0x1000U)
{
    ObservedIndirectTarget result;
    result.source_module = module;
    result.source_function = 0x0f00U;
    result.source_pc = source_pc;
    result.control_flow = IndirectControlFlowKind::Call;
    result.target_register = "x8";
    result.target = target;
    result.target_module = std::move(module);
    result.pointer_provenance = analysis::IndirectTargetPointerProvenanceKind::GuestLoad;
    result.guest_load_address = 0x8000U + target;
    return result;
}

[[nodiscard]] analysis::IndirectTargetAssessment synthetic_assessment(
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

TEST_CASE("M28 structural candidate universe counts executable slots without eager state")
{
    std::array<ModuleIdentity, 3U> modules{};
    modules[0].module = "main";
    modules[0].executable_ranges = {{0x1000U, 0x20U}, {0x3000U, 0U}};
    modules[1].module = "sdk";
    modules[1].executable_ranges = {{0x2000U, 0x08U}, {0x2008U, 0x10U}};
    modules[2].module = "subsdk0";
    modules[2].executable_ranges = {{0x5003U, 0x09U}};

    const auto universe = analysis::derive_indirect_target_candidate_universe(
        std::span<const ModuleIdentity>(modules.data(), modules.size()));
    REQUIRE(universe);
    // main: 8, sdk: 6, subsdk0: aligned slots 0x5004 and 0x5008.
    REQUIRE(universe.value().executable_instruction_slots == 16U);
    REQUIRE(universe.value().provenance.kind ==
            analysis::AnalysisBudgetProvenanceKind::DerivedStructuralBound);
    REQUIRE(universe.value().provenance.detail ==
            "process_image_executable_instruction_slots");

    IndirectTargetRefinementBudgets budgets;
    budgets.candidate_universe = universe.value();
    IndirectTargetRefinementWorklist worklist(budgets);
    for (GuestAddress target = 0x1000U; target < 0x1000U + 0x20U; target += 4U)
        REQUIRE(worklist.observe(synthetic_assessment(observed("main", target))).accepted);
    REQUIRE(worklist.summary().candidate_records == 8U);
    REQUIRE(worklist.summary().unique_candidates == 8U);
}

TEST_CASE("M28 structural universe rejects checked range overflow and duplicate module identity")
{
    ModuleIdentity overflow;
    overflow.module = "overflow";
    overflow.executable_ranges = {{std::numeric_limits<GuestAddress>::max() - 1U, 4U}};
    REQUIRE(analysis::derive_indirect_target_candidate_universe(
                std::span<const ModuleIdentity>(&overflow, 1U))
                .error()
                .code == ErrorCode::InvalidGuestAddress);

    ModuleIdentity first;
    first.module = "duplicate";
    ModuleIdentity second = first;
    std::array<ModuleIdentity, 2U> duplicate{first, second};
    REQUIRE(analysis::derive_indirect_target_candidate_universe(
                std::span<const ModuleIdentity>(duplicate.data(), duplicate.size()))
                .error()
                .code == ErrorCode::DuplicateModuleIdentity);
}

TEST_CASE("M28 large structural universes retain only observed sparse records")
{
    ModuleIdentity module;
    module.module = "large-synthetic-m28";
    module.executable_ranges = {{0x1000U, 0x400000U}};
    const auto universe = analysis::derive_indirect_target_candidate_universe(
        std::span<const ModuleIdentity>(&module, 1U));
    REQUIRE(universe);
    REQUIRE(universe.value().executable_instruction_slots > 1'000'000U);

    IndirectTargetRefinementBudgets budgets;
    budgets.candidate_universe = universe.value();
    IndirectTargetRefinementWorklist worklist(budgets);
    REQUIRE(worklist.observe(synthetic_assessment(observed(module.module, 0x1000U))).accepted);
    REQUIRE(worklist.observe(synthetic_assessment(observed(module.module, 0x3ffffcU))).accepted);
    REQUIRE(worklist.summary().unique_candidates == 2U);
    REQUIRE(worklist.summary().candidate_records == 2U);
}

TEST_CASE("M28 more than 256 certified immutable promotions use ordinary structural capacity")
{
    constexpr std::size_t candidate_count = 300U;
    constexpr GuestAddress base = 0x100000U;
    auto module = make_ret_module("synthetic-m28", base, candidate_count);
    REQUIRE(module);
    std::array<ModuleIdentity, 1U> identities{module.value().identity};
    const auto universe = analysis::derive_indirect_target_candidate_universe(
        std::span<const ModuleIdentity>(identities.data(), identities.size()));
    REQUIRE(universe);
    REQUIRE(universe.value().executable_instruction_slots == candidate_count);

    const std::vector<FunctionSeed> roots{
        seed(base, FunctionDiscoverySource::ModuleEntry, FunctionConfidence::Confirmed, "root")};
    const auto initial = analysis::FunctionMapBuilder::build(
        analysis::ModuleAnalysisInput{module.value().identity, &module.value().memory, roots});
    REQUIRE(initial);
    auto map = initial.value();

    IndirectTargetRefinementBudgets budgets;
    budgets.candidate_universe = universe.value();
    IndirectTargetRefinementWorklist worklist(budgets);
    analysis::IndirectTargetDiscoveryOptions options;
    const auto input = analysis::ModuleAnalysisInput{module.value().identity,
                                                     &module.value().memory, roots};
    for (std::size_t index = 1U; index < candidate_count; ++index)
    {
        REQUIRE(worklist.begin_round());
        const auto item = observed("synthetic-m28", base + index * 4U);
        const auto assessed = analysis::assess_indirect_target(item, module.value().memory, &map,
                                                                nullptr, nullptr, options);
        REQUIRE(assessed);
        REQUIRE(assessed.value().decision.kind == IndirectTargetDecisionKind::TrustedNewEntry);
        REQUIRE(worklist.observe(assessed.value()).newly_unique_candidate);
        const auto pending = worklist.pending_candidates();
        REQUIRE(pending.size() == 1U);
        const auto identity = analysis::indirect_target_candidate_identity(pending.front());
        REQUIRE(worklist.begin_candidate_assessment(identity));
        const auto refined = analysis::refine_function_map(map, input, item, options);
        REQUIRE(refined);
        REQUIRE(refined.value().assessment.decision.promoted);
        REQUIRE(worklist.can_commit_refinement(identity, refined.value().analysis_work));
        map = refined.value().map;
        worklist.record_promotion(identity, 1U, 0U, refined.value().analysis_work);
        worklist.end_round();
    }

    const auto summary = worklist.summary();
    REQUIRE(summary.unique_candidates == candidate_count - 1U);
    REQUIRE(summary.candidate_records == candidate_count - 1U);
    REQUIRE(summary.successful_promotions == candidate_count - 1U);
    REQUIRE(summary.exhaustion.dimension == IndirectTargetRefinementBudgetDimension::None);
    REQUIRE(map.find_exact_entry(base + (candidate_count - 1U) * 4U) != nullptr);
}

TEST_CASE("M28 explicit legacy candidate ceiling is exact and preserves overflow work")
{
    IndirectTargetRefinementBudgets budgets;
    budgets.max_unique_candidates = 2U;
    budgets.candidate_universe.executable_instruction_slots = 64U;
    IndirectTargetRefinementWorklist worklist(budgets);
    REQUIRE(worklist.observe(synthetic_assessment(observed("synthetic-m28", 0x2000U))).accepted);
    REQUIRE(worklist.observe(synthetic_assessment(observed("synthetic-m28", 0x2004U))).accepted);
    REQUIRE_FALSE(
        worklist.observe(synthetic_assessment(observed("synthetic-m28", 0x2008U))).accepted);
    const auto summary = worklist.summary();
    REQUIRE(summary.exhaustion.dimension == IndirectTargetRefinementBudgetDimension::UniqueCandidates);
    REQUIRE(analysis::indirect_target_refinement_budget_dimension_name(
                summary.exhaustion.dimension) == "unique_candidates");
    REQUIRE(summary.exhaustion.consumed == 2U);
    REQUIRE(summary.exhaustion.limit == 2U);
    REQUIRE(summary.configured.candidate_limit_provenance.kind ==
            analysis::AnalysisBudgetProvenanceKind::ExplicitApiOverride);
    REQUIRE(summary.pending_candidate_count == 3U);
    REQUIRE(summary.candidate_records == 2U);
}

TEST_CASE("M28 duplicate target observations coalesce records but retain provenance")
{
    IndirectTargetRefinementWorklist worklist;
    auto first = observed("synthetic-m28", 0x3000U, 0x1000U);
    auto second = observed("synthetic-m28", 0x3000U, 0x2000U);
    second.target_register = "x9";
    REQUIRE(worklist.observe(synthetic_assessment(first)).newly_unique_candidate);
    REQUIRE_FALSE(worklist.observe(synthetic_assessment(second)).newly_unique_candidate);
    REQUIRE_FALSE(worklist.observe(synthetic_assessment(second)).newly_unique_observation);
    const auto pending = worklist.pending_candidates();
    REQUIRE(pending.size() == 1U);
    REQUIRE(pending.front().observation_provenance.size() == 2U);
    REQUIRE(worklist.summary().unique_candidates == 1U);
    REQUIRE(worklist.summary().candidate_records == 1U);
    REQUIRE(worklist.summary().duplicate_coalesced_observations == 1U);
}

TEST_CASE("M28 structurally impossible targets remain typed without candidate records")
{
    memory::GuestMemory memory;
    std::vector<std::byte> executable(0x0eU, std::byte{});
    for (std::size_t offset = 0U; offset + 4U <= executable.size(); offset += 4U)
        write_word(executable, offset, nop_instruction);
    REQUIRE(memory.map(0x4000U, std::span<const std::byte>(executable.data(), executable.size()),
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "synthetic.m28.text", memory::GuestRegionKind::Text));
    std::vector<std::byte> data(0x10U, std::byte{});
    REQUIRE(memory.map(0x5000U, std::span<const std::byte>(data.data(), data.size()),
                       memory::GuestMemoryPermissions::Read,
                       "synthetic.m28.data", memory::GuestRegionKind::Data));

    const std::array<GuestAddress, 4U> targets{0x6000U, 0x5000U, 0x4002U, 0x400cU};
    IndirectTargetRefinementWorklist worklist;
    for (const auto target : targets)
    {
        const auto item = observed("synthetic-m28", target);
        const auto assessed = analysis::assess_indirect_target(item, memory);
        REQUIRE(assessed);
        REQUIRE_FALSE(assessed.value().validation.structurally_eligible);
        REQUIRE(worklist.observe(assessed.value()).accepted);
    }
    const auto summary = worklist.summary();
    REQUIRE(summary.unique_observations == targets.size());
    REQUIRE(summary.structurally_ineligible_candidates == targets.size());
    REQUIRE(summary.unique_candidates == 0U);
    REQUIRE(summary.candidate_records == 0U);
    REQUIRE(summary.pending_candidate_count == 0U);
}

TEST_CASE("M28 pending order is stable and candidate exhaustion leaves the prior map untouched")
{
    IndirectTargetRefinementBudgets budgets;
    budgets.max_unique_candidates = 2U;
    IndirectTargetRefinementWorklist worklist(budgets);
    const auto third = synthetic_assessment(observed("synthetic-m28", 0x7008U));
    const auto first = synthetic_assessment(observed("synthetic-m28", 0x7000U));
    const auto second = synthetic_assessment(observed("synthetic-m28", 0x7004U));
    REQUIRE(worklist.observe(third).accepted);
    REQUIRE(worklist.observe(first).accepted);
    REQUIRE_FALSE(worklist.observe(second).accepted);
    const auto pending = worklist.pending_candidates();
    REQUIRE(pending.size() == 2U);
    REQUIRE(pending[0].target == 0x7000U);
    REQUIRE(pending[1].target == 0x7008U);
    REQUIRE(worklist.summary().pending_candidate_count == 3U);

    auto module = make_ret_module("synthetic-m28-map", 0x8000U, 2U);
    REQUIRE(module);
    const auto initial = analysis::FunctionMapBuilder::build(
        analysis::ModuleAnalysisInput{
            module.value().identity, &module.value().memory,
            {seed(0x8000U, FunctionDiscoverySource::ModuleEntry,
                  FunctionConfidence::Confirmed, "root")} });
    REQUIRE(initial);
    const auto before = initial.value();
    REQUIRE(worklist.exhausted());
    REQUIRE(before.frozen());
    REQUIRE(before.functions().size() == initial.value().functions().size());
}

TEST_CASE("M28 candidate exhaustion cannot publish a partial immutable refinement")
{
    constexpr GuestAddress base = 0x9000U;
    auto module = make_ret_module("synthetic-m28-transaction", base, 3U);
    REQUIRE(module);
    const std::vector<FunctionSeed> roots{
        seed(base, FunctionDiscoverySource::ModuleEntry, FunctionConfidence::Confirmed, "root")};
    const auto initial = analysis::FunctionMapBuilder::build(
        analysis::ModuleAnalysisInput{module.value().identity, &module.value().memory, roots});
    REQUIRE(initial);
    auto published = initial.value();

    IndirectTargetRefinementBudgets budgets;
    budgets.max_unique_candidates = 1U;
    IndirectTargetRefinementWorklist worklist(budgets);
    analysis::IndirectTargetDiscoveryOptions options;
    const auto input = analysis::ModuleAnalysisInput{module.value().identity,
                                                     &module.value().memory, roots};
    const auto first = observed("synthetic-m28-transaction", base + 4U);
    const auto first_identity = analysis::indirect_target_candidate_identity(first);
    REQUIRE(worklist.observe(synthetic_assessment(first)).accepted);
    REQUIRE(worklist.begin_candidate_assessment(first_identity));
    const auto refined = analysis::refine_function_map(published, input, first, options);
    REQUIRE(refined);
    REQUIRE(refined.value().assessment.decision.promoted);
    REQUIRE(worklist.can_commit_refinement(first_identity, refined.value().analysis_work));
    published = refined.value().map;
    worklist.record_promotion(first_identity, 1U, 0U, refined.value().analysis_work);
    REQUIRE(published.frozen());
    REQUIRE(published.find_exact_entry(base + 4U) != nullptr);

    const auto prior_function_count = published.functions().size();
    const auto prior_candidate = published.find_exact_entry(base + 4U);
    const auto second = observed("synthetic-m28-transaction", base + 8U);
    REQUIRE_FALSE(worklist.observe(synthetic_assessment(second)).accepted);
    REQUIRE(worklist.summary().exhaustion.dimension ==
            IndirectTargetRefinementBudgetDimension::UniqueCandidates);
    REQUIRE(published.frozen());
    REQUIRE(published.functions().size() == prior_function_count);
    REQUIRE(published.find_exact_entry(base + 4U) == prior_candidate);
    REQUIRE(published.find_exact_entry(base + 8U) == nullptr);
}

} // namespace
