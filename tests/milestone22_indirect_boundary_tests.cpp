#include "switchrecomp/analysis/indirect_target.hpp"
#include "switchrecomp/execution/session.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace switchrecomp;
using analysis::FunctionConfidence;
using analysis::FunctionDiscoverySource;
using analysis::FunctionEntryEvidence;
using analysis::FunctionEntryEvidenceKind;
using analysis::FunctionEntryEvidenceStrength;
using analysis::FunctionBoundaryReconciliationKind;
using analysis::IndirectControlFlowKind;
using analysis::IndirectTargetDecisionKind;
using analysis::IndirectTargetDiscoveryOptions;
using analysis::ObservedIndirectTarget;
using memory::GuestAddress;

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

[[nodiscard]] std::uint32_t branch(GuestAddress pc, GuestAddress target, bool link = false)
{
    const auto displacement = static_cast<std::int64_t>(target) -
                              static_cast<std::int64_t>(pc);
    return (link ? 0x94000000U : 0x14000000U) |
           (static_cast<std::uint32_t>(displacement / 4) & 0x03ffffffU);
}

[[nodiscard]] analysis::FunctionSeed manual_seed(GuestAddress address)
{
    return analysis::FunctionSeed{address, FunctionDiscoverySource::AnalystSeed,
                                  FunctionConfidence::Manual, std::nullopt, std::nullopt,
                                  "M22 synthetic boundary seed"};
}

void write_words(std::vector<std::byte>& code, std::size_t offset,
                 std::initializer_list<std::uint32_t> values)
{
    const auto bytes = words(values);
    REQUIRE(offset <= code.size());
    REQUIRE(bytes.size() <= code.size() - offset);
    std::copy(bytes.begin(), bytes.end(), code.begin() + static_cast<std::ptrdiff_t>(offset));
}

struct Fixture
{
    memory::GuestMemory memory;
    analysis::ModuleIdentity identity;
    analysis::FinalizedFunctionMap map;
    std::vector<analysis::FunctionSeed> seeds;
};

[[nodiscard]] Result<Fixture> make_fixture(GuestAddress base, GuestAddress size,
                                            std::vector<std::byte> code,
                                            std::vector<analysis::FunctionSeed> seeds,
                                            std::optional<std::pair<GuestAddress, GuestAddress>> data =
                                                std::nullopt)
{
    Fixture fixture;
    if (code.size() < size) code.resize(static_cast<std::size_t>(size), std::byte{});
    const auto mapped = fixture.memory.map(
        base, std::span<const std::byte>(code.data(), code.size()),
        memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Execute,
        "synthetic.m22.text", memory::GuestRegionKind::Text);
    if (!mapped) return Result<Fixture>::failure(mapped.error());
    if (data)
    {
        const auto data_mapped = fixture.memory.map(
            data->first, data->second,
            memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Write,
            "synthetic.m22.data", memory::GuestRegionKind::Data);
        if (!data_mapped) return Result<Fixture>::failure(data_mapped.error());
    }
    fixture.identity.module = "synthetic-m22";
    fixture.identity.build_id = "synthetic-m22-build";
    fixture.identity.input_sha256 = "synthetic-m22-sha";
    fixture.identity.guest_base = base;
    fixture.identity.executable_ranges.push_back({base, size});
    fixture.seeds = seeds;
    const auto finalized = analysis::FunctionMapBuilder::build(
        analysis::ModuleAnalysisInput{fixture.identity, &fixture.memory, fixture.seeds});
    if (!finalized) return Result<Fixture>::failure(finalized.error());
    fixture.map = std::move(finalized).value();
    return Result<Fixture>::success(std::move(fixture));
}

[[nodiscard]] ObservedIndirectTarget observed(GuestAddress target)
{
    ObservedIndirectTarget result;
    result.source_module = "synthetic-m22";
    result.source_function = 0x1000U;
    result.source_pc = 0x1004U;
    result.control_flow = IndirectControlFlowKind::Call;
    result.target_register = "x8";
    result.target = target;
    result.target_module = "synthetic-m22";
    result.pointer_provenance = analysis::IndirectTargetPointerProvenanceKind::GuestLoad;
    result.guest_load_address = 0x8000U;
    return result;
}

[[nodiscard]] analysis::ModuleAnalysisInput input_for(const Fixture& fixture)
{
    return analysis::ModuleAnalysisInput{fixture.identity, &fixture.memory, fixture.seeds};
}

