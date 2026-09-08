#include "switchrecomp/analysis/indirect_target.hpp"
#include "switchrecomp/execution/session.hpp"
#include "switchrecomp/version.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace switchrecomp;
using analysis::IndirectControlFlowKind;
using analysis::IndirectTargetDecisionKind;
using analysis::IndirectTargetDiscoveryOptions;
using analysis::IndirectTargetPointerProvenanceKind;
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

[[nodiscard]] std::uint32_t movz(std::uint8_t reg, std::uint16_t value)
{
    return 0xd2800000U | (static_cast<std::uint32_t>(value) << 5U) | reg;
}

[[nodiscard]] std::uint32_t branch(GuestAddress pc, GuestAddress target)
{
    const auto displacement = static_cast<std::int64_t>(target) -
                              static_cast<std::int64_t>(pc);
    return 0x14000000U |
           (static_cast<std::uint32_t>(displacement / 4) & 0x03ffffffU);
}

[[nodiscard]] std::vector<std::byte> blank_code(GuestAddress size)
{
    return std::vector<std::byte>(static_cast<std::size_t>(size), std::byte{0});
}

void write_words(std::vector<std::byte>& image, std::size_t byte_offset,
                 std::initializer_list<std::uint32_t> values)
{
    const auto bytes = words(values);
    REQUIRE(byte_offset <= image.size());
    REQUIRE(bytes.size() <= image.size() - byte_offset);
    std::copy(bytes.begin(), bytes.end(), image.begin() +
                                        static_cast<std::ptrdiff_t>(byte_offset));
}

[[nodiscard]] analysis::FunctionSeed manual_seed(GuestAddress address)
{
    return analysis::FunctionSeed{address, analysis::FunctionDiscoverySource::AnalystSeed,
                                  analysis::FunctionConfidence::Manual, std::nullopt, std::nullopt,
                                  "M18 synthetic seed"};
}

struct Fixture
{
    memory::GuestMemory memory;
    analysis::ModuleIdentity identity;
    analysis::FinalizedFunctionMap map;
};

[[nodiscard]] Result<Fixture> make_fixture(GuestAddress base, GuestAddress size,
                                            std::vector<std::byte> code,
                                            std::vector<analysis::FunctionSeed> seeds,
                                            std::optional<std::pair<GuestAddress, GuestAddress>> data =
                                                std::nullopt,
                                            std::string module_name = "synthetic-m18")
{
    Fixture fixture;
    if (code.size() < size) code.resize(static_cast<std::size_t>(size), std::byte{0});
    const auto mapped = fixture.memory.map(
        base, std::span<const std::byte>(code.data(), code.size()),
        memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Execute,
        "synthetic.m18.text", memory::GuestRegionKind::Text);
    if (!mapped) return Result<Fixture>::failure(mapped.error());
    if (data)
    {
        const auto data_mapped = fixture.memory.map(
            data->first, data->second,
            memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Write,
            "synthetic.m18.data", memory::GuestRegionKind::Data);
        if (!data_mapped) return Result<Fixture>::failure(data_mapped.error());
    }
    fixture.identity.module = std::move(module_name);
    fixture.identity.build_id = "synthetic-m18-build";
    fixture.identity.input_sha256 = "synthetic-m18-sha";
    fixture.identity.guest_base = base;
    fixture.identity.translator_version = version;
    fixture.identity.executable_ranges.push_back(
        analysis::GuestAddressRange{base, size});
    fixture.identity.entry_points.push_back(analysis::EntryPointEvidence{
        base, analysis::EntryPointKind::DynamicInit, "synthetic DT_INIT",
        analysis::FunctionConfidence::High, false, "controlled synthetic entry"});
    const auto finalized = analysis::FunctionMapBuilder::build(
        analysis::ModuleAnalysisInput{fixture.identity, &fixture.memory, std::move(seeds)});
    if (!finalized) return Result<Fixture>::failure(finalized.error());
    fixture.map = std::move(finalized).value();
    return Result<Fixture>::success(std::move(fixture));
}

