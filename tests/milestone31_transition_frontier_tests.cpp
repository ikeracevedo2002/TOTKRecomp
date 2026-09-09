#include "switchrecomp/execution/session.hpp"
#include "switchrecomp/version.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace switchrecomp;

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

[[nodiscard]] std::vector<std::byte> words_from_u32(const std::vector<std::uint32_t>& values)
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

[[nodiscard]] std::uint32_t bl(memory::GuestAddress pc, memory::GuestAddress target)
{
    const auto displacement = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pc);
    return 0x94000000U | (static_cast<std::uint32_t>(displacement / 4) & 0x03ffffffU);
}

[[nodiscard]] std::uint32_t branch(memory::GuestAddress pc, memory::GuestAddress target)
{
    const auto displacement = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pc);
    return 0x14000000U | (static_cast<std::uint32_t>(displacement / 4) & 0x03ffffffU);
}

struct Fixture
{
    memory::GuestMemory memory;
    analysis::FinalizedFunctionMap function_map;
};

[[nodiscard]] analysis::FunctionSeed seed(memory::GuestAddress address)
{
    return analysis::FunctionSeed{address, analysis::FunctionDiscoverySource::AnalystSeed,
                                  analysis::FunctionConfidence::Manual, std::nullopt, std::nullopt,
                                  "synthetic M31 function"};
}

[[nodiscard]] Result<Fixture> make_fixture(
    memory::GuestAddress base, std::span<const std::byte> code,
    std::vector<analysis::FunctionSeed> seeds)
{
    Fixture fixture;
    const auto mapped = fixture.memory.map(
        base, code, memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Execute,
        "synthetic.m31.text", memory::GuestRegionKind::Text);
    if (!mapped) return Result<Fixture>::failure(mapped.error());
    analysis::ModuleIdentity identity;
    identity.module = "synthetic-m31";
    identity.build_id = "synthetic-build";
    identity.input_sha256 = "synthetic-sha256";
    identity.guest_base = base;
    identity.translator_version = version;
    identity.executable_ranges.push_back(
        analysis::GuestAddressRange{base, static_cast<memory::GuestSize>(code.size())});
    identity.entry_points.push_back(analysis::EntryPointEvidence{
        base, analysis::EntryPointKind::DynamicInit, "synthetic DT_INIT",
        analysis::FunctionConfidence::High, false, "synthetic controlled entry"});
    const auto map = analysis::FunctionMapBuilder::build(
        analysis::ModuleAnalysisInput{std::move(identity), &fixture.memory, std::move(seeds)});
    if (!map) return Result<Fixture>::failure(map.error());
    fixture.function_map = std::move(map).value();
    return Result<Fixture>::success(std::move(fixture));
}

[[nodiscard]] Result<execution::ExecutionSessionResult> run_fixture(
    Fixture& fixture, memory::GuestAddress entry,
    execution::ExecutionSessionOptions options = {})
{
    execution::ExecutionSession session(fixture.memory, fixture.function_map, {},
                                        std::move(options));
    const auto selected = execution::select_entry(
        fixture.function_map.identity(), execution::EntrySelectionKind::DynamicInit);
    if (!selected) return Result<execution::ExecutionSessionResult>::failure(selected.error());
    auto selection = selected.value();
    selection.address = entry;
    return session.run(selection);
}

[[nodiscard]] Fixture repeated_call_fixture(std::size_t count)
{
    constexpr memory::GuestAddress base = 0x10000U;
    constexpr memory::GuestAddress helper_base = base + 0x100U;
    std::vector<std::uint32_t> code(
        static_cast<std::size_t>((helper_base - base) / 4U) + count * 4U,
        0xd503201fU);
    std::vector<analysis::FunctionSeed> seeds{seed(base)};
    for (std::size_t index = 0U; index < count; ++index)
    {
        const auto call_pc = base + index * 4U;
        const auto helper = helper_base + index * 0x10U;
        code[index] = bl(call_pc, helper);
        const auto helper_index = static_cast<std::size_t>((helper - base) / 4U);
        code[helper_index] = movz(0U, static_cast<std::uint16_t>(index + 1U));
        code[helper_index + 1U] = 0xd65f03c0U;
        seeds.push_back(seed(helper));
    }
    code[count] = 0xd65f03c0U;
    auto fixture = make_fixture(base, words_from_u32(code), std::move(seeds));
    REQUIRE(fixture);
    return std::move(fixture).value();
}

