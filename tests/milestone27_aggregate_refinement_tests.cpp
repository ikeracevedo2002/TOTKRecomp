#include "switchrecomp/analysis/indirect_target.hpp"
#include "switchrecomp/analysis/function_map.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

namespace
{

using namespace switchrecomp;
using analysis::AnalysisBudgets;
using analysis::FunctionConfidence;
using analysis::FunctionDiscoverySource;
using analysis::FunctionMapOptions;
using analysis::FunctionSeed;
using analysis::IndirectTargetAssessment;
using analysis::IndirectTargetCandidateIdentity;
using analysis::IndirectTargetDecisionKind;
using analysis::IndirectTargetRefinementAnalysisDimension;
using analysis::IndirectTargetRefinementAnalysisWork;
using analysis::IndirectTargetRefinementBudgetDimension;
using analysis::IndirectTargetRefinementBudgets;
using analysis::IndirectTargetRefinementWorklist;
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

[[nodiscard]] std::uint32_t unconditional_branch(GuestAddress pc, GuestAddress target)
{
    const auto displacement = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pc);
    return 0x14000000U | (static_cast<std::uint32_t>(displacement / 4) & 0x03ffffffU);
}

struct SyntheticModule
{
    memory::GuestMemory memory;
    analysis::ModuleIdentity identity;
};

[[nodiscard]] Result<SyntheticModule> make_module(std::string name, GuestAddress base,
                                                    std::size_t size = 0x100U,
                                                    std::initializer_list<std::pair<std::size_t,
                                                                                    std::uint32_t>> words = {})
{
    SyntheticModule module;
    std::vector<std::byte> code(size, std::byte{});
    for (std::size_t offset = 0U; offset + 4U <= code.size(); offset += 4U)
        write_word(code, offset, nop_instruction);
    for (const auto& [offset, value] : words)
    {
        if (offset + 4U > code.size())
            return Result<SyntheticModule>::failure(
                make_error(ErrorCode::InvalidArgument, "synthetic instruction exceeds module"));
        write_word(code, offset, value);
    }
    const auto mapped = module.memory.map(
        base, std::span<const std::byte>(code.data(), code.size()),
        memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Execute,
        "synthetic.m27.text", memory::GuestRegionKind::Text);
    if (!mapped) return Result<SyntheticModule>::failure(mapped.error());
    module.identity.module = std::move(name);
    module.identity.build_id = "synthetic-m27-build";
    module.identity.input_sha256 = "synthetic-m27-sha";
    module.identity.guest_base = base;
    module.identity.executable_ranges.push_back(
        analysis::GuestAddressRange{base, static_cast<memory::GuestSize>(size)});
    return Result<SyntheticModule>::success(std::move(module));
}

[[nodiscard]] FunctionSeed seed(GuestAddress entry, FunctionDiscoverySource source,
                                FunctionConfidence confidence, std::string note = {})
{
    return FunctionSeed{entry, source, confidence, std::nullopt, std::nullopt, std::move(note)};
}

[[nodiscard]] ObservedIndirectTarget observed(std::string module, GuestAddress source,
                                              GuestAddress target)
{
    ObservedIndirectTarget result;
    result.source_module = module;
    result.source_function = source;
    result.source_pc = source + 4U;
    result.control_flow = analysis::IndirectControlFlowKind::Call;
    result.target_register = "x8";
    result.target = target;
    result.target_module = std::move(module);
    result.pointer_provenance = analysis::IndirectTargetPointerProvenanceKind::Unknown;
    return result;
}

[[nodiscard]] IndirectTargetAssessment assessment(const ObservedIndirectTarget& item)
{
    IndirectTargetAssessment result;
    result.observed = item;
    result.validation.target_module = item.target_module;
    result.validation.structurally_eligible = true;
    result.decision.kind = IndirectTargetDecisionKind::TrustedNewEntry;
    result.decision.eligible_for_promotion = true;
    return result;
}

[[nodiscard]] IndirectTargetRefinementAnalysisWork work(std::string module,
                                                        std::size_t instructions = 4U)
{
    return IndirectTargetRefinementAnalysisWork{std::move(module), 1U, 0U, 0U, instructions,
                                               1U, 0U, static_cast<memory::GuestSize>(instructions),
                                               1U, 0U, 1U};
}

TEST_CASE("M27 ordinary aggregate accounting permits well over 128 certified promotions")
{
    IndirectTargetRefinementWorklist worklist;
    constexpr std::size_t promotion_count = 192U;
    for (std::size_t index = 0U; index < promotion_count; ++index)
    {
        REQUIRE(worklist.begin_round());
        const auto item = observed("synthetic-m27", 0x1000U, 0x2000U + index * 4U);
        const auto identity = analysis::indirect_target_candidate_identity(item);
        REQUIRE(worklist.observe(assessment(item)).newly_unique_candidate);
        REQUIRE(worklist.begin_candidate_assessment(identity));
        REQUIRE(worklist.can_commit_refinement(identity, work("synthetic-m27")));
        worklist.record_promotion(identity, 1U, 2U, work("synthetic-m27"));
        worklist.end_round();
    }
    const auto summary = worklist.summary();
    REQUIRE(summary.successful_promotions == promotion_count);
    REQUIRE(summary.map_rebuilds == promotion_count);
    REQUIRE_FALSE(summary.configured.legacy_event_limits);
    REQUIRE(summary.analysis.transactions == promotion_count);
    REQUIRE(summary.exhaustion.dimension == IndirectTargetRefinementBudgetDimension::None);
}