[[nodiscard]] ObservedIndirectTarget observed(GuestAddress target)
{
    ObservedIndirectTarget result;
    result.source_module = "synthetic-m18";
    result.source_function = 0x1000U;
    result.source_pc = 0x1004U;
    result.control_flow = IndirectControlFlowKind::Branch;
    result.target_register = "x17";
    result.target = target;
    result.pointer_provenance = IndirectTargetPointerProvenanceKind::GuestLoad;
    result.guest_load_address = 0x8000U;
    return result;
}

[[nodiscard]] Result<execution::ExecutionSessionResult> run(
    Fixture& fixture, GuestAddress entry,
    execution::ExecutionSessionOptions options = {})
{
    execution::ExecutionSession session(fixture.memory, fixture.map, {}, std::move(options));
    const auto selected = execution::select_entry(
        fixture.map.identity(), execution::EntrySelectionKind::DynamicInit);
    if (!selected) return Result<execution::ExecutionSessionResult>::failure(selected.error());
    auto selection = selected.value();
    selection.address = entry;
    return session.run(selection);
}

} // namespace

TEST_CASE("M18 existing trusted indirect target is accepted without a duplicate")
{
    auto code = blank_code(0x40U);
    write_words(code, 0U, {movz(16U, 0x1020U), 0xd61f0200U, 0xd65f03c0U});
    write_words(code, 0x20U, {0xd65f03c0U});
    auto fixture = make_fixture(0x1000U, 0x40U, std::move(code),
                                {manual_seed(0x1000U), manual_seed(0x1020U)});
    REQUIRE(fixture);
    const auto assessment = analysis::assess_indirect_target(
        observed(0x1020U), fixture.value().memory, &fixture.value().map);
    REQUIRE(assessment);
    REQUIRE(assessment.value().decision.kind == IndirectTargetDecisionKind::TrustedExistingEntry);
    REQUIRE(assessment.value().validation.existing_canonical_entry == 0x1020U);
    REQUIRE(fixture.value().map.functions().size() == 2U);
}

TEST_CASE("M18 existing secondary entry is reported as an alias")
{
    auto code = blank_code(0x20U);
    write_words(code, 0U, {0xd503201fU, 0xd65f03c0U});
    const auto canonical = manual_seed(0x1000U);
    const auto secondary = analysis::FunctionSeed{
        0x1004U, analysis::FunctionDiscoverySource::AnalystSeed,
        analysis::FunctionConfidence::Manual, 0x1000U, std::nullopt,
        "M18 synthetic secondary entry"};
    auto fixture = make_fixture(0x1000U, 0x20U, std::move(code), {canonical, secondary});
    REQUIRE(fixture);
    const auto assessment = analysis::assess_indirect_target(
        observed(0x1004U), fixture.value().memory, &fixture.value().map);
    REQUIRE(assessment);
    REQUIRE(assessment.value().validation.ownership ==
            analysis::IndirectTargetOwnership::ExistingSecondaryEntry);
    REQUIRE(assessment.value().decision.kind == IndirectTargetDecisionKind::AliasOfExistingEntry);
    REQUIRE(assessment.value().decision.canonical_entry == 0x1000U);
}

TEST_CASE("M18 valid new indirect target is validated and immutably promoted")
{
    auto code = blank_code(0x2020U);
    write_words(code, 0U, {movz(16U, 0x3000U), 0xd61f0200U});
    write_words(code, 0x2000U, {movz(0U, 7U), 0xd65f03c0U});
    auto fixture = make_fixture(0x1000U, 0x2020U, std::move(code),
                                {manual_seed(0x1000U)});
    REQUIRE(fixture);
    const auto original_count = fixture.value().map.functions().size();
    auto refined = analysis::refine_function_map(
        fixture.value().map,
        analysis::ModuleAnalysisInput{fixture.value().identity, &fixture.value().memory,
                                      {manual_seed(0x1000U)}},
        observed(0x3000U));
    REQUIRE(refined);
    REQUIRE(refined.value().assessment.decision.kind == IndirectTargetDecisionKind::TrustedNewEntry);
    REQUIRE(refined.value().assessment.decision.promoted);
    REQUIRE(refined.value().assessment.decision.canonical_entry == 0x3000U);
    REQUIRE(refined.value().map.frozen());
    REQUIRE(fixture.value().map.functions().size() == original_count);
    REQUIRE(refined.value().map.find(0x3000U) != nullptr);
    REQUIRE(refined.value().map.find(0x3000U)->primary_source ==
            analysis::FunctionDiscoverySource::ObservedIndirectTarget);
}