TEST_CASE("M22 exact entry and precise-owner APIs remain distinct and idempotent")
{
    auto code = words({0xd65f03c0U, 0xd65f03c0U});
    code.resize(0x40U, std::byte{});
    auto fixture = make_fixture(0x1000U, 0x40U, std::move(code),
                                {manual_seed(0x1000U), manual_seed(0x1004U)});
    REQUIRE(fixture);
    REQUIRE(fixture.value().map.find_exact_entry(0x1000U) != nullptr);
    REQUIRE(fixture.value().map.find_canonical_entry(0x1000U) != nullptr);
    REQUIRE(fixture.value().map.find_callable_entry(0x1000U) != nullptr);
    REQUIRE(fixture.value().map.find_precise_owners(0x1000U).size() == 1U);
    REQUIRE(fixture.value().map.find_exact_entry(0x1008U) == nullptr);
    REQUIRE(fixture.value().map.find_precise_owners(0x1008U).empty());

    const auto assessment = analysis::assess_indirect_target(
        observed(0x1004U), fixture.value().memory, &fixture.value().map);
    REQUIRE(assessment);
    REQUIRE(assessment.value().decision.kind == IndirectTargetDecisionKind::TrustedExistingEntry);
    const auto refinement = analysis::refine_function_map(
        fixture.value().map, input_for(fixture.value()), observed(0x1004U));
    REQUIRE(refinement);
    REQUIRE_FALSE(refinement.value().assessment.decision.promoted);
    REQUIRE(refinement.value().map.functions().size() == fixture.value().map.functions().size());
}

TEST_CASE("M22 disjoint candidate promotion is transactional and idempotent")
{
    auto code = std::vector<std::byte>(0x40U, std::byte{});
    write_words(code, 0U, {0xd65f03c0U});
    write_words(code, 0x20U, {0xd65f03c0U});
    auto fixture = make_fixture(0x1000U, 0x40U, std::move(code), {manual_seed(0x1000U)});
    REQUIRE(fixture);
    const auto before = fixture.value().map.functions().size();
    const auto refinement = analysis::refine_function_map(
        fixture.value().map, input_for(fixture.value()), observed(0x1020U));
    REQUIRE(refinement);
    REQUIRE(refinement.value().assessment.decision.promoted);
    REQUIRE(refinement.value().map.functions().size() == before + 1U);
    REQUIRE(refinement.value().map.conflicts().empty());
    REQUIRE(fixture.value().map.functions().size() == before);

    const auto repeated = analysis::refine_function_map(
        refinement.value().map, input_for(fixture.value()), observed(0x1020U));
    REQUIRE(repeated);
    REQUIRE_FALSE(repeated.value().assessment.decision.promoted);
    REQUIRE(repeated.value().assessment.decision.kind ==
            IndirectTargetDecisionKind::TrustedExistingEntry);
    REQUIRE(repeated.value().map.functions().size() == refinement.value().map.functions().size());
}

TEST_CASE("M22 refinement keeps an honest failed direct-call closure separate from the candidate")
{
    auto code = std::vector<std::byte>(0x80U, std::byte{});
    write_words(code, 0U, {0xd65f03c0U});
    write_words(code, 0x20U, {branch(0x1020U, 0x1040U, true), 0xd65f03c0U});
    write_words(code, 0x40U, {branch(0x1040U, 0x1050U)});
    write_words(code, 0x50U, {0xffffffffU});
    auto fixture = make_fixture(0x1000U, 0x80U, std::move(code), {manual_seed(0x1000U)});
    REQUIRE(fixture);

    const auto refinement = analysis::refine_function_map(
        fixture.value().map, input_for(fixture.value()), observed(0x1020U));
    REQUIRE(refinement);
    REQUIRE(refinement.value().assessment.decision.promoted);

    const auto* candidate = refinement.value().map.find_exact_entry(0x1020U);
    REQUIRE(candidate != nullptr);
    REQUIRE(candidate->cfg != nullptr);
    const auto* failed_callee = refinement.value().map.find_exact_entry(0x1040U);
    REQUIRE(failed_callee != nullptr);
    REQUIRE(failed_callee->cfg == nullptr);
    REQUIRE(failed_callee->translation_status == analysis::TranslationStatus::Failed);

    // An exact failed record is not dereferenced as though it had a CFG.
    const auto failed_assessment = analysis::assess_indirect_target(
        observed(0x1040U), fixture.value().memory, &refinement.value().map);
    REQUIRE(failed_assessment);
    REQUIRE_FALSE(failed_assessment.value().decision.eligible_for_promotion);
    REQUIRE(failed_assessment.value().decision.kind ==
            IndirectTargetDecisionKind::InsufficientEvidence);
}

