#include "switchrecomp/execution/session.hpp"
#include "switchrecomp/version.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

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

[[nodiscard]] std::vector<std::byte> words(const std::vector<std::uint32_t>& values)
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

[[nodiscard]] std::uint32_t bl(memory::GuestAddress pc, memory::GuestAddress target)
{
    const auto displacement = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pc);
    return 0x94000000U | (static_cast<std::uint32_t>(displacement / 4) & 0x03ffffffU);
}

[[nodiscard]] std::uint32_t movz(std::uint8_t reg, std::uint16_t value)
{
    return 0xd2800000U | (static_cast<std::uint32_t>(value) << 5U) | reg;
}

[[nodiscard]] std::uint32_t branch(memory::GuestAddress pc, memory::GuestAddress target)
{
    const auto displacement = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pc);
    return 0x14000000U | (static_cast<std::uint32_t>(displacement / 4) & 0x03ffffffU);
}

[[nodiscard]] analysis::FunctionSeed seed(memory::GuestAddress address)
{
    return analysis::FunctionSeed{address, analysis::FunctionDiscoverySource::AnalystSeed,
                                  analysis::FunctionConfidence::Manual, std::nullopt, std::nullopt,
                                  "synthetic M34 function"};
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
        "synthetic.m34.text", memory::GuestRegionKind::Text);
    if (!mapped) return Result<Fixture>::failure(mapped.error());
    if (data)
    {
        const auto data_mapped = fixture.memory.map(
            data->first, data->second,
            memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Write,
            "synthetic.m34.data", memory::GuestRegionKind::Data);
        if (!data_mapped) return Result<Fixture>::failure(data_mapped.error());
    }
    analysis::ModuleIdentity identity;
    identity.module = "synthetic-m34";
    identity.build_id = "synthetic-m34-build";
    identity.input_sha256 = "synthetic-m34-sha";
    identity.guest_base = base;
    identity.translator_version = version;
    identity.executable_ranges.push_back(
        analysis::GuestAddressRange{base, static_cast<memory::GuestSize>(code.size())});
    identity.entry_points.push_back({base, analysis::EntryPointKind::DynamicInit,
                                     "synthetic M34 entry", analysis::FunctionConfidence::High,
                                     false, "bounded event observability"});
    const auto map = analysis::FunctionMapBuilder::build(
        analysis::ModuleAnalysisInput{std::move(identity), &fixture.memory, std::move(seeds)});
    if (!map) return Result<Fixture>::failure(map.error());
    fixture.function_map = std::move(map).value();
    return Result<Fixture>::success(std::move(fixture));
}

[[nodiscard]] Result<execution::ExecutionSessionResult> run_fixture(
    Fixture& fixture, memory::GuestAddress entry,
    execution::ExecutionSessionOptions options = {},
    runtime::RuntimeImportRegistry* runtime_imports = nullptr,
    std::vector<loader::UnresolvedRelocation> imports = {})
{
    execution::ExecutionSession session(fixture.memory, fixture.function_map, imports,
                                         std::move(options), {}, runtime_imports);
    const auto selected = execution::select_entry(
        fixture.function_map.identity(), execution::EntrySelectionKind::DynamicInit);
    if (!selected) return Result<execution::ExecutionSessionResult>::failure(selected.error());
    auto selection = selected.value();
    selection.address = entry;
    return session.run(selection);
}

[[nodiscard]] Result<Fixture> make_loop_fixture(memory::GuestAddress base)
{
    const auto code = words(std::vector<std::uint32_t>{
        bl(base, base + 0x0cU),       // call the helper
        0x91000400U,                  // add x0, x0, #1 after the return
        branch(base + 8U, base),      // repeat the caller body
        0xd65f03c0U,                  // helper: ret
    });
    return make_fixture(base, code, {seed(base), seed(base + 0x0cU)});
}

[[nodiscard]] format::ImportSymbol import_symbol(std::uint32_t index, std::string name)
{
    return format::ImportSymbol{index, std::move(name), format::SymbolBinding::Global,
                                format::SymbolType::Function, format::SymbolVisibility::Default};
}