TEST_CASE("M18 non-executable, misaligned, and unmapped targets fail closed")
{
    auto fixture = make_fixture(0x1000U, 0x40U, words({0xd65f03c0U}),
                                {manual_seed(0x1000U)}, std::make_pair(0x2000U, 0x20U));
    REQUIRE(fixture);
    auto non_executable = analysis::assess_indirect_target(
        observed(0x2000U), fixture.value().memory, &fixture.value().map);
    REQUIRE(non_executable);
    REQUIRE(non_executable.value().decision.kind == IndirectTargetDecisionKind::NonExecutable);
    auto misaligned = analysis::assess_indirect_target(
        observed(0x1001U), fixture.value().memory, &fixture.value().map);
    REQUIRE(misaligned);
    REQUIRE(misaligned.value().decision.kind == IndirectTargetDecisionKind::InvalidAlignment);
    auto unmapped = analysis::assess_indirect_target(
        observed(0x4000U), fixture.value().memory, &fixture.value().map);
    REQUIRE(unmapped);
    REQUIRE(unmapped.value().decision.kind == IndirectTargetDecisionKind::Unmapped);
}

TEST_CASE("M18 executable targets without a unique module are rejected")
{
    auto first = make_fixture(0x1000U, 0x40U, words({0xd65f03c0U}),
                              {manual_seed(0x1000U)});
    auto second = make_fixture(0x1000U, 0x40U, words({0xd503201fU, 0xd65f03c0U}),
                               {manual_seed(0x1004U)}, std::nullopt, "synthetic-m18-b");
    REQUIRE(first);
    REQUIRE(second);
    std::vector<analysis::FinalizedFunctionMap> maps;
    maps.push_back(std::move(first.value().map));
    maps.push_back(std::move(second.value().map));
    const auto process_map = analysis::ProcessFunctionMap::build(std::move(maps));
    REQUIRE(process_map);
    const auto assessment = analysis::assess_indirect_target(
        observed(0x1020U), first.value().memory, nullptr, &process_map.value());
    REQUIRE(assessment);
    REQUIRE(assessment.value().decision.kind ==
            IndirectTargetDecisionKind::CrossModuleAmbiguity);
}

TEST_CASE("M18 precise ownership rejects a target inside an existing function")
{
    auto fixture = make_fixture(0x1000U, 0x20U,
                                words({0xd503201fU, 0xd503201fU, 0xd65f03c0U}),
                                {manual_seed(0x1000U)});
    REQUIRE(fixture);
    const auto assessment = analysis::assess_indirect_target(
        observed(0x1004U), fixture.value().memory, &fixture.value().map);
    REQUIRE(assessment);
    REQUIRE(assessment.value().validation.ownership ==
            analysis::IndirectTargetOwnership::InsideExistingFunction);
    REQUIRE(assessment.value().decision.kind == IndirectTargetDecisionKind::InsideExistingFunction);
}

TEST_CASE("M18 candidate CFG overlap is a typed ownership conflict")
{
    auto fixture = make_fixture(0x1000U, 0x40U,
                                words({0xd65f03c0U, 0xd503201fU, 0xd503201fU,
                                       0xd503201fU, 0xd65f03c0U}),
                                {manual_seed(0x1000U), manual_seed(0x1010U)});
    REQUIRE(fixture);
    const auto assessment = analysis::assess_indirect_target(
        observed(0x1004U), fixture.value().memory, &fixture.value().map);
    REQUIRE(assessment);
    REQUIRE(assessment.value().decision.kind == IndirectTargetDecisionKind::FunctionBoundaryConflict);
    REQUIRE_FALSE(assessment.value().validation.overlap_ranges.empty());
}