[[nodiscard]] std::size_t category_sum(const execution::FunctionTransitionAccounting& accounting)
{
    return accounting.initial_entries + accounting.direct_call_entries +
           accounting.indirect_call_entries + accounting.function_transfer_entries +
           accounting.function_resume_entries + accounting.other_entries;
}

} // namespace

TEST_CASE("M31 exact direct-call accounting records one entry without transfer charge")
{
    constexpr memory::GuestAddress base = 0x2000U;
    const auto code = words({bl(base, base + 0x10U), 0x91000400U, 0xd65f03c0U,
                             0xd503201fU, movz(0U, 7U), 0xd65f03c0U});
    auto fixture = make_fixture(base, code, {seed(base), seed(base + 0x10U)});
    REQUIRE(fixture);
    const auto result = run_fixture(fixture.value(), base);
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    const auto& accounting = result.value().transition_accounting;
    REQUIRE(accounting.total_charged == 0U);
    REQUIRE(accounting.total_function_entries == 2U);
    REQUIRE(category_sum(accounting) == accounting.total_function_entries);
    REQUIRE(accounting.initial_entries == 1U);
    REQUIRE(accounting.direct_call_entries == 1U);
    REQUIRE(accounting.indirect_call_entries == 0U);
    REQUIRE(accounting.function_transfer_entries == 0U);
    REQUIRE(accounting.history.size() == result.value().executed_functions.size());
    REQUIRE(accounting.history[1U].category == execution::FunctionTransitionCategory::DirectCall);
    REQUIRE(!accounting.history[1U].resource_charged);
    REQUIRE(accounting.history[1U].source_guest_pc == base);
    REQUIRE(accounting.history[1U].target_function == base + 0x10U);
    REQUIRE(accounting.history[1U].call_depth_before == 0U);
    REQUIRE(accounting.history[1U].call_depth_after == 1U);
    REQUIRE(accounting.history[1U].call_frame_pushed);
    REQUIRE(accounting.history[1U].link_register_action ==
            execution::FunctionTransitionLinkRegisterAction::WrittenArchitecturalReturnPc);
    REQUIRE(accounting.history[1U].expected_return_pc == base + 4U);
    REQUIRE(accounting.history[1U].target_ownership ==
            execution::FunctionTransitionTargetOwnership::ExactTrustedFunctionEntry);
    REQUIRE(accounting.history[1U].source_pc_owned_by_source_function);
    REQUIRE(accounting.history[1U].canonical_boundary_valid);
}

TEST_CASE("M31 indirect certified call records exactly one target entry")
{
    constexpr memory::GuestAddress base = 0x3000U;
    const auto code = words({movz(16U, static_cast<std::uint16_t>(base + 0x10U)),
                             0xd63f0200U, 0x91000400U, 0xd65f03c0U,
                             0x91000800U, 0xd65f03c0U});
    auto fixture = make_fixture(base, code, {seed(base), seed(base + 0x10U)});
    REQUIRE(fixture);
    const auto result = run_fixture(fixture.value(), base);
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    const auto& accounting = result.value().transition_accounting;
    REQUIRE(accounting.indirect_call_entries == 1U);
    REQUIRE(accounting.total_charged == 0U);
    REQUIRE(accounting.total_function_entries == 2U);
    REQUIRE(accounting.history[1U].category == execution::FunctionTransitionCategory::IndirectCall);
    REQUIRE(accounting.history[1U].target_ownership ==
            execution::FunctionTransitionTargetOwnership::ExactTrustedFunctionEntry);
    REQUIRE(accounting.history[1U].target_previously_entered == false);
}

