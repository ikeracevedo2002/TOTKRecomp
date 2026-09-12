#include "switchrecomp/execution/session.hpp"
#include "switchrecomp/version.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
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

[[nodiscard]] Result<Fixture> make_fixture(
    memory::GuestAddress base, std::span<const std::byte> code,
    std::vector<analysis::FunctionSeed> seeds,
    std::optional<std::pair<memory::GuestAddress, memory::GuestSize>> data = std::nullopt)
{
    Fixture fixture;
    const auto mapped = fixture.memory.map(
        base, code, memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Execute,
        "synthetic.m11.text", memory::GuestRegionKind::Text);
    if (!mapped) return Result<Fixture>::failure(mapped.error());
    if (data)
    {
        const auto data_mapped = fixture.memory.map(
            data->first, data->second, memory::GuestMemoryPermissions::Read |
                                            memory::GuestMemoryPermissions::Write,
            "synthetic.m11.data", memory::GuestRegionKind::Data);
        if (!data_mapped) return Result<Fixture>::failure(data_mapped.error());
    }
    analysis::ModuleIdentity identity;
    identity.module = "synthetic-m11";
    identity.build_id = "synthetic-build";
    identity.input_sha256 = "synthetic-sha256";
    identity.guest_base = base;
    identity.translator_version = version;
    identity.executable_ranges.push_back(
        analysis::GuestAddressRange{base, static_cast<memory::GuestSize>(code.size())});
    identity.entry_points.push_back(analysis::EntryPointEvidence{
        base, analysis::EntryPointKind::DynamicInit, "synthetic DT_INIT", analysis::FunctionConfidence::High,
        false, "synthetic controlled entry"});
    const auto map = analysis::FunctionMapBuilder::build(
        analysis::ModuleAnalysisInput{std::move(identity), &fixture.memory, std::move(seeds)});
    if (!map) return Result<Fixture>::failure(map.error());
    fixture.function_map = std::move(map).value();
    return Result<Fixture>::success(std::move(fixture));
}

[[nodiscard]] Result<execution::ExecutionSessionResult> run_fixture(
    Fixture& fixture, memory::GuestAddress entry,
    execution::ExecutionSessionOptions options = {},
    std::vector<loader::UnresolvedRelocation> imports = {})
{
    execution::ExecutionSession session(fixture.memory, fixture.function_map, imports,
                                         std::move(options));
    const auto selected = execution::select_entry(
        fixture.function_map.identity(), execution::EntrySelectionKind::DynamicInit);
    if (!selected) return Result<execution::ExecutionSessionResult>::failure(selected.error());
    auto selection = selected.value();
    selection.address = entry;
    return session.run(selection);
}

[[nodiscard]] analysis::FunctionSeed seed(memory::GuestAddress address)
{
    return analysis::FunctionSeed{address, analysis::FunctionDiscoverySource::AnalystSeed,
                                  analysis::FunctionConfidence::Manual, std::nullopt, std::nullopt,
                                  "synthetic M11 function"};
}

} // namespace

TEST_CASE("M11 simple controlled entry returns with deterministic state")
{
    const auto code = words({movz(0U, 7U), 0xd65f03c0U});
    auto fixture = make_fixture(0x1000U, code, {seed(0x1000U)});
    REQUIRE(fixture);
    const auto result = run_fixture(fixture.value(), 0x1000U);
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(result.value().final_cpu.x[0] == 7U);
    REQUIRE(result.value().final_cpu.x[30U] == result.value().synthetic_lr_sentinel);
    REQUIRE(result.value().maximum_call_depth == 0U);
    REQUIRE(result.value().events.front().kind == execution::ExecutionEventKind::SessionStart);
    // The execution loop revisits the immutable entry between boundaries; the
    // second lookup must use the per-session lift cache.
    REQUIRE(result.value().performance.lift_cache_misses == 1U);
    REQUIRE(result.value().performance.functions_lifted == 1U);
    REQUIRE(result.value().performance.lift_cache_hits >= 1U);
}

TEST_CASE("M11 direct guest call resumes caller and preserves BL link semantics")
{
    constexpr memory::GuestAddress base = 0x2000U;
    const auto code = words({movz(0U, 1U), bl(base + 4U, base + 0x10U),
                             0x91000400U, 0xd65f03c0U,
                             0x91000800U, 0xd65f03c0U});
    auto fixture = make_fixture(base, code, {seed(base), seed(base + 0x10U)});
    REQUIRE(fixture);
    const auto result = run_fixture(fixture.value(), base);
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(result.value().final_cpu.x[0] == 4U);
    REQUIRE(result.value().direct_calls == 1U);
    REQUIRE(result.value().returns == 2U);
    REQUIRE(result.value().maximum_call_depth == 1U);
    REQUIRE(result.value().executed_functions == std::vector<memory::GuestAddress>{base, base + 0x10U});
    REQUIRE(result.value().events[4U].kind == execution::ExecutionEventKind::DirectCall);
    REQUIRE(result.value().events[7U].kind == execution::ExecutionEventKind::FunctionResume);
}

