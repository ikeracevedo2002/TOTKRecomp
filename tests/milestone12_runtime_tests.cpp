#include "switchrecomp/execution/session.hpp"
#include "switchrecomp/version.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
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

void write_u64_le(memory::GuestMemory& memory, memory::GuestAddress address,
                  std::uint64_t value)
{
    std::array<std::byte, sizeof(value)> bytes{};
    for (std::size_t index = 0U; index < bytes.size(); ++index)
    {
        bytes[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
    REQUIRE(memory.write(address, bytes));
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

struct Fixture
{
    memory::GuestMemory memory;
    analysis::FinalizedFunctionMap function_map;
};

[[nodiscard]] analysis::FunctionSeed seed(memory::GuestAddress address)
{
    return analysis::FunctionSeed{address, analysis::FunctionDiscoverySource::AnalystSeed,
                                  analysis::FunctionConfidence::Manual, std::nullopt, std::nullopt,
                                  "synthetic M12 function"};
}

[[nodiscard]] Result<Fixture> make_fixture(
    memory::GuestAddress base, std::span<const std::byte> code,
    std::vector<analysis::FunctionSeed> seeds,
    std::optional<std::pair<memory::GuestAddress, memory::GuestSize>> data = std::nullopt)
{
    Fixture fixture;
    const auto mapped = fixture.memory.map(
        base, code, memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Execute,
        "synthetic.m12.text", memory::GuestRegionKind::Text);
    if (!mapped) return Result<Fixture>::failure(mapped.error());
    if (data)
    {
        const auto data_mapped = fixture.memory.map(
            data->first, data->second, memory::GuestMemoryPermissions::Read |
                                            memory::GuestMemoryPermissions::Write,
            "synthetic.m12.data", memory::GuestRegionKind::Data);
        if (!data_mapped) return Result<Fixture>::failure(data_mapped.error());
    }
    analysis::ModuleIdentity identity;
    identity.module = "synthetic-m12";
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

[[nodiscard]] format::ImportSymbol import_symbol(std::uint32_t index, std::string name)
{
    return format::ImportSymbol{index, std::move(name), format::SymbolBinding::Global,
                                format::SymbolType::Function, format::SymbolVisibility::Default};
}

[[nodiscard]] loader::UnresolvedRelocation unresolved_import(
    memory::GuestAddress slot, std::uint32_t symbol_index, std::string name,
    std::size_t relocation_index = 0U)
{
    format::Relocation relocation{slot, slot, 1026U, format::AArch64RelocationType::JumpSlot,
                                  symbol_index, 0, format::RelocationSource::JmpRel};
    return loader::UnresolvedRelocation{relocation_index, relocation,
                                        import_symbol(symbol_index, std::move(name))};
}

[[nodiscard]] runtime::RuntimeImportDescriptor descriptor(
    std::string name, std::size_t argument_count, runtime::AbiReturnKind return_kind)
{
    runtime::RuntimeImportDescriptor result;
    result.symbol_name = std::move(name);
    result.subsystem = runtime::RuntimeSubsystem::C;
    result.support = runtime::RuntimeSupportStatus::Implemented;
    result.signature.argument_count = argument_count;
    result.signature.arguments.assign(argument_count, runtime::AbiValueKind::Integer64);
    result.signature.observed_argument_count = argument_count;
    result.signature.return_kind = return_kind;
    result.evidence.confidence = "synthetic";
    result.evidence.rationale = "synthetic handler contract";
    return result;
}

[[nodiscard]] runtime::ImportProvenance provenance_for(const format::ImportSymbol& symbol)
{
    runtime::ImportProvenance result;
    result.relocation_target = 0x2000U;
    result.relocation_index = 1U;
    result.relocation = format::Relocation{0x2000U, 0x2000U, 1026U,
                                           format::AArch64RelocationType::JumpSlot,
                                           symbol.symbol_index, 0,
                                           format::RelocationSource::JmpRel};
    result.symbol = symbol;
    return result;
}

[[nodiscard]] Result<execution::ExecutionSessionResult> run_fixture(
    Fixture& fixture, memory::GuestAddress entry, runtime::RuntimeImportRegistry& registry,
    std::vector<loader::UnresolvedRelocation> imports)
{
    execution::ExecutionSession session(fixture.memory, fixture.function_map, imports, {}, {},
                                        &registry);
    const auto selected = execution::select_entry(
        fixture.function_map.identity(), execution::EntrySelectionKind::DynamicInit);
    if (!selected) return Result<execution::ExecutionSessionResult>::failure(selected.error());
    auto selection = selected.value();
    selection.address = entry;
    return session.run(selection);
}

TEST_CASE("M12 runtime registry is explicit, typed, and deterministic")
{
    runtime::RuntimeImportRegistry registry;
    auto first = descriptor("z_import", 0U, runtime::AbiReturnKind::Void);
    first.support = runtime::RuntimeSupportStatus::Unimplemented;
    auto second = descriptor("a_import", 0U, runtime::AbiReturnKind::Void);
    second.support = runtime::RuntimeSupportStatus::Unimplemented;
    REQUIRE(registry.register_import(std::move(first)));
    REQUIRE(registry.register_import(std::move(second)));
    REQUIRE(registry.size() == 2U);
    REQUIRE(registry.find("missing") == nullptr);
    const auto entries = registry.descriptors();
    REQUIRE(entries.size() == 2U);
    REQUIRE(entries[0].symbol_name == "a_import");
    REQUIRE(entries[1].symbol_name == "z_import");
    auto duplicate = descriptor("a_import", 0U, runtime::AbiReturnKind::Void);
    duplicate.support = runtime::RuntimeSupportStatus::Unimplemented;
    const auto duplicate_result = registry.register_import(std::move(duplicate));
    REQUIRE_FALSE(duplicate_result);
    REQUIRE(duplicate_result.error().code == ErrorCode::DuplicateExternalSymbol);

    auto success = descriptor("increment", 1U, runtime::AbiReturnKind::Integer64);
    REQUIRE(registry.register_import(
        std::move(success), [](runtime::RuntimeImportContext& context) {
            const auto value = context.abi.integer_argument(0U);
            if (!value) return Result<runtime::RuntimeImportOutcome>::failure(value.error());
            const auto written = context.abi.set_integer_return(value.value() + 1U);
            if (!written) return Result<runtime::RuntimeImportOutcome>::failure(written.error());
            return Result<runtime::RuntimeImportOutcome>::success(
                runtime::RuntimeImportOutcome::handled());
        }));
    runtime::CpuState cpu;
    cpu.sp = 0x1000U;
    cpu.x[0] = 41U;
    memory::GuestMemory memory;
    REQUIRE(memory.map(0x1000U, 0x100U,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Write,
                       "synthetic.m12.runtime_stack"));
    const auto* registered = registry.find("increment");
    REQUIRE(registered != nullptr);
    runtime::AArch64GuestCall abi(cpu, memory, registered->signature);
    const auto symbol = import_symbol(7U, "increment");
    runtime::RuntimeState state;
    runtime::RuntimeImportContext context{abi, memory, state, *registered,
                                          provenance_for(symbol),
                                          runtime::ExternalInvocationKind::Call};
    const auto invoked = registry.invoke(context);
    REQUIRE(invoked);
    REQUIRE(invoked.value().kind == runtime::RuntimeImportOutcomeKind::Handled);
    REQUIRE(cpu.x[0] == 42U);

    auto unsupported = descriptor("unsupported", 0U, runtime::AbiReturnKind::Void);
    unsupported.support = runtime::RuntimeSupportStatus::Unknown;
    REQUIRE_FALSE(registry.register_import(std::move(unsupported)));
}

TEST_CASE("M12 AArch64 ABI reads register and checked guest stack arguments")
{
    memory::GuestMemory memory;
    REQUIRE(memory.map(0x1000U, 0x200U,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Write,
                       "synthetic.m12.stack"));
    runtime::CpuState cpu;
    cpu.sp = 0x1080U;
    for (std::size_t index = 0U; index < 8U; ++index) cpu.x[index] = 0x100U + index;
    write_u64_le(memory, 0x1080U, 0x108U);
    write_u64_le(memory, 0x1088U, 0x109U);
    runtime::AbiSignature signature;
    signature.argument_count = 10U;
    signature.arguments.assign(10U, runtime::AbiValueKind::Integer64);
    signature.observed_argument_count = 10U;
    runtime::AArch64GuestCall abi(cpu, memory, signature);
    REQUIRE(abi.validate());
    for (std::size_t index = 0U; index < 10U; ++index)
    {
        const auto value = abi.integer_argument(index);
        REQUIRE(value);
        REQUIRE(value.value() == (index < 8U ? 0x100U + index : 0x100U + index));
    }
    REQUIRE(abi.snapshot().x29 == 0U);
    REQUIRE(abi.set_integer32_return(0xffffffffU));
    REQUIRE(cpu.x[0] == 0xffffffffU);
    REQUIRE(abi.set_integer_return(std::numeric_limits<std::uint64_t>::max()));
    REQUIRE(cpu.x[0] == std::numeric_limits<std::uint64_t>::max());

    cpu.sp = 0x1088U;
    REQUIRE_FALSE(abi.validate());
    REQUIRE(abi.validate().error().code == ErrorCode::InvalidGuestAddress);

    cpu.sp = 0x3000U;
    REQUIRE_FALSE(abi.validate());
    REQUIRE(abi.validate().error().code == ErrorCode::UnmappedMemory);

    memory::GuestMemory write_only;
    REQUIRE(write_only.map(0x3000U, 0x100U, memory::GuestMemoryPermissions::Write,
                            "synthetic.m12.write_only"));
    cpu.sp = 0x3040U;
    runtime::AArch64GuestCall permission_abi(cpu, write_only, signature);
    REQUIRE_FALSE(permission_abi.validate());
    REQUIRE(permission_abi.validate().error().code == ErrorCode::PermissionDenied);

    runtime::AbiSignature huge_signature;
    huge_signature.observed_argument_count = std::numeric_limits<std::size_t>::max();
    runtime::AArch64GuestCall huge_abi(cpu, memory, huge_signature);
    const auto overflow = huge_abi.raw_argument(std::numeric_limits<std::size_t>::max() - 1U);
    REQUIRE_FALSE(overflow);
    REQUIRE(overflow.error().code == ErrorCode::ArithmeticOverflow);
}

TEST_CASE("M12 runtime call resumes the original guest continuation")
{
    constexpr memory::GuestAddress base = 0x1000U;
    constexpr memory::GuestAddress slot = 0x4000U;
    const auto code = words({movz(0U, 7U), movz(1U, static_cast<std::uint16_t>(slot)),
                             0xf9400030U, 0xd63f0200U, 0x91000400U, 0xd65f03c0U});
    auto fixture = make_fixture(base, code, {seed(base)}, std::make_pair(slot, 16U));
    REQUIRE(fixture);
    runtime::RuntimeImportRegistry registry;
    auto import = descriptor("synthetic_runtime_call", 1U, runtime::AbiReturnKind::Integer64);
    REQUIRE(registry.register_import(
        std::move(import), [](runtime::RuntimeImportContext& context) {
            const auto argument = context.abi.integer_argument(0U);
            if (!argument) return Result<runtime::RuntimeImportOutcome>::failure(argument.error());
            const auto written = context.abi.set_integer_return(argument.value() + 5U);
            if (!written) return Result<runtime::RuntimeImportOutcome>::failure(written.error());
            return Result<runtime::RuntimeImportOutcome>::success(
                runtime::RuntimeImportOutcome::handled());
        }));
    const auto result = run_fixture(fixture.value(), base, registry,
                                    {unresolved_import(slot, 3U, "synthetic_runtime_call")});
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(result.value().final_cpu.x[0] == 13U);
    REQUIRE(result.value().runtime.imports_encountered == 1U);
    REQUIRE(result.value().runtime.imports_handled == 1U);
    REQUIRE(result.value().runtime.import_returns == 1U);
    REQUIRE(result.value().runtime.imports.front().invocation ==
            runtime::ExternalInvocationKind::Call);
    REQUIRE(result.value().runtime.imports.front().outcome == "handled");
    REQUIRE(result.value().returns == 1U);
    REQUIRE(result.value().maximum_call_depth == 0U);
}

TEST_CASE("M12 tail runtime import inherits the original BL return contract")
{
    constexpr memory::GuestAddress base = 0x5000U;
    constexpr memory::GuestAddress thunk = base + 0x14U;
    constexpr memory::GuestAddress slot = 0x7000U;
    const auto code = words({0xaa1e03f3U, bl(base + 4U, thunk), 0x91000400U, 0xd65f03c0U,
                             0xd503201fU, 0xaa1303feU,
                             movz(1U, static_cast<std::uint16_t>(slot)), 0xf9400031U,
                             0xd61f0220U});
    auto fixture = make_fixture(base, code, {seed(base), seed(thunk)}, std::make_pair(slot, 16U));
    REQUIRE(fixture);
    runtime::RuntimeImportRegistry registry;
    auto import = descriptor("synthetic_tail_import", 0U, runtime::AbiReturnKind::Integer64);
    REQUIRE(registry.register_import(
        std::move(import), [](runtime::RuntimeImportContext& context) {
            const auto written = context.abi.set_integer_return(12U);
            if (!written) return Result<runtime::RuntimeImportOutcome>::failure(written.error());
            return Result<runtime::RuntimeImportOutcome>::success(
                runtime::RuntimeImportOutcome::handled());
        }));
    const auto result = run_fixture(fixture.value(), base, registry,
                                    {unresolved_import(slot, 4U, "synthetic_tail_import")});
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(result.value().final_cpu.x[0] == 13U);
    REQUIRE(result.value().final_cpu.x[30U] == result.value().synthetic_lr_sentinel);
    REQUIRE(result.value().runtime.imports.front().invocation ==
            runtime::ExternalInvocationKind::TailTransfer);
    REQUIRE(result.value().runtime.imports_handled == 1U);
    REQUIRE(result.value().returns == 1U);
    REQUIRE(result.value().function_transfers == 0U);
    REQUIRE(result.value().maximum_call_depth == 1U);
    REQUIRE(result.value().executed_functions == std::vector<memory::GuestAddress>{base, thunk});
}

TEST_CASE("M12 recognized but unimplemented imports stop distinctly from unknown imports")
{
    constexpr memory::GuestAddress base = 0x9000U;
    constexpr memory::GuestAddress slot = 0xa000U;
    const auto code = words({movz(1U, static_cast<std::uint16_t>(slot)), 0xf9400030U,
                             0xd63f0200U, 0xd65f03c0U});
    auto fixture = make_fixture(base, code, {seed(base)}, std::make_pair(slot, 16U));
    REQUIRE(fixture);
    runtime::RuntimeImportRegistry registry;
    auto known = descriptor("known_unimplemented", 0U, runtime::AbiReturnKind::Void);
    known.support = runtime::RuntimeSupportStatus::Unimplemented;
    REQUIRE(registry.register_import(std::move(known)));
    const auto known_result = run_fixture(
        fixture.value(), base, registry, {unresolved_import(slot, 5U, "known_unimplemented")});
    REQUIRE(known_result);
    REQUIRE(known_result.value().stop_reason ==
            execution::ExecutionStopReason::RuntimeImportUnimplemented);
    REQUIRE(known_result.value().runtime.imports_unimplemented == 1U);
    REQUIRE(known_result.value().runtime.imports.front().outcome == "unimplemented");

    auto unknown_fixture = make_fixture(base + 0x2000U, code, {seed(base + 0x2000U)},
                                        std::make_pair(slot, 16U));
    REQUIRE(unknown_fixture);
    runtime::RuntimeImportRegistry empty;
    const auto unknown_result = run_fixture(
        unknown_fixture.value(), base + 0x2000U, empty,
        {unresolved_import(slot, 5U, "unknown_import")});
    REQUIRE(unknown_result);
    REQUIRE(unknown_result.value().stop_reason == execution::ExecutionStopReason::UnresolvedImport);
    REQUIRE(unknown_result.value().runtime.imports_unimplemented == 0U);
}

TEST_CASE("M12 runtime handlers use checked guest memory and report typed faults")
{
    runtime::RuntimeImportRegistry registry;
    auto read = descriptor("checked_read", 1U, runtime::AbiReturnKind::Integer64);
    REQUIRE(registry.register_import(
        std::move(read), [](runtime::RuntimeImportContext& context) {
            std::array<std::byte, sizeof(std::uint64_t)> bytes{};
            const auto pointer = context.abi.pointer_argument(0U);
            if (!pointer) return Result<runtime::RuntimeImportOutcome>::failure(pointer.error());
            const auto loaded = context.memory.read(pointer.value(), bytes);
            if (!loaded) return Result<runtime::RuntimeImportOutcome>::failure(loaded.error());
            return Result<runtime::RuntimeImportOutcome>::success(
                runtime::RuntimeImportOutcome::handled());
        }));
    runtime::CpuState cpu;
    cpu.sp = 0x1000U;
    cpu.x[0] = 0xdeadU;
    memory::GuestMemory memory;
    REQUIRE(memory.map(0x1000U, 0x100U,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Write,
                       "synthetic.m12.runtime_stack"));
    const auto* registered = registry.find("checked_read");
    REQUIRE(registered != nullptr);
    runtime::AArch64GuestCall abi(cpu, memory, registered->signature);
    const auto symbol = import_symbol(8U, "checked_read");
    runtime::RuntimeState state;
    runtime::RuntimeImportContext context{abi, memory, state, *registered,
                                          provenance_for(symbol)};
    const auto invoked = registry.invoke(context);
    REQUIRE(invoked);
    REQUIRE(invoked.value().kind == runtime::RuntimeImportOutcomeKind::MemoryFault);
    REQUIRE(invoked.value().error);
    REQUIRE(invoked.value().error->code == ErrorCode::UnmappedMemory);
}

TEST_CASE("M12 DSO/TLS registration validates ranges and is transactional")
{
    memory::GuestMemory memory;
    REQUIRE(memory.map(0x1000U, 0x100U,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "synthetic.m12.text", memory::GuestRegionKind::Text));
    REQUIRE(memory.map(0x2000U, 0x100U,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Write,
                       "synthetic.m12.tdata", memory::GuestRegionKind::Data));
    REQUIRE(memory.map(0x3000U, 0x100U,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Write,
                       "synthetic.m12.tbss", memory::GuestRegionKind::Bss));
    runtime::DsoTlsDescriptor valid{0x1000U, 0x1010U, 0x2000U, 0x2010U,
                                    0x3000U, 0x3010U, 0x10U, 0x10U};
    runtime::RuntimeState state;
    REQUIRE(state.register_dso_tls(memory, valid));
    REQUIRE(state.dso_modules_registered() == 1U);
    const auto before = state.dso_modules();

    auto overlap = valid;
    overlap.tdata_begin = 0x1008U;
    overlap.tdata_end = 0x1018U;
    overlap.tdata_alignment = 8U;
    REQUIRE_FALSE(state.register_dso_tls(memory, overlap));
    REQUIRE(state.dso_modules() == before);

    auto bad_alignment = valid;
    bad_alignment.tdata_alignment = 3U;
    REQUIRE_FALSE(state.register_dso_tls(memory, bad_alignment));
    REQUIRE(state.dso_modules() == before);

    auto unmapped = valid;
    unmapped.executable_begin = 0x5000U;
    unmapped.executable_end = 0x5010U;
    REQUIRE_FALSE(state.register_dso_tls(memory, unmapped));
    REQUIRE(state.dso_modules() == before);

    memory::GuestMemory read_only;
    REQUIRE(read_only.map(0x1000U, 0x100U,
                          memory::GuestMemoryPermissions::Read |
                              memory::GuestMemoryPermissions::Execute,
                          "synthetic.m12.text", memory::GuestRegionKind::Text));
    REQUIRE(read_only.map(0x2000U, 0x100U, memory::GuestMemoryPermissions::Read,
                          "synthetic.m12.tdata", memory::GuestRegionKind::Data));
    REQUIRE(read_only.map(0x3000U, 0x100U, memory::GuestMemoryPermissions::Read,
                          "synthetic.m12.tbss", memory::GuestRegionKind::Bss));
    REQUIRE_FALSE(state.register_dso_tls(read_only, valid));
    REQUIRE(state.dso_modules() == before);
}

TEST_CASE("M12 synthetic nnmusl evidence model reports observed slots without faking success")
{
    runtime::RuntimeImportRegistry registry;
    REQUIRE(runtime::register_m12_evidence_imports(registry));
    const auto* descriptor = registry.find("__nnmusl_init_dso");
    REQUIRE(descriptor != nullptr);
    REQUIRE(descriptor->support == runtime::RuntimeSupportStatus::Unimplemented);
    REQUIRE_FALSE(descriptor->signature.argument_count);
    REQUIRE(descriptor->signature.observed_argument_count == 10U);

    memory::GuestMemory memory;
    REQUIRE(memory.map(0x1000U, 0x100U,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Write,
                       "synthetic.m12.stack"));
    runtime::CpuState cpu;
    cpu.sp = 0x1040U;
    cpu.x[0] = 0x1234U;
    for (std::size_t index = 0U; index < 8U; ++index) cpu.x[index] = index + 1U;
    write_u64_le(memory, 0x1040U, 9U);
    write_u64_le(memory, 0x1048U, 10U);
    runtime::AArch64GuestCall abi(cpu, memory, descriptor->signature);
    const auto symbol = import_symbol(5U, "__nnmusl_init_dso");
    runtime::RuntimeState state;
    runtime::RuntimeImportContext context{abi, memory, state, *descriptor,
                                          provenance_for(symbol)};
    const auto invoked = registry.invoke(context);
    REQUIRE(invoked);
    REQUIRE(invoked.value().kind == runtime::RuntimeImportOutcomeKind::Unimplemented);
    REQUIRE(invoked.value().abi_validated);
    REQUIRE(cpu.x[0] == 1U);
    REQUIRE(state.dso_modules_registered() == 0U);
}

} // namespace