TEST_CASE("M31 return resume restores a frame without a fresh transition")
{
    constexpr memory::GuestAddress base = 0x4000U;
    const auto code = words({bl(base, base + 0x10U), 0x91000400U, 0xd65f03c0U,
                             0x91000800U, 0xd65f03c0U});
    auto fixture = make_fixture(base, code, {seed(base), seed(base + 0x10U)});
    REQUIRE(fixture);
    const auto result = run_fixture(fixture.value(), base);
    REQUIRE(result);
    REQUIRE(result.value().returns == 2U);
    REQUIRE(result.value().events.back().kind == execution::ExecutionEventKind::SessionStop);
    REQUIRE(result.value().transition_accounting.total_charged == 0U);
    REQUIRE(result.value().transition_accounting.total_function_entries == 2U);
    REQUIRE(result.value().transition_accounting.function_resume_entries == 0U);
    REQUIRE(result.value().transition_accounting.history.size() == 2U);
    REQUIRE(result.value().resumes == 0U);
}

TEST_CASE("M31 resumable IR slices do not duplicate one activation")
{
    constexpr memory::GuestAddress base = 0x5000U;
    std::vector<std::uint32_t> code;
    for (std::uint16_t value = 0U; value < 12U; ++value)
        code.push_back(movz(0U, value));
    code.push_back(0xd65f03c0U);
    auto fixture = make_fixture(base, words_from_u32(code), {seed(base)});
    REQUIRE(fixture);
    execution::ExecutionSessionOptions options;
    options.budgets.slice_ir_operations = 1U;
    const auto result = run_fixture(fixture.value(), base, options);
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(result.value().resumable_yields > 1U);
    REQUIRE(result.value().resumes > 0U);
    REQUIRE(result.value().transition_accounting.total_charged == 0U);
    REQUIRE(result.value().transition_accounting.total_function_entries == 1U);
    REQUIRE(result.value().transition_accounting.history.size() == 1U);
    REQUIRE(result.value().transition_accounting.initial_entries == 1U);
    REQUIRE(result.value().transition_accounting.function_resume_entries == 0U);
}

TEST_CASE("M31 mid-block resumes do not duplicate a function charge")
{
    constexpr memory::GuestAddress base = 0x6000U;
    const auto code = words({movz(0U, 1U), 0x91000400U, 0x91000400U, 0xd65f03c0U});
    auto fixture = make_fixture(base, code, {seed(base)});
    REQUIRE(fixture);
    execution::ExecutionSessionOptions options;
    options.budgets.slice_ir_operations = 1U;
    const auto result = run_fixture(fixture.value(), base, options);
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(result.value().mid_block_resumes > 0U);
    REQUIRE(result.value().transition_accounting.total_charged == 0U);
    REQUIRE(result.value().transition_accounting.total_function_entries == 1U);
    REQUIRE(result.value().transition_accounting.history.front().category ==
            execution::FunctionTransitionCategory::InitialEntry);
}

TEST_CASE("M31 true tail transfer preserves X30 and pushes no call frame")
{
    constexpr memory::GuestAddress base = 0x7000U;
    const auto code = words({branch(base, base + 0x10U), 0xd503201fU, 0xd503201fU,
                             0xd503201fU, movz(0U, 5U), 0xd65f03c0U});
    auto fixture = make_fixture(base, code, {seed(base), seed(base + 0x10U)});
    REQUIRE(fixture);
    const auto result = run_fixture(fixture.value(), base);
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    const auto& transition = result.value().transition_accounting.history[1U];
    REQUIRE(transition.category == execution::FunctionTransitionCategory::FunctionTransfer);
    REQUIRE(transition.link_register_action ==
            execution::FunctionTransitionLinkRegisterAction::Preserved);
    REQUIRE(!transition.call_frame_pushed);
    REQUIRE(transition.call_depth_before == 0U);
    REQUIRE(transition.call_depth_after == 0U);
    REQUIRE(transition.expected_return_pc == result.value().synthetic_lr_sentinel);
    REQUIRE(transition.resource_charged);
    REQUIRE(transition.resource_sequence == 1U);
    REQUIRE(transition.canonical_boundary_valid);
    REQUIRE(result.value().final_cpu.x[30U] == result.value().synthetic_lr_sentinel);
}