TEST_CASE("M18 display envelopes do not replace precise ownership")
{
    auto fixture = make_fixture(0x1000U, 0x20U,
                                words({branch(0x1000U, 0x100cU), movz(0U, 1U),
                                       0xd65f03c0U, 0xd65f03c0U}),
                                {manual_seed(0x1000U)});
    REQUIRE(fixture);
    const auto assessment = analysis::assess_indirect_target(
        observed(0x1004U), fixture.value().memory, &fixture.value().map);
    REQUIRE(assessment);
    REQUIRE(assessment.value().validation.ownership ==
            analysis::IndirectTargetOwnership::ExistingDisplayEnvelopeOnly);
    REQUIRE(assessment.value().decision.kind == IndirectTargetDecisionKind::TrustedNewEntry);
}

TEST_CASE("M18 malformed candidates and exhausted CFG budgets fail closed")
{
    auto malformed = make_fixture(0x1000U, 0x20U,
                                  words({0xd65f03c0U, 0xffffffffU}),
                                  {manual_seed(0x1000U)});
    REQUIRE(malformed);
    const auto failed = analysis::assess_indirect_target(
        observed(0x1004U), malformed.value().memory, &malformed.value().map);
    REQUIRE(failed);
    REQUIRE(failed.value().decision.kind == IndirectTargetDecisionKind::AnalysisFailed);

    auto limited = make_fixture(0x1000U, 0x40U,
                                words({0xd65f03c0U, 0xd503201fU, 0xd503201fU,
                                       0xd503201fU, 0xd503201fU, 0xd503201fU}),
                                {manual_seed(0x1000U)});
    REQUIRE(limited);
    IndirectTargetDiscoveryOptions options;
    options.budgets.max_instructions = 1U;
    const auto budget = analysis::assess_indirect_target(
        observed(0x1004U), limited.value().memory, &limited.value().map, nullptr, nullptr,
        options);
    REQUIRE(budget);
    REQUIRE(budget.value().decision.kind == IndirectTargetDecisionKind::BudgetExceeded);
    REQUIRE(budget.value().validation.cfg_status == analysis::IndirectTargetCFGStatus::BudgetExceeded);
}

TEST_CASE("M18 structurally valid unresolved flow remains explicit")
{
    auto code = blank_code(0x20U);
    write_words(code, 0U, {0xd65f03c0U, 0xd61f0220U});
    auto fixture = make_fixture(0x1000U, 0x20U, std::move(code), {manual_seed(0x1000U)});
    REQUIRE(fixture);
    const auto assessment = analysis::assess_indirect_target(
        observed(0x1004U), fixture.value().memory, &fixture.value().map);
    REQUIRE(assessment);
    REQUIRE(assessment.value().validation.cfg_status ==
            analysis::IndirectTargetCFGStatus::ValidatedWithUnresolvedFlow);
    REQUIRE(assessment.value().decision.kind == IndirectTargetDecisionKind::TrustedNewEntry);
    REQUIRE(assessment.value().validation.unresolved_control_flow.size() == 1U);
}

TEST_CASE("M18 deterministic ordering is independent of observation insertion order")
{
    auto first = observed(0x3000U);
    first.target_module = "b";
    first.source_pc = 0x2000U;
    auto second = observed(0x2000U);
    second.target_module = "a";
    second.source_pc = 0x1000U;
    std::vector<ObservedIndirectTarget> left{first, second};
    std::vector<ObservedIndirectTarget> right{second, first};
    analysis::sort_observed_indirect_targets(left);
    analysis::sort_observed_indirect_targets(right);
    REQUIRE(left == right);
    REQUIRE(left.front().target == 0x2000U);
}

TEST_CASE("M18 promoted BR preserves tail-transfer depth and guest address state")
{
    auto code = blank_code(0x2020U);
    write_words(code, 0U, {movz(16U, 0x3000U), 0xd61f0200U});
    write_words(code, 0x2000U, {movz(0U, 7U), 0xd65f03c0U});
    auto fixture = make_fixture(0x1000U, 0x2020U, std::move(code),
                                {manual_seed(0x1000U)});
    REQUIRE(fixture);
    const auto refined = analysis::refine_function_map(
        fixture.value().map,
        analysis::ModuleAnalysisInput{fixture.value().identity, &fixture.value().memory,
                                      {manual_seed(0x1000U)}},
        observed(0x3000U));
    REQUIRE(refined);
    fixture.value().map = refined.value().map;
    const auto result = run(fixture.value(), 0x1000U);
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(result.value().function_transfers == 1U);
    REQUIRE(result.value().maximum_call_depth == 0U);
    REQUIRE(result.value().final_cpu.x[30U] == result.value().synthetic_lr_sentinel);
    REQUIRE(result.value().executed_functions.back() == 0x3000U);
}