TEST_CASE("M27 aggregate analysis exhaustion identifies the exact next work item")
{
    IndirectTargetRefinementBudgets budgets;
    budgets.analysis.max_instructions = 5U;
    IndirectTargetRefinementWorklist worklist(budgets);
    const auto first = observed("synthetic-m27", 0x1000U, 0x3000U);
    const auto second = observed("synthetic-m27", 0x1000U, 0x3004U);
    REQUIRE(worklist.observe(assessment(first)).accepted);
    REQUIRE(worklist.observe(assessment(second)).accepted);

    const auto first_identity = analysis::indirect_target_candidate_identity(first);
    REQUIRE(worklist.begin_candidate_assessment(first_identity));
    REQUIRE(worklist.can_commit_refinement(first_identity, work("synthetic-m27")));
    worklist.record_promotion(first_identity, 1U, 0U, work("synthetic-m27"));

    const auto second_identity = analysis::indirect_target_candidate_identity(second);
    REQUIRE(worklist.begin_candidate_assessment(second_identity));
    REQUIRE_FALSE(worklist.can_commit_refinement(second_identity, work("synthetic-m27")));
    const auto summary = worklist.summary();
    REQUIRE(summary.exhaustion.dimension ==
            IndirectTargetRefinementBudgetDimension::RefinementAnalysisInstructions);
    REQUIRE(summary.exhaustion.consumed == 4U);
    REQUIRE(summary.exhaustion.limit == 5U);
    REQUIRE(summary.exhaustion.module == "synthetic-m27");
    REQUIRE(summary.exhaustion.generation == 1U);
    REQUIRE(summary.exhaustion.next_work == second_identity);
    REQUIRE(summary.pending_candidate_count == 1U);
}

TEST_CASE("M27 disjoint immutable refinement reuses unaffected finalized analysis")
{
    auto module = make_module("main", 0x1000U, 0x100U,
                              {{0U, ret_instruction}, {0x20U, ret_instruction}});
    REQUIRE(module);
    const auto initial = analysis::FunctionMapBuilder::build(
        analysis::ModuleAnalysisInput{module.value().identity, &module.value().memory,
                                      {seed(0x1000U, FunctionDiscoverySource::ModuleEntry,
                                            FunctionConfidence::Confirmed, "root")}});
    REQUIRE(initial);

    analysis::IndirectTargetDiscoveryOptions options;
    const auto refined = analysis::refine_function_map(
        initial.value(),
        analysis::ModuleAnalysisInput{module.value().identity, &module.value().memory,
                                      {seed(0x1000U, FunctionDiscoverySource::ModuleEntry,
                                            FunctionConfidence::Confirmed, "root")}},
        observed("main", 0x1000U, 0x1020U), options);
    REQUIRE(refined);
    REQUIRE(refined.value().assessment.decision.promoted);
    REQUIRE(refined.value().map.find_exact_entry(0x1020U) != nullptr);
    REQUIRE(refined.value().analysis_work.functions_reused == 1U);
    REQUIRE(refined.value().analysis_work.invalidated_records == 0U);
    REQUIRE(refined.value().analysis_work.functions_reanalyzed == 0U);
    REQUIRE(refined.value().analysis_work.transactions == 1U);
    REQUIRE(refined.value().map.frozen());
    REQUIRE(initial.value().find_exact_entry(0x1020U) == nullptr);
}

TEST_CASE("M27 newly introduced callable boundaries invalidate dependent ownership")
{
    constexpr GuestAddress base = 0x4000U;
    auto module = make_module("main", base, 0x40U,
                              {{0U, unconditional_branch(base, base + 0x8U)},
                               {0x8U, ret_instruction}});
    REQUIRE(module);
    const auto initial = analysis::FunctionMapBuilder::build(
        analysis::ModuleAnalysisInput{module.value().identity, &module.value().memory,
                                      {seed(base, FunctionDiscoverySource::ModuleEntry,
                                            FunctionConfidence::Confirmed, "root")} });
    REQUIRE(initial);
    REQUIRE(initial.value().find_precise_owners(base + 0x8U).size() == 1U);

    FunctionMapOptions options;
    options.reuse_map = &initial.value();
    options.newly_introduced_function_entries.insert(base + 0x8U);
    const auto rebuilt = analysis::FunctionMapBuilder::build(
        analysis::ModuleAnalysisInput{
            module.value().identity, &module.value().memory,
            {seed(base, FunctionDiscoverySource::ModuleEntry, FunctionConfidence::Confirmed,
                  "root"),
             seed(base + 0x8U, FunctionDiscoverySource::ObservedIndirectTarget,
                  FunctionConfidence::High, "new callable boundary")}},
        options);
    REQUIRE(rebuilt);
    REQUIRE(rebuilt.value().find_exact_entry(base + 0x8U) != nullptr);
    REQUIRE(rebuilt.value().find_precise_owners(base + 0x8U).size() == 1U);
    REQUIRE(rebuilt.value().accounting().invalidated_records == 1U);
    REQUIRE(rebuilt.value().accounting().reanalyzed_functions >= 1U);
    REQUIRE(rebuilt.value().accounting().reused_functions == 0U);
    REQUIRE(rebuilt.value().conflicts().empty());
}

TEST_CASE("M27 aggregate dimension names and finite defaults are deterministic")
{
    const IndirectTargetRefinementBudgets budgets;
    REQUIRE(budgets.analysis.max_functions_analyzed != 0U);
    REQUIRE(budgets.analysis.max_functions_reanalyzed != 0U);
    REQUIRE(budgets.analysis.max_transactions != 0U);
    REQUIRE(analysis::indirect_target_refinement_analysis_dimension_name(
                IndirectTargetRefinementAnalysisDimension::Instructions) == "instructions");
    REQUIRE(analysis::indirect_target_refinement_budget_dimension_name(
                IndirectTargetRefinementBudgetDimension::RefinementAnalysisTransactions) ==
            "refinement_analysis_transactions");
}

} // namespace