TEST_CASE("M11 nested calls retain continuations in guest frames")
{
    constexpr memory::GuestAddress base = 0x3000U;
    const auto code = words({bl(base, base + 0x10U), 0x91000400U, 0xd65f03c0U,
                             0xd503201fU,
                             0xaa1e03f3U, bl(base + 0x14U, base + 0x24U),
                             0x91000800U, 0xaa1303feU, 0xd65f03c0U,
                             0xd503201fU,
                             0x91001000U, 0xd65f03c0U});
    auto fixture = make_fixture(base, code, {seed(base), seed(base + 0x10U), seed(base + 0x24U)});
    REQUIRE(fixture);
    const auto result = run_fixture(fixture.value(), base);
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(result.value().final_cpu.x[0] == 7U);
    REQUIRE(result.value().maximum_call_depth == 2U);
    REQUIRE(result.value().returns == 3U);
}

TEST_CASE("M11 tail transfer preserves the inherited caller return contract")
{
    constexpr memory::GuestAddress base = 0x11000U;
    const auto code = words({0xaa1e03f3U, bl(base + 4U, base + 0x14U), 0xaa1303feU,
                             0x91000400U, 0xd65f03c0U,
                             branch(base + 0x14U, base + 0x24U), 0xd503201fU,
                             0xd503201fU, 0xd503201fU,
                             movz(0U, 5U), 0xd65f03c0U});
    auto fixture = make_fixture(base, code,
                                {seed(base), seed(base + 0x14U), seed(base + 0x24U)});
    REQUIRE(fixture);
    const auto result = run_fixture(fixture.value(), base);
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(result.value().final_cpu.x[0] == 6U);
    REQUIRE(result.value().function_transfers == 1U);
    REQUIRE(result.value().maximum_call_depth == 1U);
    REQUIRE(result.value().returns == 2U);
    REQUIRE(result.value().final_cpu.x[30U] == result.value().synthetic_lr_sentinel);
}

TEST_CASE("M11 recursion is bounded by guest call depth, not host recursion")
{
    constexpr memory::GuestAddress base = 0x4000U;
    const auto code = words({movz(16U, static_cast<std::uint16_t>(base)),
                             0xd63f0200U, 0xd65f03c0U});
    auto fixture = make_fixture(base, code, {seed(base)});
    REQUIRE(fixture);
    execution::ExecutionSessionOptions options;
    options.budgets.max_call_depth = 3U;
    const auto result = run_fixture(fixture.value(), base, options);
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::CallDepthExceeded);
    REQUIRE(result.value().maximum_call_depth == 3U);
}

TEST_CASE("M11 distinguishes B function transfer from BL")
{
    constexpr memory::GuestAddress base = 0x5000U;
    const auto code = words({branch(base, base + 0x10U), 0xd503201fU, 0xd503201fU,
                             0xd503201fU, movz(0U, 9U), 0xd65f03c0U});
    auto fixture = make_fixture(base, code, {seed(base), seed(base + 0x10U)});
    REQUIRE(fixture);
    const auto result = run_fixture(fixture.value(), base);
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(result.value().function_transfers == 1U);
    REQUIRE(result.value().maximum_call_depth == 0U);
    REQUIRE(result.value().final_cpu.x[30U] == result.value().synthetic_lr_sentinel);
}