TEST_CASE("M18 promoted BLR preserves call and return semantics")
{
    auto code = blank_code(0x2020U);
    write_words(code, 0U, {movz(16U, 0x3000U), 0xd63f0200U,
                           0x91000400U, 0xd65f03c0U});
    write_words(code, 0x2000U, {movz(0U, 7U), 0xd65f03c0U});
    auto fixture = make_fixture(0x1000U, 0x2020U, std::move(code),
                                {manual_seed(0x1000U)});
    REQUIRE(fixture);
    const auto refined = analysis::refine_function_map(
        fixture.value().map,
        analysis::ModuleAnalysisInput{fixture.value().identity, &fixture.value().memory,
                                      {manual_seed(0x1000U)}},
        observed(0x3000U));
    REQUIRE(refined);
    fixture.value().map = refined.value().map;
    const auto result = run(fixture.value(), 0x1000U);
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(result.value().indirect_calls == 1U);
    REQUIRE(result.value().returns == 2U);
    REQUIRE(result.value().maximum_call_depth == 1U);
    REQUIRE(result.value().final_cpu.x[0] == 8U);
}

TEST_CASE("M18 guest target evidence never becomes a host pointer")
{
    auto code = blank_code(0x2020U);
    write_words(code, 0U, {movz(16U, 0x3000U), 0xd61f0200U});
    write_words(code, 0x2000U, {movz(0U, 7U), 0xd65f03c0U});
    auto fixture = make_fixture(0x1000U, 0x2020U, std::move(code),
                                {manual_seed(0x1000U)});
    REQUIRE(fixture);
    const auto assessment = analysis::assess_indirect_target(
        observed(0x3000U), fixture.value().memory, &fixture.value().map);
    REQUIRE(assessment);
    REQUIRE(assessment.value().observed.target == 0x3000U);
    REQUIRE(assessment.value().validation.target_module_base == 0x1000U);
    REQUIRE(assessment.value().observed.guest_load_address == 0x8000U);
}

TEST_CASE("M18 static evidence can require more than runtime observation")
{
    auto fixture = make_fixture(0x1000U, 0x20U,
                                words({0xd65f03c0U, 0xd65f03c0U}),
                                {manual_seed(0x1000U)});
    REQUIRE(fixture);
    auto candidate = observed(0x1004U);
    IndirectTargetDiscoveryOptions options;
    options.require_independent_static_evidence = true;
    const auto assessment = analysis::assess_indirect_target(
        candidate, fixture.value().memory, &fixture.value().map, nullptr, nullptr, options);
    REQUIRE(assessment);
    REQUIRE(assessment.value().decision.kind == IndirectTargetDecisionKind::InsufficientEvidence);
    REQUIRE_FALSE(assessment.value().decision.eligible_for_promotion);
}

TEST_CASE("M18 execution reports retain indirect target provenance and schema")
{
    auto code = blank_code(0x2020U);
    write_words(code, 0U, {movz(16U, 0x3000U), 0xd61f0200U});
    write_words(code, 0x2000U, {movz(0U, 7U), 0xd65f03c0U});
    auto fixture = make_fixture(0x1000U, 0x2020U, std::move(code),
                                {manual_seed(0x1000U)});
    REQUIRE(fixture);
    const auto refined = analysis::refine_function_map(
        fixture.value().map,
        analysis::ModuleAnalysisInput{fixture.value().identity, &fixture.value().memory,
                                      {manual_seed(0x1000U)}},
        observed(0x3000U));
    REQUIRE(refined);
    fixture.value().map = refined.value().map;
    const auto result = run(fixture.value(), 0x1000U);
    REQUIRE(result);
    REQUIRE(execution::ExecutionSessionResult::schema_version == 7U);
    const auto report = execution::render_execution_report_json(result.value());
    REQUIRE(report.find("indirect_target_discovery") != std::string::npos);
    REQUIRE(report.find("0x0000000000003000") != std::string::npos);
}