TEST_CASE("M22 internal fallthrough overlap remains a typed conflict")
{
    auto code = std::vector<std::byte>(0x80U, std::byte{});
    write_words(code, 0U, {branch(0x1000U, 0x103cU)});
    write_words(code, 0x3cU, {0xd503201fU, 0xd65f03c0U});
    write_words(code, 0x60U, {branch(0x1060U, 0x1040U)});
    auto fixture = make_fixture(0x1000U, 0x80U, std::move(code), {manual_seed(0x1000U)});
    REQUIRE(fixture);
    const auto assessment = analysis::assess_indirect_target(
        observed(0x1060U), fixture.value().memory, &fixture.value().map);
    REQUIRE(assessment);
    REQUIRE(assessment.value().decision.kind == IndirectTargetDecisionKind::FunctionBoundaryConflict);
    REQUIRE(assessment.value().validation.boundary_reconciliation.kind ==
            FunctionBoundaryReconciliationKind::IncompatiblePreciseOverlap);
    REQUIRE(assessment.value().validation.overlap_ranges ==
            std::vector<analysis::GuestAddressRange>{{0x1040U, 4U}});
    REQUIRE_FALSE(assessment.value().decision.eligible_for_promotion);
}

TEST_CASE("M22 distinct callable entries may share an identical closed tail")
{
    auto code = std::vector<std::byte>(0xa0U, std::byte{});
    write_words(code, 0U, {branch(0x1000U, 0x1040U)});
    write_words(code, 0x40U, {0xd65f03c0U});
    write_words(code, 0x60U, {branch(0x1060U, 0x1040U)});
    write_words(code, 0x80U, {branch(0x1080U, 0x1040U)});
    auto fixture = make_fixture(0x1000U, 0xa0U, std::move(code), {manual_seed(0x1000U)});
    REQUIRE(fixture);

    const auto assessment = analysis::assess_indirect_target(
        observed(0x1060U), fixture.value().memory, &fixture.value().map);
    REQUIRE(assessment);
    REQUIRE(assessment.value().decision.kind == IndirectTargetDecisionKind::TrustedNewEntry);
    REQUIRE(assessment.value().decision.eligible_for_promotion);
    REQUIRE(assessment.value().validation.boundary_reconciliation.kind ==
            FunctionBoundaryReconciliationKind::SharedTail);

    const auto refinement = analysis::refine_function_map(
        fixture.value().map, input_for(fixture.value()), observed(0x1060U));
    REQUIRE(refinement);
    REQUIRE(refinement.value().assessment.decision.promoted);
    REQUIRE(refinement.value().map.conflicts().empty());
    REQUIRE(refinement.value().map.shared_code().size() == 1U);
    REQUIRE(refinement.value().map.shared_code().front().ranges ==
            std::vector<analysis::GuestAddressRange>{{0x1040U, 4U}});
    REQUIRE(refinement.value().map.find_precise_owners(0x1040U).size() == 2U);

    const auto third = analysis::refine_function_map(
        refinement.value().map, input_for(fixture.value()), observed(0x1080U));
    REQUIRE(third);
    REQUIRE(third.value().assessment.decision.promoted);
    REQUIRE(third.value().assessment.validation.boundary_reconciliation.kind ==
            FunctionBoundaryReconciliationKind::SharedTail);
    REQUIRE(third.value().map.conflicts().empty());
    REQUIRE(third.value().map.shared_code().size() == 3U);
    REQUIRE(third.value().map.find_precise_owners(0x1040U).size() == 3U);
}