TEST_CASE("M11 indirect call and indirect branch dispatch exact known entries")
{
    constexpr memory::GuestAddress call_base = 0x6000U;
    const auto call_code = words({movz(16U, static_cast<std::uint16_t>(call_base + 0x10U)),
                                  0xd63f0200U, 0x91000400U, 0xd65f03c0U,
                                  0x91000800U, 0xd65f03c0U});
    auto call_fixture = make_fixture(call_base, call_code,
                                      {seed(call_base), seed(call_base + 0x10U)});
    REQUIRE(call_fixture);
    const auto call_result = run_fixture(call_fixture.value(), call_base);
    REQUIRE(call_result);
    REQUIRE(call_result.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(call_result.value().indirect_calls == 1U);
    REQUIRE(call_result.value().final_cpu.x[0] == 3U);

    constexpr memory::GuestAddress branch_base = 0x7000U;
    const auto branch_code = words({movz(16U, static_cast<std::uint16_t>(branch_base + 0x10U)),
                                    0xd61f0200U, 0xd503201fU, 0xd503201fU,
                                    movz(0U, 11U), 0xd65f03c0U});
    auto branch_fixture = make_fixture(branch_base, branch_code,
                                        {seed(branch_base), seed(branch_base + 0x10U)});
    REQUIRE(branch_fixture);
    const auto branch_result = run_fixture(branch_fixture.value(), branch_base);
    REQUIRE(branch_result);
    REQUIRE(branch_result.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(branch_result.value().function_transfers == 1U);
    REQUIRE(branch_result.value().maximum_call_depth == 0U);
}

TEST_CASE("M11 unknown indirect targets stop without executing arbitrary bytes")
{
    constexpr memory::GuestAddress base = 0x8000U;
    const auto code = words({movz(16U, 0x9000U), 0xd61f0200U, 0xd65f03c0U});
    auto fixture = make_fixture(base, code, {seed(base)});
    REQUIRE(fixture);
    const auto result = run_fixture(fixture.value(), base);
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::InvalidIndirectTarget);
    REQUIRE(result.value().target == 0x9000U);

    auto mapped_unknown = make_fixture(base + 0x2000U, code, {seed(base + 0x2000U)});
    REQUIRE(mapped_unknown);
    const auto unknown_code = words({0xd503201fU, 0xd65f03c0U});
    const auto mapped = mapped_unknown.value().memory.map(
        0x9000U, unknown_code, memory::GuestMemoryPermissions::Read |
                                   memory::GuestMemoryPermissions::Execute,
        "synthetic.m11.unknown", memory::GuestRegionKind::Text);
    REQUIRE(mapped);
    const auto unknown = run_fixture(mapped_unknown.value(), base + 0x2000U);
    REQUIRE(unknown);
    REQUIRE(unknown.value().stop_reason == execution::ExecutionStopReason::UnknownGuestFunction);
}

TEST_CASE("M11 return target mismatch is explicit")
{
    constexpr memory::GuestAddress base = 0x9000U;
    const auto code = words({bl(base, base + 0x10U), 0xd65f03c0U, 0xd503201fU, 0xd503201fU,
                             movz(30U, 0x999U), 0xd65f03c0U});
    auto fixture = make_fixture(base, code, {seed(base), seed(base + 0x10U)});
    REQUIRE(fixture);
    const auto result = run_fixture(fixture.value(), base);
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::ReturnTargetMismatch);
}

TEST_CASE("M11 unresolved imports use relocation evidence while zero does not")
{
    constexpr memory::GuestAddress base = 0xa000U;
    constexpr memory::GuestAddress slot = 0xb000U;
    const auto code = words({movz(1U, static_cast<std::uint16_t>(slot)),
                             0xf9400030U, 0xd63f0200U, 0xd65f03c0U});
    auto fixture = make_fixture(base, code, {seed(base)}, std::make_pair(slot, 16U));
    REQUIRE(fixture);
    format::Relocation relocation{0U, slot, 0U, format::AArch64RelocationType::JumpSlot, 3U, 0,
                                  format::RelocationSource::JmpRel};
    format::ImportSymbol symbol{3U, "synthetic_import", format::SymbolBinding::Global,
                                format::SymbolType::Function, format::SymbolVisibility::Default};
    loader::UnresolvedRelocation unresolved{4U, relocation, symbol};
    const auto imported = run_fixture(fixture.value(), base, {}, {unresolved});
    REQUIRE(imported);
    REQUIRE(imported.value().stop_reason == execution::ExecutionStopReason::UnresolvedImport);
    REQUIRE(imported.value().import_boundary);
    REQUIRE(imported.value().import_boundary->symbol.name == "synthetic_import");
    REQUIRE(imported.value().target_register == "x16");
    REQUIRE(imported.value().target_provenance.find("0x000000000000b000") != std::string::npos);

    auto zero_fixture = make_fixture(base + 0x2000U, code, {seed(base + 0x2000U)},
                                     std::make_pair(slot, 16U));
    REQUIRE(zero_fixture);
    const auto zero = run_fixture(zero_fixture.value(), base + 0x2000U);
    REQUIRE(zero);
    REQUIRE(zero.value().stop_reason == execution::ExecutionStopReason::InvalidIndirectTarget);
    REQUIRE_FALSE(zero.value().import_boundary);
}