TEST_CASE("M31 intra-function branch remains ordinary CFG flow")
{
    constexpr memory::GuestAddress base = 0x8000U;
    const auto code = words({branch(base, base + 0x8U), 0xd503201fU,
                             movz(0U, 9U), 0xd65f03c0U});
    auto fixture = make_fixture(base, code, {seed(base)});
    REQUIRE(fixture);
    const auto result = run_fixture(fixture.value(), base);
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(result.value().function_transfers == 0U);
    REQUIRE(result.value().transition_accounting.total_charged == 0U);
    REQUIRE(result.value().transition_accounting.total_function_entries == 1U);
    REQUIRE(result.value().transition_accounting.function_transfer_entries == 0U);
    REQUIRE(result.value().transition_accounting.history.size() == 1U);
}

TEST_CASE("M31 shallow repeated calls retain exact finite transition semantics")
{
    auto fixture = repeated_call_fixture(4U);
    execution::ExecutionSessionOptions options;
    options.budgets.max_function_transitions = 3U;
    const auto result = run_fixture(fixture, 0x10000U, options);
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(result.value().transition_accounting.total_charged == 0U);
    REQUIRE(result.value().transition_accounting.total_function_entries == 5U);
    REQUIRE(result.value().transition_accounting.direct_call_entries == 4U);
    REQUIRE(result.value().direct_calls == 4U);
    REQUIRE(!result.value().transition_accounting.terminal_attempt.has_value());
}

TEST_CASE("M31 pathological tail-transfer cycle remains finitely bounded")
{
    constexpr memory::GuestAddress base = 0x9000U;
    const auto code = words({branch(base, base + 0x10U), 0xd503201fU, 0xd503201fU,
                             0xd503201fU, branch(base + 0x10U, base)});
    auto fixture = make_fixture(base, code, {seed(base), seed(base + 0x10U)});
    REQUIRE(fixture);
    execution::ExecutionSessionOptions options;
    options.budgets.max_function_transitions = 5U;
    const auto result = run_fixture(fixture.value(), base, options);
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::FunctionTransitionLimitExceeded);
    REQUIRE(result.value().transition_accounting.total_charged == 5U);
    REQUIRE(result.value().transition_accounting.function_transfer_entries == 5U);
    REQUIRE(result.value().transition_accounting.maximum_edge_repetition == 3U);
    REQUIRE(result.value().transition_accounting.terminal_attempt.has_value());
    REQUIRE(result.value().transition_accounting.terminal_attempt->category ==
            execution::FunctionTransitionCategory::FunctionTransfer);
    REQUIRE(result.value().transition_accounting.terminal_attempt->resource_sequence == 6U);
    REQUIRE(result.value().maximum_call_depth == 0U);
    REQUIRE(result.value().final_cpu.x[30U] == result.value().synthetic_lr_sentinel);
}

TEST_CASE("M31 recursion records entries while call depth independently stops it")
{
    constexpr memory::GuestAddress base = 0xa000U;
    const auto code = words({movz(16U, static_cast<std::uint16_t>(base)), 0xd63f0200U,
                             0xd65f03c0U});
    auto fixture = make_fixture(base, code, {seed(base)});
    REQUIRE(fixture);
    execution::ExecutionSessionOptions options;
    options.budgets.max_call_depth = 2U;
    options.budgets.max_function_transitions = 10U;
    const auto result = run_fixture(fixture.value(), base, options);
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::CallDepthExceeded);
    REQUIRE(result.value().maximum_call_depth == 2U);
    REQUIRE(result.value().transition_accounting.total_charged == 0U);
    REQUIRE(result.value().transition_accounting.total_function_entries == 3U);
    REQUIRE(result.value().transition_accounting.indirect_call_entries == 2U);
    REQUIRE(result.value().indirect_calls == 3U);
}