TEST_CASE("M22 shared-tail classification is seed-order worker and reuse deterministic")
{
    auto code = std::vector<std::byte>(0x80U, std::byte{});
    write_words(code, 0U, {branch(0x1000U, 0x1040U)});
    write_words(code, 0x40U, {0xd65f03c0U});
    write_words(code, 0x60U, {branch(0x1060U, 0x1040U)});
    auto fixture = make_fixture(0x1000U, 0x80U, std::move(code), {manual_seed(0x1000U)});
    REQUIRE(fixture);
    const auto incremental = analysis::refine_function_map(
        fixture.value().map, input_for(fixture.value()), observed(0x1060U));
    REQUIRE(incremental);
    REQUIRE(incremental.value().assessment.decision.promoted);

    const auto build = [&](std::vector<analysis::FunctionSeed> seeds, std::size_t workers) {
        analysis::FunctionMapOptions options;
        options.analysis_workers = workers;
        return analysis::FunctionMapBuilder::build(
            analysis::ModuleAnalysisInput{fixture.value().identity, &fixture.value().memory,
                                          std::move(seeds)},
            options);
    };
    const auto forward = build({manual_seed(0x1000U), manual_seed(0x1060U)}, 1U);
    const auto reverse = build({manual_seed(0x1060U), manual_seed(0x1000U)}, 1U);
    const auto parallel = build({manual_seed(0x1000U), manual_seed(0x1060U)}, 2U);
    REQUIRE(forward);
    REQUIRE(reverse);
    REQUIRE(parallel);

    const auto snapshot = [](const analysis::FinalizedFunctionMap& map) {
        std::vector<std::pair<GuestAddress, std::vector<analysis::GuestAddressRange>>> result;
        for (const auto& function : map.functions())
            result.emplace_back(function.canonical_entry, function.owned_code_ranges);
        return std::make_pair(result, map.shared_code());
    };
    REQUIRE(snapshot(forward.value()) == snapshot(reverse.value()));
    REQUIRE(snapshot(forward.value()) == snapshot(parallel.value()));
    REQUIRE(snapshot(forward.value()) == snapshot(incremental.value().map));
    REQUIRE(forward.value().conflicts().empty());
}