TEST_CASE("M11 synthetic stack is writable, aligned, and bounded")
{
    constexpr memory::GuestAddress base = 0xd000U;
    const auto stack_code = words({0xd10043ffU, 0xf90003e0U, 0xf94003e1U,
                                   0x910043ffU, 0xd65f03c0U});
    auto fixture = make_fixture(base, stack_code, {seed(base)});
    REQUIRE(fixture);
    execution::ExecutionSessionOptions options;
    options.synthetic_stack_size = 0x2000U;
    const auto result = run_fixture(fixture.value(), base, options);
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(result.value().final_cpu.x[0] == 0U);
    REQUIRE(result.value().final_cpu.x[1] == 0U);
    REQUIRE((result.value().initial_sp & 0xfU) == 0U);

    constexpr memory::GuestAddress guard_base = 0xe000U;
    const auto guard_code = words({0xf94003e0U, 0xd65f03c0U});
    auto guard_fixture = make_fixture(guard_base, guard_code, {seed(guard_base)});
    REQUIRE(guard_fixture);
    const auto guard = run_fixture(guard_fixture.value(), guard_base, options);
    REQUIRE(guard);
    REQUIRE(guard.value().stop_reason == execution::ExecutionStopReason::MemoryFault);
}

TEST_CASE("M11 session IR and event budgets are global and deterministic")
{
    constexpr memory::GuestAddress base = 0xc000U;
    const auto code = words({bl(base, base + 0x10U), 0xd65f03c0U, 0xd503201fU, 0xd503201fU,
                             movz(0U, 1U), 0xd65f03c0U});
    auto first = make_fixture(base, code, {seed(base), seed(base + 0x10U)});
    auto second = make_fixture(base, code, {seed(base), seed(base + 0x10U)});
    REQUIRE(first);
    REQUIRE(second);
    execution::ExecutionSessionOptions limited;
    limited.budgets.max_ir_operations = 5U;
    const auto limited_result = run_fixture(first.value(), base, limited);
    REQUIRE(limited_result);
    REQUIRE(limited_result.value().stop_reason == execution::ExecutionStopReason::IrOperationLimitExceeded);

    auto third = make_fixture(base, code, {seed(base), seed(base + 0x10U)});
    REQUIRE(third);
    const auto first_result = run_fixture(third.value(), base);
    const auto second_result = run_fixture(second.value(), base);
    REQUIRE(first_result);
    REQUIRE(second_result);
    REQUIRE(execution::render_execution_report_json(first_result.value()) ==
            execution::render_execution_report_json(second_result.value()));

    execution::ExecutionSessionOptions event_limited;
    event_limited.budgets.max_events = 4U;
    auto event_fixture = make_fixture(base, code, {seed(base), seed(base + 0x10U)});
    REQUIRE(event_fixture);
    const auto event_result = run_fixture(event_fixture.value(), base, event_limited);
    REQUIRE(event_result);
    REQUIRE(event_result.value().stop_reason == execution::ExecutionStopReason::EventLimitExceeded);
}

TEST_CASE("M11 entry selection preserves dynamic and unverified provenance")
{
    analysis::ModuleIdentity identity;
    identity.module = "synthetic-entry";
    identity.entry_points.push_back(analysis::EntryPointEvidence{
        0x1234U, analysis::EntryPointKind::DynamicInit, "metadata DT_INIT",
        analysis::FunctionConfidence::High, false, "candidate"});
    const auto init = execution::select_entry(identity, execution::EntrySelectionKind::DynamicInit);
    REQUIRE(init);
    REQUIRE(init.value().address == 0x1234U);
    REQUIRE_FALSE(init.value().verified_process_entry);
    const auto process = execution::select_entry(identity, execution::EntrySelectionKind::VerifiedProcessEntry);
    REQUIRE_FALSE(process);
    REQUIRE(process.error().message.find("no verified process entry") != std::string::npos);
    const auto text = execution::select_entry(identity, execution::EntrySelectionKind::TextStartCandidate);
    REQUIRE_FALSE(text);
}

TEST_CASE("M11 refuses to dispatch a precise ownership conflict")
{
    constexpr memory::GuestAddress base = 0xf000U;
    const auto code = words({0xd503201fU, 0xd503201fU, 0xd65f03c0U, 0xd503201fU});
    auto fixture = make_fixture(base, code, {seed(base), seed(base + 4U)});
    REQUIRE(fixture);
    const auto result = run_fixture(fixture.value(), base + 4U);
    REQUIRE(result);
    REQUIRE(result.value().precise_conflicts > 0U);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::FunctionOwnershipConflict);
}