[[nodiscard]] loader::UnresolvedRelocation unresolved_import(
    memory::GuestAddress slot, std::uint32_t symbol_index, std::string name)
{
    const format::Relocation relocation{slot, slot, 1026U,
                                        format::AArch64RelocationType::JumpSlot,
                                        symbol_index, 0, format::RelocationSource::JmpRel};
    return loader::UnresolvedRelocation{0U, relocation,
                                        import_symbol(symbol_index, std::move(name))};
}

[[nodiscard]] runtime::RuntimeImportDescriptor runtime_descriptor(std::string name)
{
    runtime::RuntimeImportDescriptor result;
    result.symbol_name = std::move(name);
    result.subsystem = runtime::RuntimeSubsystem::C;
    result.support = runtime::RuntimeSupportStatus::Implemented;
    result.signature.return_kind = runtime::AbiReturnKind::Void;
    result.evidence.confidence = "synthetic";
    result.evidence.rationale = "synthetic M34 multi-event boundary";
    return result;
}

} // namespace

TEST_CASE("M34 ordinary execution crosses 4096 logical events and retains bounded evidence")
{
    constexpr memory::GuestAddress base = 0x10000U;
    auto fixture = make_loop_fixture(base);
    REQUIRE(fixture);
    execution::ExecutionSessionOptions options;
    options.budgets.max_guest_blocks = 6'000U;
    const auto result = run_fixture(fixture.value(), base, options);
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::GuestBlockLimitExceeded);
    REQUIRE(result.value().event_resource.total_generated > 4'096U);
    REQUIRE(result.value().final_cpu.x[0] > 1'000U);
    REQUIRE(result.value().event_resource.retained == 4'096U);
    REQUIRE(result.value().event_resource.omitted > 0U);
    REQUIRE(result.value().event_resource.history_truncated);
    REQUIRE(result.value().event_resource.reconciles);
    REQUIRE(result.value().events.front().sequence == 0U);
    REQUIRE(result.value().events.back().sequence ==
            result.value().event_resource.total_generated - 1U);
}

TEST_CASE("M34 history capacity does not affect guest architectural execution")
{
    constexpr memory::GuestAddress base = 0x20000U;
    auto narrow_fixture = make_loop_fixture(base);
    auto wide_fixture = make_loop_fixture(base);
    REQUIRE(narrow_fixture);
    REQUIRE(wide_fixture);
    execution::ExecutionSessionOptions narrow_options;
    narrow_options.budgets.max_guest_blocks = 1'200U;
    narrow_options.budgets.event_history_limit = 8U;
    execution::ExecutionSessionOptions wide_options = narrow_options;
    wide_options.budgets.event_history_limit = 128U;
    const auto narrow = run_fixture(narrow_fixture.value(), base, narrow_options);
    const auto wide = run_fixture(wide_fixture.value(), base, wide_options);
    REQUIRE(narrow);
    REQUIRE(wide);
    REQUIRE(narrow.value().stop_reason == wide.value().stop_reason);
    REQUIRE(narrow.value().final_cpu.x == wide.value().final_cpu.x);
    REQUIRE(narrow.value().final_cpu.sp == wide.value().final_cpu.sp);
    REQUIRE(narrow.value().final_cpu.pc == wide.value().final_cpu.pc);
    REQUIRE(narrow.value().final_cpu.n == wide.value().final_cpu.n);
    REQUIRE(narrow.value().final_cpu.z == wide.value().final_cpu.z);
    REQUIRE(narrow.value().final_cpu.c == wide.value().final_cpu.c);
    REQUIRE(narrow.value().final_cpu.v == wide.value().final_cpu.v);
    REQUIRE(narrow.value().final_cpu.fpcr == wide.value().final_cpu.fpcr);
    REQUIRE(narrow.value().final_cpu.fpsr == wide.value().final_cpu.fpsr);
    REQUIRE(narrow.value().final_cpu.vreg == wide.value().final_cpu.vreg);
    REQUIRE(narrow.value().final_cpu.tpidr_el0 == wide.value().final_cpu.tpidr_el0);
    REQUIRE(narrow.value().final_cpu.tpidrro_el0 == wide.value().final_cpu.tpidrro_el0);
    REQUIRE(narrow.value().guest_blocks == wide.value().guest_blocks);
    REQUIRE(narrow.value().guest_instruction_count == wide.value().guest_instruction_count);
    REQUIRE(narrow.value().ir_operations == wide.value().ir_operations);
    REQUIRE(narrow.value().direct_calls == wide.value().direct_calls);
    REQUIRE(narrow.value().returns == wide.value().returns);
    REQUIRE(narrow.value().event_resource.total_generated ==
            wide.value().event_resource.total_generated);
    REQUIRE(narrow.value().event_resource.retained == 8U);
    REQUIRE(wide.value().event_resource.retained == 128U);
}

TEST_CASE("M34 first-prefix and recent-window history has deterministic logical ordering")
{
    constexpr memory::GuestAddress base = 0x30000U;
    auto fixture = make_loop_fixture(base);
    REQUIRE(fixture);
    execution::ExecutionSessionOptions options;
    options.budgets.max_guest_blocks = 1'200U;
    options.budgets.event_history_limit = 8U;
    const auto result = run_fixture(fixture.value(), base, options);
    REQUIRE(result);
    const auto& value = result.value();
    REQUIRE(value.event_resource.history_truncated);
    REQUIRE(value.events.size() == 8U);
    REQUIRE(value.events[0].sequence == 0U);
    REQUIRE(value.events[1].sequence == 1U);
    REQUIRE(value.events[2].sequence == 2U);
    REQUIRE(value.events[3].sequence == 3U);
    REQUIRE(value.events[4].sequence == value.event_resource.total_generated - 4U);
    REQUIRE(value.events[7].sequence == value.event_resource.total_generated - 1U);
    REQUIRE(value.event_resource.first_sequence_retained == 0U);
    REQUIRE(value.event_resource.last_sequence_retained ==
            value.event_resource.total_generated - 1U);
    REQUIRE(value.event_resource.omitted == value.event_resource.total_generated - 8U);
}

TEST_CASE("M34 explicit max-events remains an exact compatibility execution guard")
{
    constexpr memory::GuestAddress base = 0x40000U;
    auto fixture = make_loop_fixture(base);
    REQUIRE(fixture);
    execution::ExecutionSessionOptions options;
    options.budgets.max_events = 3U;
    options.budgets.event_history_limit = 8U;
    const auto result = run_fixture(fixture.value(), base, options);
    REQUIRE(result);
    const auto& value = result.value();
    REQUIRE(value.stop_reason == execution::ExecutionStopReason::EventLimitExceeded);
    REQUIRE(value.event_resource.execution_limit == 3U);
    REQUIRE(value.event_resource.execution_limit_provenance ==
            execution::EventExecutionLimitProvenance::ExplicitLibraryApi);
    REQUIRE(value.event_resource.total_generated == 3U);
    REQUIRE(value.events.size() == 3U);
    REQUIRE(value.event_resource.terminal_attempt_sequence == 3U);
    REQUIRE(value.event_resource.terminal_attempt_kind ==
            execution::ExecutionEventKind::FunctionEnter);
    REQUIRE(value.event_resource.terminal_attempt);
    REQUIRE(value.event_resource.terminal_attempt->sequence == 3U);
    REQUIRE(value.diagnostic.find("consumed=3 limit=3 attempted_sequence=3") != std::string::npos);
    REQUIRE(value.final_cpu.x[0] == 0U);
}

TEST_CASE("M34 runtime boundary accounts for every intentionally emitted diagnostic event")
{
    constexpr memory::GuestAddress base = 0x50000U;
    constexpr memory::GuestAddress slot = 0x6000U;
    const auto code = words({movz(1U, static_cast<std::uint16_t>(slot)), 0xf9400030U,
                              0xd63f0200U, 0xd65f03c0U});
    auto fixture = make_fixture(base, code, {seed(base)}, std::make_pair(slot, 16U));
    REQUIRE(fixture);
    runtime::RuntimeImportRegistry registry;
    REQUIRE(registry.register_import(runtime_descriptor("synthetic_m34_runtime"),
                                      [](runtime::RuntimeImportContext&) {
                                          return Result<runtime::RuntimeImportOutcome>::success(
                                              runtime::RuntimeImportOutcome::handled());
                                      }));
    execution::ExecutionSessionOptions options;
    options.budgets.max_guest_blocks = 100U;
    const auto result = run_fixture(
        fixture.value(), base, options, &registry,
        {unresolved_import(slot, 7U, "synthetic_m34_runtime")});
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    const auto& counts = result.value().event_resource.by_kind;
    REQUIRE(counts[static_cast<std::size_t>(execution::ExecutionEventKind::RuntimeImportResolved)] == 1U);
    REQUIRE(counts[static_cast<std::size_t>(execution::ExecutionEventKind::RuntimeImportEnter)] == 1U);
    REQUIRE(counts[static_cast<std::size_t>(execution::ExecutionEventKind::RuntimeImportArgumentSummary)] == 1U);
    REQUIRE(counts[static_cast<std::size_t>(execution::ExecutionEventKind::RuntimeImportReturn)] == 1U);
    REQUIRE(counts[static_cast<std::size_t>(execution::ExecutionEventKind::RuntimeStateRegistration)] == 0U);
    REQUIRE(result.value().event_resource.total_generated == 11U);
    REQUIRE(result.value().event_resource.reconciles);
}

TEST_CASE("M34 function call and return events reconcile exactly")
{
    constexpr memory::GuestAddress base = 0x70000U;
    const auto code = words({bl(base, base + 0x0cU), 0x91000400U, 0xd65f03c0U,
                             0xd65f03c0U});
    auto fixture = make_fixture(base, code, {seed(base), seed(base + 0x0cU)});
    REQUIRE(fixture);
    const auto result = run_fixture(fixture.value(), base);
    REQUIRE(result);
    const auto& value = result.value();
    REQUIRE(value.stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(value.direct_calls == 1U);
    REQUIRE(value.returns == 2U);
    REQUIRE(value.event_resource.by_kind[static_cast<std::size_t>(execution::ExecutionEventKind::FunctionEnter)] == 2U);
    REQUIRE(value.event_resource.by_kind[static_cast<std::size_t>(execution::ExecutionEventKind::DirectCall)] == 1U);
    REQUIRE(value.event_resource.by_kind[static_cast<std::size_t>(execution::ExecutionEventKind::Return)] == 2U);
    REQUIRE(value.event_resource.by_kind[static_cast<std::size_t>(execution::ExecutionEventKind::FunctionResume)] == 1U);
    std::size_t counted = 0U;
    for (const auto count : value.event_resource.by_kind) counted += count;
    REQUIRE(counted == value.event_resource.total_generated);
}

TEST_CASE("M34 logical event sequencing fails closed at size_t overflow")
{
    const auto max = execution::checked_next_execution_event_sequence(
        std::numeric_limits<std::size_t>::max());
    REQUIRE_FALSE(max);
    REQUIRE(max.error().code == ErrorCode::ArithmeticOverflow);
    const auto last = execution::checked_next_execution_event_sequence(
        std::numeric_limits<std::size_t>::max() - 1U);
    REQUIRE(last);
    REQUIRE(last.value() == std::numeric_limits<std::size_t>::max());
}

TEST_CASE("M34 identical synthetic sessions serialize event diagnostics byte-identically")
{
    constexpr memory::GuestAddress base = 0x80000U;
    auto first_fixture = make_loop_fixture(base);
    auto second_fixture = make_loop_fixture(base);
    REQUIRE(first_fixture);
    REQUIRE(second_fixture);
    execution::ExecutionSessionOptions options;
    options.budgets.max_guest_blocks = 1'200U;
    options.budgets.event_history_limit = 16U;
    const auto first = run_fixture(first_fixture.value(), base, options);
    const auto second = run_fixture(second_fixture.value(), base, options);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(execution::render_execution_report_json(first.value()) ==
            execution::render_execution_report_json(second.value()));
    const auto report = nlohmann::json::parse(
        execution::render_execution_report_json(first.value()));
    REQUIRE(report.at("schema_version").get<std::uint32_t>() == 20U);
    REQUIRE(report.at("event_resource").at("history_policy") ==
            "first_prefix_plus_recent_window");
}