TEST_CASE("M22 execution resumes an exact refined indirect-call boundary without replay")
{
    auto code = std::vector<std::byte>(0x40U, std::byte{});
    write_words(code, 0U, {0xd2820410U, 0xd63f0200U, 0xd65f03c0U});
    write_words(code, 0x20U, {0xd65f03c0U});
    auto fixture = make_fixture(0x1000U, 0x40U, std::move(code), {manual_seed(0x1000U)});
    REQUIRE(fixture);
    execution::ExecutionSession session(fixture.value().memory, fixture.value().map, {});
    execution::EntrySelection selection;
    selection.kind = execution::EntrySelectionKind::AnalystAddress;
    selection.address = 0x1000U;
    selection.source = "M22 resumable refinement fixture";
    selection.confidence = FunctionConfidence::Manual;
    auto first = session.run(selection);
    REQUIRE(first);
    REQUIRE(first.value().stop_reason == execution::ExecutionStopReason::UnknownGuestFunction);
    REQUIRE(first.value().target == 0x1020U);
    const auto prefix_instructions = first.value().guest_instruction_count;

    const auto refinement = analysis::refine_function_map(
        fixture.value().map, input_for(fixture.value()), observed(0x1020U));
    REQUIRE(refinement);
    REQUIRE(refinement.value().assessment.decision.promoted);
    fixture.value().map = refinement.value().map;
    const auto resumed = session.resume_after_refinement(std::move(first).value());
    REQUIRE(resumed);
    REQUIRE(resumed.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(resumed.value().guest_instruction_count == prefix_instructions + 2U);
    REQUIRE(resumed.value().indirect_calls == 1U);
}

TEST_CASE("M22 refinement clears stale boundary diagnostics before resumed execution")
{
    auto code = std::vector<std::byte>(0x40U, std::byte{});
    write_words(code, 0U, {0xd2820410U, 0xd63f0200U, 0xd65f03c0U});
    write_words(code, 0x20U, {0xf9400000U, 0xd65f03c0U});
    auto fixture = make_fixture(0x1000U, 0x40U, std::move(code), {manual_seed(0x1000U)});
    REQUIRE(fixture);
    execution::ExecutionSession session(fixture.value().memory, fixture.value().map, {});
    execution::EntrySelection selection;
    selection.kind = execution::EntrySelectionKind::AnalystAddress;
    selection.address = 0x1000U;
    selection.source = "M22 stale diagnostic reset fixture";
    selection.confidence = FunctionConfidence::Manual;

    auto first = session.run(selection);
    REQUIRE(first);
    REQUIRE(first.value().stop_reason == execution::ExecutionStopReason::UnknownGuestFunction);
    const auto refinement = analysis::refine_function_map(
        fixture.value().map, input_for(fixture.value()), observed(0x1020U));
    REQUIRE(refinement);
    REQUIRE(refinement.value().assessment.decision.promoted);
    fixture.value().map = refinement.value().map;

    auto resumed = session.resume_after_refinement(std::move(first).value());
    REQUIRE(resumed);
    REQUIRE(resumed.value().stop_reason == execution::ExecutionStopReason::MemoryFault);
    REQUIRE_FALSE(resumed.value().diagnostic_pc.has_value());
    REQUIRE_FALSE(resumed.value().diagnostic_opcode.has_value());
    REQUIRE(resumed.value().diagnostic_instruction_id.empty());
    REQUIRE(resumed.value().diagnostic_instruction.empty());
}

TEST_CASE("M22 immutable lift artifacts survive deterministic session reruns")
{
    auto code = words({0xd65f03c0U});
    auto fixture = make_fixture(0x1000U, 0x20U, std::move(code), {manual_seed(0x1000U)});
    REQUIRE(fixture);
    execution::ExecutionSession session(fixture.value().memory, fixture.value().map, {});
    execution::EntrySelection selection;
    selection.kind = execution::EntrySelectionKind::AnalystAddress;
    selection.address = 0x1000U;
    selection.source = "M22 deterministic cache fixture";
    selection.confidence = FunctionConfidence::Manual;
    const auto first = session.run(selection);
    const auto second = session.run(selection);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(first.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(second.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(first.value().performance.functions_lifted == 1U);
    REQUIRE(second.value().performance.functions_lifted == 0U);
    REQUIRE(second.value().performance.lift_cache_hits >= 1U);
}

TEST_CASE("M22 display-envelope overlap does not imply precise ownership")
{
    auto code = std::vector<std::byte>(0x20U, std::byte{});
    write_words(code, 0U, {branch(0x1000U, 0x100cU)});
    write_words(code, 0x04U, {0xd503201fU, 0xd65f03c0U});
    write_words(code, 0x0cU, {0xd65f03c0U});
    auto fixture = make_fixture(0x1000U, 0x20U, std::move(code), {manual_seed(0x1000U)});
    REQUIRE(fixture);
    const auto assessment = analysis::assess_indirect_target(
        observed(0x1004U), fixture.value().memory, &fixture.value().map);
    REQUIRE(assessment);
    REQUIRE(assessment.value().validation.ownership ==
            analysis::IndirectTargetOwnership::ExistingDisplayEnvelopeOnly);
    REQUIRE(assessment.value().validation.overlap_ranges.empty());
    REQUIRE(assessment.value().decision.kind == IndirectTargetDecisionKind::TrustedNewEntry);
}

TEST_CASE("M22 alignment, executable mapping, CFG, budget, and evidence gates fail closed")
{
    auto code = std::vector<std::byte>(0x40U, std::byte{});
    write_words(code, 0U, {0xd65f03c0U});
    write_words(code, 0x04U, {0xffffffffU});
    auto fixture = make_fixture(0x1000U, 0x40U, std::move(code), {manual_seed(0x1000U)},
                                std::make_pair<GuestAddress, GuestAddress>(0x2000U, 0x20U));
    REQUIRE(fixture);

    const auto misaligned = analysis::assess_indirect_target(
        observed(0x1001U), fixture.value().memory, &fixture.value().map);
    REQUIRE(misaligned);
    REQUIRE(misaligned.value().decision.kind == IndirectTargetDecisionKind::InvalidAlignment);

    const auto non_executable = analysis::assess_indirect_target(
        observed(0x2000U), fixture.value().memory, &fixture.value().map);
    REQUIRE(non_executable);
    REQUIRE(non_executable.value().decision.kind == IndirectTargetDecisionKind::NonExecutable);

    const auto invalid_cfg = analysis::assess_indirect_target(
        observed(0x1004U), fixture.value().memory, &fixture.value().map);
    REQUIRE(invalid_cfg);
    REQUIRE(invalid_cfg.value().decision.kind == IndirectTargetDecisionKind::AnalysisFailed);

    auto budget_code = std::vector<std::byte>(0x40U, std::byte{});
    write_words(budget_code, 0x20U, {0xd503201fU, 0xd65f03c0U});
    auto budget_fixture = make_fixture(0x1000U, 0x40U, std::move(budget_code),
                                       {manual_seed(0x1000U)});
    REQUIRE(budget_fixture);
    IndirectTargetDiscoveryOptions budget_options;
    budget_options.budgets.max_instructions = 1U;
    const auto budget = analysis::assess_indirect_target(
        observed(0x1020U), budget_fixture.value().memory, &budget_fixture.value().map,
        nullptr, nullptr, budget_options);
    REQUIRE(budget);
    REQUIRE(budget.value().decision.kind == IndirectTargetDecisionKind::BudgetExceeded);

    auto unresolved_code = std::vector<std::byte>(0x40U, std::byte{});
    write_words(unresolved_code, 0x20U, {0xd61f0220U});
    auto unresolved_fixture = make_fixture(0x1000U, 0x40U, std::move(unresolved_code),
                                           {manual_seed(0x1000U)});
    REQUIRE(unresolved_fixture);
    IndirectTargetDiscoveryOptions strict_options;
    strict_options.allow_runtime_cfg_promotion = false;
    const auto unresolved = analysis::assess_indirect_target(
        observed(0x1020U), unresolved_fixture.value().memory, &unresolved_fixture.value().map,
        nullptr, nullptr, strict_options);
    REQUIRE(unresolved);
    REQUIRE(unresolved.value().validation.cfg_status ==
            analysis::IndirectTargetCFGStatus::ValidatedWithUnresolvedFlow);
    REQUIRE(unresolved.value().decision.kind == IndirectTargetDecisionKind::InsufficientEvidence);

    auto relocation_only = observed(0x1020U);
    relocation_only.entry_evidence.push_back(FunctionEntryEvidence{
        FunctionEntryEvidenceKind::RelocatedFunctionPointer,
        FunctionEntryEvidenceStrength::Supporting, "synthetic-m22", 0x9000U,
        "synthetic-m22", 0x1020U, 4U, format::AArch64RelocationType::Relative,
        format::RelocationSource::Rela, 0U, {}, false, true, "rebasing-only relocation"});
    IndirectTargetDiscoveryOptions evidence_options;
    evidence_options.require_independent_static_evidence = true;
    const auto insufficient = analysis::assess_indirect_target(
        relocation_only, budget_fixture.value().memory, &budget_fixture.value().map,
        nullptr, nullptr, evidence_options);
    REQUIRE(insufficient);
    REQUIRE(insufficient.value().decision.kind == IndirectTargetDecisionKind::InsufficientEvidence);
    REQUIRE_FALSE(insufficient.value().certification.certified);
}

TEST_CASE("M22 direct-call boundary reconciliation removes only the proven helper overlap")
{
    auto code = std::vector<std::byte>(0x120U, std::byte{});
    write_words(code, 0x00U, {branch(0x1000U, 0x1040U)});
    write_words(code, 0x40U, {0xd65f03c0U});
    write_words(code, 0x80U, {branch(0x1080U, 0x1040U, true), branch(0x1084U, 0x1040U)});
    // A local loop and conditional diamond prove that ordinary local CFG flow
    // remains intraprocedural while the helper edges become transfers.
    write_words(code, 0x100U, {0x34000060U, branch(0x1104U, 0x1100U), 0xd503201fU,
                               0xd65f03c0U});
    auto fixture = make_fixture(0x1000U, 0x120U, std::move(code),
                                {manual_seed(0x1000U), manual_seed(0x1100U)});
    REQUIRE(fixture);

    const auto before = analysis::assess_indirect_target(
        observed(0x1080U), fixture.value().memory, &fixture.value().map);
    REQUIRE(before);
    REQUIRE(before.value().decision.kind == IndirectTargetDecisionKind::TrustedNewEntry);
    REQUIRE(before.value().validation.boundary_reconciliation.kind ==
            FunctionBoundaryReconciliationKind::AnalyzerOverClaim);
    const auto& reconciliation = before.value().validation.boundary_reconciliation;
    REQUIRE(reconciliation.precise_overlap_ranges ==
            std::vector<analysis::GuestAddressRange>{{0x1040U, 4U}});
    REQUIRE(reconciliation.boundary_entries == std::vector<GuestAddress>{0x1040U});
    REQUIRE(reconciliation.boundary_owned_code_ranges ==
            std::vector<analysis::GuestAddressRange>{{0x1040U, 4U}});
    REQUIRE(reconciliation.candidate_owned_code_ranges_after ==
            std::vector<analysis::GuestAddressRange>{{0x1080U, 8U}});
    REQUIRE(reconciliation.existing_owned_code_ranges_after ==
            std::vector<analysis::GuestAddressRange>{{0x1000U, 4U}});
    REQUIRE(reconciliation.witnesses.size() == 4U);
    REQUIRE(before.value().validation.cfg->blocks.at(0x1084U).successors.front().kind ==
            analysis::EdgeKind::FunctionTransfer);
    REQUIRE_FALSE(before.value().validation.cfg->blocks.at(0x1084U).successors.front().internal);

    const auto refinement = analysis::refine_function_map(
        fixture.value().map, input_for(fixture.value()), observed(0x1080U));
    REQUIRE(refinement);
    REQUIRE(refinement.value().assessment.decision.promoted);
    REQUIRE(refinement.value().map.conflicts().empty());
    const auto* helper = refinement.value().map.find_canonical_entry(0x1040U);
    const auto* existing = refinement.value().map.find_canonical_entry(0x1000U);
    const auto* candidate = refinement.value().map.find_canonical_entry(0x1080U);
    REQUIRE(helper != nullptr);
    REQUIRE(existing != nullptr);
    REQUIRE(candidate != nullptr);
    REQUIRE(helper->owned_code_ranges == reconciliation.boundary_owned_code_ranges);
    REQUIRE(existing->owned_code_ranges == reconciliation.existing_owned_code_ranges_after);
    REQUIRE(candidate->owned_code_ranges == reconciliation.candidate_owned_code_ranges_after);
    REQUIRE(refinement.value().map.find_precise_owners(0x1040U).size() == 1U);
    REQUIRE(refinement.value().map.find_precise_owners(0x1040U).front() == helper);
    REQUIRE(existing->cfg->blocks.at(0x1000U).successors.front().kind ==
            analysis::EdgeKind::FunctionTransfer);
    REQUIRE(candidate->cfg->blocks.at(0x1084U).successors.front().kind ==
            analysis::EdgeKind::FunctionTransfer);
    REQUIRE(refinement.value().map.find_canonical_entry(0x1100U)->cfg->blocks.at(0x1104U)
                .successors.front()
                .kind == analysis::EdgeKind::Branch);
    REQUIRE(refinement.value().map.find_canonical_entry(0x1100U)->cfg->blocks.at(0x1104U)
                .successors.front()
                .internal);

    execution::ExecutionSession session(fixture.value().memory, refinement.value().map, {});
    execution::EntrySelection selection;
    selection.kind = execution::EntrySelectionKind::AnalystAddress;
    selection.address = 0x1080U;
    selection.source = "M22 synthetic alternate run";
    selection.confidence = FunctionConfidence::Manual;
    const auto run = session.run(selection);
    REQUIRE(run);
    REQUIRE(run.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(run.value().direct_calls == 1U);
    REQUIRE(run.value().function_transfers == 1U);
    REQUIRE(run.value().returns == 2U);
    REQUIRE(run.value().maximum_call_depth == 1U);
    REQUIRE(run.value().executed_functions.front() == 0x1080U);
    REQUIRE(run.value().executed_functions[1] == 0x1040U);
}

TEST_CASE("M22 boundary reconciliation evidence ordering is deterministic")
{
    auto code = std::vector<std::byte>(0x120U, std::byte{});
    write_words(code, 0x00U, {branch(0x1000U, 0x1040U)});
    write_words(code, 0x40U, {0xd65f03c0U});
    write_words(code, 0x80U, {branch(0x1080U, 0x1040U, true), branch(0x1084U, 0x1040U)});
    auto first_fixture = make_fixture(0x1000U, 0x120U, code,
                                      {manual_seed(0x1000U)});
    auto second_fixture = make_fixture(0x1000U, 0x120U, std::move(code),
                                       {manual_seed(0x1000U)});
    REQUIRE(first_fixture);
    REQUIRE(second_fixture);
    const auto first = analysis::assess_indirect_target(
        observed(0x1080U), first_fixture.value().memory, &first_fixture.value().map);
    const auto second = analysis::assess_indirect_target(
        observed(0x1080U), second_fixture.value().memory, &second_fixture.value().map);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(first.value().validation.boundary_reconciliation.witnesses ==
            second.value().validation.boundary_reconciliation.witnesses);
    REQUIRE(first.value().validation.boundary_reconciliation.candidate_owned_code_ranges_after ==
            second.value().validation.boundary_reconciliation.candidate_owned_code_ranges_after);
}

} // namespace