TEST_CASE("M31 non-call transition limit is exact below, at, and beyond the boundary")
{
    constexpr memory::GuestAddress base = 0xb000U;
    const auto code = words({branch(base, base + 0x10U), 0xd503201fU,
                             0xd503201fU, 0xd503201fU,
                             branch(base + 0x10U, base + 0x20U), 0xd503201fU,
                             0xd503201fU, 0xd503201fU,
                             movz(0U, 1U), 0xd65f03c0U});

    auto below_fixture = make_fixture(base, code,
                                      {seed(base), seed(base + 0x10U), seed(base + 0x20U)});
    REQUIRE(below_fixture);
    execution::ExecutionSessionOptions below_options;
    below_options.budgets.max_function_transitions = 3U;
    const auto below = run_fixture(below_fixture.value(), base, below_options);
    REQUIRE(below);
    REQUIRE(below.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(below.value().transition_accounting.total_charged == 2U);

    auto exact_fixture = make_fixture(base, code,
                                      {seed(base), seed(base + 0x10U), seed(base + 0x20U)});
    REQUIRE(exact_fixture);
    execution::ExecutionSessionOptions exact_options;
    exact_options.budgets.max_function_transitions = 2U;
    const auto exact = run_fixture(exact_fixture.value(), base, exact_options);
    REQUIRE(exact);
    REQUIRE(exact.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(exact.value().transition_accounting.total_charged == 2U);

    auto beyond_fixture = make_fixture(base, code,
                                       {seed(base), seed(base + 0x10U), seed(base + 0x20U)});
    REQUIRE(beyond_fixture);
    execution::ExecutionSessionOptions beyond_options;
    beyond_options.budgets.max_function_transitions = 1U;
    const auto beyond = run_fixture(beyond_fixture.value(), base, beyond_options);
    REQUIRE(beyond);
    REQUIRE(beyond.value().stop_reason == execution::ExecutionStopReason::FunctionTransitionLimitExceeded);
    REQUIRE(beyond.value().transition_accounting.total_charged == 1U);
    REQUIRE(beyond.value().transition_accounting.terminal_attempt->resource_sequence == 2U);
}

TEST_CASE("M31 terminal witness identifies the attempted transfer without fake provenance")
{
    constexpr memory::GuestAddress base = 0xc000U;
    const auto code = words({branch(base, base + 0x10U), 0xd503201fU, 0xd503201fU,
                             0xd503201fU, branch(base + 0x10U, base)});
    auto fixture = make_fixture(base, code, {seed(base), seed(base + 0x10U)});
    REQUIRE(fixture);
    execution::ExecutionSessionOptions options;
    options.budgets.max_function_transitions = 1U;
    const auto result = run_fixture(fixture.value(), base, options);
    REQUIRE(result);
    const auto& attempt = result.value().transition_accounting.terminal_attempt;
    REQUIRE(attempt.has_value());
    REQUIRE(attempt->category == execution::FunctionTransitionCategory::FunctionTransfer);
    REQUIRE(attempt->source_guest_pc == base + 0x10U);
    REQUIRE(attempt->target_function == base);
    REQUIRE(attempt->target_previously_entered);
    REQUIRE(attempt->previous_source_target_edge_count == 0U);
    REQUIRE(attempt->target_ownership ==
            execution::FunctionTransitionTargetOwnership::ExactTrustedFunctionEntry);
    REQUIRE(attempt->source_opcode.has_value());
    REQUIRE(attempt->source_instruction_id == "b");
    REQUIRE(attempt->source_instruction == "b #0xc000");
    REQUIRE(attempt->link_register_action ==
            execution::FunctionTransitionLinkRegisterAction::Preserved);
    REQUIRE(result.value().source_pc.has_value());
    REQUIRE(result.value().target.has_value());
    REQUIRE(attempt->target_register.empty());
    REQUIRE(attempt->target_provenance.empty());
}

TEST_CASE("M31 aggregation and diagnostics are byte deterministic")
{
    auto first_fixture = repeated_call_fixture(8U);
    auto second_fixture = repeated_call_fixture(8U);
    execution::ExecutionSessionOptions options;
    options.budgets.max_function_transitions = 1U;
    const auto first = run_fixture(first_fixture, 0x10000U, options);
    const auto second = run_fixture(second_fixture, 0x10000U, options);
    REQUIRE(first);
    REQUIRE(second);
    const auto first_report = execution::render_execution_report_json(first.value());
    const auto second_report = execution::render_execution_report_json(second.value());
    REQUIRE(first_report == second_report);
    REQUIRE(first.value().transition_accounting.module_matrix.size() == 1U);
    REQUIRE(first.value().transition_accounting.depth_counts.size() == 2U);
    REQUIRE(first.value().transition_accounting.unique_call_sites == 8U);
    REQUIRE(first.value().transition_accounting.unique_function_targets == 9U);
    REQUIRE(first.value().transition_accounting.total_charged == 0U);
    REQUIRE(first.value().transition_accounting.total_function_entries == 9U);
}

TEST_CASE("M31 transition stop reclaims the generation-scoped synthetic stack")
{
    constexpr memory::GuestAddress base = 0xd000U;
    const auto code = words({branch(base, base + 0x10U), 0xd503201fU, 0xd503201fU,
                             0xd503201fU, branch(base + 0x10U, base)});
    auto fixture = make_fixture(base, code, {seed(base), seed(base + 0x10U)});
    REQUIRE(fixture);
    memory::GuestMemoryAccounting after_run;
    {
        execution::ExecutionSessionOptions options;
        options.budgets.max_function_transitions = 1U;
        execution::ExecutionSession session(fixture.value().memory, fixture.value().function_map, {}, options);
        const auto selected = execution::select_entry(
            fixture.value().function_map.identity(), execution::EntrySelectionKind::DynamicInit);
        REQUIRE(selected);
        auto selection = selected.value();
        selection.address = base;
        const auto result = session.run(selection);
        REQUIRE(result);
        REQUIRE(result.value().stop_reason ==
                execution::ExecutionStopReason::FunctionTransitionLimitExceeded);
        REQUIRE(result.value().guest_memory.live_owned_mappings == 1U);
    }
    after_run = fixture.value().memory.accounting();
    REQUIRE(after_run.live_owned_mappings == 0U);
    REQUIRE(after_run.owned_mappings_created == 1U);
    REQUIRE(after_run.owned_mappings_reclaimed == 1U);
}

TEST_CASE("M31 accounting reconciliation includes every charged category")
{
    auto fixture = repeated_call_fixture(3U);
    const auto result = run_fixture(fixture, 0x10000U);
    REQUIRE(result);
    const auto& accounting = result.value().transition_accounting;
    REQUIRE(accounting.total_charged == accounting.function_transfer_entries);
    REQUIRE(accounting.total_function_entries == result.value().executed_functions.size());
    REQUIRE(category_sum(accounting) == accounting.total_function_entries);
    REQUIRE(accounting.total_charged == 0U);
    REQUIRE(accounting.total_function_entries == 4U);
    REQUIRE(accounting.initial_entries == 1U);
    REQUIRE(accounting.direct_call_entries == 3U);
    REQUIRE(accounting.function_resume_entries == 0U);
    REQUIRE(accounting.other_entries == 0U);
    REQUIRE(accounting.module_matrix.front().source_module == "synthetic-m31");
    REQUIRE(accounting.module_matrix.front().target_module == "synthetic-m31");
}

TEST_CASE("M31 finite-model configuration rejects zero and derives an overflow-safe history bound")
{
    auto invalid_fixture = repeated_call_fixture(1U);
    execution::ExecutionSessionOptions invalid_options;
    invalid_options.budgets.max_function_transitions = 0U;
    const auto invalid = run_fixture(invalid_fixture, 0x10000U, invalid_options);
    REQUIRE_FALSE(invalid);
    REQUIRE(invalid.error().code == ErrorCode::InvalidArgument);

    auto maximum_fixture = repeated_call_fixture(1U);
    execution::ExecutionSessionOptions maximum_options;
    maximum_options.budgets.max_guest_blocks = std::numeric_limits<std::size_t>::max();
    const auto maximum = run_fixture(maximum_fixture, 0x10000U, maximum_options);
    REQUIRE(maximum);
    REQUIRE(maximum.value().transition_accounting.history_limit ==
            std::numeric_limits<std::size_t>::max());
    REQUIRE_FALSE(maximum.value().transition_accounting.history_truncated);
}
