#include "switchrecomp/execution/session.hpp"
#include "switchrecomp/interpreter/interpreter.hpp"
#include "switchrecomp/ir/builder.hpp"
#include "switchrecomp/runtime/atomic_memory.hpp"
#include "switchrecomp/runtime/context.hpp"
#include "switchrecomp/version.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace switchrecomp;

constexpr memory::GuestAddress data_address = 0x1000U;

memory::GuestMemory make_memory()
{
    memory::GuestMemory memory;
    REQUIRE(memory.map(data_address, 0x1000U,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Write,
                       "m25-data", memory::GuestRegionKind::Data));
    REQUIRE(memory.map(0x3000U, 0x1000U,
                       memory::GuestMemoryPermissions::Read,
                       "m25-read-only", memory::GuestRegionKind::Data));
    return memory;
}

[[nodiscard]] ir::Type type_for_width(std::uint8_t width)
{
    switch (width)
    {
    case 1U: return ir::i8_type();
    case 2U: return ir::i16_type();
    case 4U: return ir::i32_type();
    case 8U: return ir::i64_type();
    default: return ir::void_type();
    }
}

[[nodiscard]] ir::GuestRegister x_register(std::uint8_t index)
{
    return ir::GuestRegister{ir::RegisterWidth::X64, index, false, false};
}

[[nodiscard]] ir::GuestRegister w_register(std::uint8_t index)
{
    return ir::GuestRegister{ir::RegisterWidth::W32, index, false, false};
}

void finish_return(ir::Builder& builder)
{
    ir::Terminator terminator;
    terminator.kind = ir::TerminatorKind::Return;
    REQUIRE(builder.set_terminator(terminator));
}

[[nodiscard]] ir::ValueId constant(ir::Builder& builder, ir::Type type, std::uint64_t value)
{
    const auto result = builder.constant(type, value);
    REQUIRE(result);
    return result.value();
}

[[nodiscard]] ir::ValueId emit_value(ir::Builder& builder, ir::Instruction instruction)
{
    const auto result = builder.emit(std::move(instruction));
    REQUIRE(result);
    return result.value();
}

void emit_void(ir::Builder& builder, ir::Instruction instruction)
{
    REQUIRE(builder.emit_void(std::move(instruction)));
}

void write_register(ir::Builder& builder, ir::GuestRegister reg, ir::ValueId value)
{
    ir::Instruction instruction;
    instruction.opcode = ir::Opcode::WriteRegister;
    instruction.reg = reg;
    instruction.operands = {value};
    emit_void(builder, std::move(instruction));
}

[[nodiscard]] ir::ValueId atomic_load(ir::Builder& builder, ir::ValueId address,
                                      std::uint8_t width, ir::MemoryOrder order)
{
    ir::Instruction instruction;
    instruction.opcode = ir::Opcode::AtomicLoad;
    instruction.result_type = type_for_width(width);
    instruction.operands = {address};
    instruction.memory_size = width;
    instruction.memory_order = order;
    return emit_value(builder, std::move(instruction));
}

[[nodiscard]] ir::ValueId exclusive_load(ir::Builder& builder, ir::ValueId address,
                                         std::uint8_t width, ir::MemoryOrder order)
{
    ir::Instruction instruction;
    instruction.opcode = ir::Opcode::ExclusiveLoad;
    instruction.result_type = type_for_width(width);
    instruction.operands = {address};
    instruction.memory_size = width;
    instruction.memory_order = order;
    return emit_value(builder, std::move(instruction));
}

[[nodiscard]] ir::ValueId exclusive_store(ir::Builder& builder, ir::ValueId address,
                                           ir::ValueId value, std::uint8_t width,
                                           ir::MemoryOrder order)
{
    ir::Instruction instruction;
    instruction.opcode = ir::Opcode::ExclusiveStore;
    instruction.result_type = ir::i32_type();
    instruction.operands = {address, value};
    instruction.memory_size = width;
    instruction.memory_order = order;
    return emit_value(builder, std::move(instruction));
}

void atomic_store(ir::Builder& builder, ir::ValueId address, ir::ValueId value,
                  std::uint8_t width, ir::MemoryOrder order)
{
    ir::Instruction instruction;
    instruction.opcode = ir::Opcode::AtomicStore;
    instruction.operands = {address, value};
    instruction.memory_size = width;
    instruction.memory_order = order;
    emit_void(builder, std::move(instruction));
}

void clear_exclusive(ir::Builder& builder)
{
    ir::Instruction instruction;
    instruction.opcode = ir::Opcode::ClearExclusive;
    emit_void(builder, std::move(instruction));
}

void barrier(ir::Builder& builder, ir::BarrierKind kind, ir::BarrierOption option)
{
    ir::Instruction instruction;
    instruction.opcode = ir::Opcode::MemoryBarrier;
    instruction.barrier_kind = kind;
    instruction.barrier_option = option;
    emit_void(builder, std::move(instruction));
}

[[nodiscard]] ir::ValueId read_system_register(ir::Builder& builder, ir::SystemRegister reg)
{
    ir::Instruction instruction;
    instruction.opcode = ir::Opcode::ReadSystemRegister;
    instruction.result_type = ir::i64_type();
    instruction.system_register = reg;
    return emit_value(builder, std::move(instruction));
}

void write_system_register(ir::Builder& builder, ir::SystemRegister reg, ir::ValueId value)
{
    ir::Instruction instruction;
    instruction.opcode = ir::Opcode::WriteSystemRegister;
    instruction.system_register = reg;
    instruction.operands = {value};
    emit_void(builder, std::move(instruction));
}

[[nodiscard]] ir::Function make_full_m9_function()
{
    ir::Function function("m25-full-m9", 0x5000U);
    const auto block = function.add_block(0x5000U, "entry");
    function.set_entry_block(block);
    ir::Builder builder(function);
    REQUIRE(builder.set_insert_block(block));

    const auto address = constant(builder, ir::i64_type(), data_address);
    const auto stored = constant(builder, ir::i64_type(), 0x1122334455667788U);
    atomic_store(builder, address, stored, 8U, ir::MemoryOrder::Release);
    const auto loaded = atomic_load(builder, address, 8U, ir::MemoryOrder::Acquire);
    write_register(builder, x_register(4U), loaded);
    const auto reserved = exclusive_load(builder, address, 8U, ir::MemoryOrder::Acquire);
    write_register(builder, x_register(5U), reserved);
    const auto status = exclusive_store(builder, address, stored, 8U, ir::MemoryOrder::Release);
    write_register(builder, w_register(6U), status);
    clear_exclusive(builder);
    barrier(builder, ir::BarrierKind::Dmb, ir::BarrierOption::Sy);
    barrier(builder, ir::BarrierKind::Dsb, ir::BarrierOption::Ish);
    barrier(builder, ir::BarrierKind::Isb, ir::BarrierOption::Oshld);
    const auto tls = constant(builder, ir::i64_type(), 0xabcdef0123456789U);
    write_system_register(builder, ir::SystemRegister::TpidrEl0, tls);
    write_register(builder, x_register(7U),
                   read_system_register(builder, ir::SystemRegister::TpidrEl0));
    write_register(builder, x_register(8U),
                   read_system_register(builder, ir::SystemRegister::TpidrroEl0));
    finish_return(builder);
    return function;
}

[[nodiscard]] ir::Function make_atomic_roundtrip(std::uint8_t width,
                                                  ir::MemoryOrder store_order,
                                                  ir::MemoryOrder load_order,
                                                  memory::GuestAddress address = data_address)
{
    ir::Function function("m25-atomic-roundtrip", 0x5100U);
    const auto block = function.add_block(0x5100U, "entry");
    function.set_entry_block(block);
    ir::Builder builder(function);
    REQUIRE(builder.set_insert_block(block));
    const auto address_value = constant(builder, ir::i64_type(), address);
    const auto stored_value = width == 1U ? 0x11U : width == 2U ? 0x2211U
                               : width == 4U ? 0x44332211U : 0x8877665544332211U;
    const auto value = constant(builder, type_for_width(width), stored_value);
    atomic_store(builder, address_value, value, width, store_order);
    const auto loaded = atomic_load(builder, address_value, width, load_order);
    if (width < 4U)
    {
        ir::Instruction extend;
        extend.opcode = ir::Opcode::ZeroExtend;
        extend.result_type = ir::i32_type();
        extend.operands = {loaded};
        write_register(builder, w_register(0U), emit_value(builder, std::move(extend)));
    }
    else if (width == 4U)
    {
        write_register(builder, w_register(0U), loaded);
    }
    else
    {
        write_register(builder, x_register(0U), loaded);
    }
    finish_return(builder);
    return function;
}

[[nodiscard]] ir::Function make_exclusive_function(bool invalidate, bool clear,
                                                    bool address_mismatch,
                                                    bool width_mismatch,
                                                    ir::MemoryOrder load_order = ir::MemoryOrder::Relaxed,
                                                    ir::MemoryOrder store_order = ir::MemoryOrder::Relaxed)
{
    ir::Function function("m25-exclusive", 0x5200U);
    const auto block = function.add_block(0x5200U, "entry");
    function.set_entry_block(block);
    ir::Builder builder(function);
    REQUIRE(builder.set_insert_block(block));
    const auto address = constant(builder, ir::i64_type(), data_address);
    const auto other_address = constant(builder, ir::i64_type(), data_address + 8U);
    const auto old_value = constant(builder, ir::i64_type(), 0x11U);
    const auto new_value = constant(builder, width_mismatch ? ir::i32_type() : ir::i64_type(), 0x22U);
    (void)exclusive_load(builder, address, 8U, load_order);
    if (invalidate) atomic_store(builder, address, old_value, 8U, ir::MemoryOrder::Relaxed);
    if (clear) clear_exclusive(builder);
    const auto status = exclusive_store(builder, address_mismatch ? other_address : address,
                                        new_value, width_mismatch ? 4U : 8U, store_order);
    write_register(builder, w_register(0U), status);
    finish_return(builder);
    return function;
}

[[nodiscard]] ir::Function make_tls_function()
{
    ir::Function function("m25-tls", 0x5300U);
    const auto block = function.add_block(0x5300U, "entry");
    function.set_entry_block(block);
    ir::Builder builder(function);
    REQUIRE(builder.set_insert_block(block));
    const auto value = constant(builder, ir::i64_type(), 0x1111222233334444U);
    write_system_register(builder, ir::SystemRegister::TpidrEl0, value);
    write_register(builder, x_register(0U),
                   read_system_register(builder, ir::SystemRegister::TpidrEl0));
    write_register(builder, x_register(1U),
                   read_system_register(builder, ir::SystemRegister::TpidrroEl0));
    finish_return(builder);
    return function;
}

[[nodiscard]] ir::Function make_barrier_function()
{
    ir::Function function("m25-barriers", 0x5400U);
    const auto block = function.add_block(0x5400U, "entry");
    function.set_entry_block(block);
    ir::Builder builder(function);
    REQUIRE(builder.set_insert_block(block));
    barrier(builder, ir::BarrierKind::Dmb, ir::BarrierOption::Sy);
    barrier(builder, ir::BarrierKind::Dsb, ir::BarrierOption::Ishst);
    barrier(builder, ir::BarrierKind::Isb, ir::BarrierOption::Oshld);
    finish_return(builder);
    return function;
}

[[nodiscard]] ir::Function make_invalid_atomic_function(std::uint8_t width,
                                                        ir::MemoryOrder order,
                                                        ir::Opcode opcode = ir::Opcode::AtomicLoad,
                                                        memory::GuestAddress address = data_address)
{
    ir::Function function("m25-invalid-atomic", 0x5500U);
    const auto block = function.add_block(0x5500U, "entry");
    function.set_entry_block(block);
    ir::Builder builder(function);
    REQUIRE(builder.set_insert_block(block));
    const auto address_value = constant(builder, ir::i64_type(), address);
    ir::Instruction operation;
    operation.opcode = opcode;
    operation.result_type = type_for_width(width);
    operation.operands = {address_value};
    operation.memory_size = width;
    operation.memory_order = order;
    (void)emit_value(builder, std::move(operation));
    finish_return(builder);
    return function;
}

[[nodiscard]] ir::Function make_boundary_function()
{
    ir::Function function("m25-resumable-boundary", 0x5600U);
    const auto entry = function.add_block(0x5600U, "entry");
    const auto continuation = function.add_block(0x560cU, "continuation");
    function.set_entry_block(entry);
    ir::Builder builder(function);
    REQUIRE(builder.set_insert_block(entry));
    ir::Instruction set_pc;
    set_pc.opcode = ir::Opcode::SetPc;
    set_pc.source.guest_pc = 0x5600U;
    emit_void(builder, std::move(set_pc));
    const auto address = constant(builder, ir::i64_type(), data_address);
    const auto value = constant(builder, ir::i64_type(), 0x42U);
    atomic_store(builder, address, value, 8U, ir::MemoryOrder::Relaxed);
    const auto target = constant(builder, ir::i64_type(), 0x9000U);
    ir::Terminator call;
    call.kind = ir::TerminatorKind::DirectCall;
    call.target_value = target;
    call.continuation = continuation;
    call.continuation_guest_pc = 0x560cU;
    call.source.guest_pc = 0x5608U;
    REQUIRE(builder.set_terminator(call));

    REQUIRE(builder.set_insert_block(continuation));
    ir::Instruction continuation_pc;
    continuation_pc.opcode = ir::Opcode::SetPc;
    continuation_pc.source.guest_pc = 0x560cU;
    emit_void(builder, std::move(continuation_pc));
    const auto loaded = atomic_load(builder, address, 8U, ir::MemoryOrder::Acquire);
    write_register(builder, x_register(0U), loaded);
    finish_return(builder);
    return function;
}

[[nodiscard]] ir::Function make_provenance_function(ir::Opcode load_opcode)
{
    ir::Function function("m25-load-provenance", 0x5700U);
    const auto block = function.add_block(0x5700U, "entry");
    function.set_entry_block(block);
    ir::Builder builder(function);
    REQUIRE(builder.set_insert_block(block));
    const auto address = constant(builder, ir::i64_type(), data_address);
    const auto loaded = load_opcode == ir::Opcode::AtomicLoad
                            ? atomic_load(builder, address, 8U, ir::MemoryOrder::Relaxed)
                            : exclusive_load(builder, address, 8U, ir::MemoryOrder::Relaxed);
    ir::Terminator branch;
    branch.kind = ir::TerminatorKind::IndirectBranch;
    branch.target_value = loaded;
    branch.source.guest_pc = 0x5708U;
    REQUIRE(builder.set_terminator(branch));
    return function;
}

[[nodiscard]] std::vector<std::byte> bytes_at(const memory::GuestMemory& memory,
                                               memory::GuestAddress address,
                                               std::size_t size)
{
    std::vector<std::byte> bytes(size);
    REQUIRE(memory.read(address, std::span<std::byte>(bytes)));
    return bytes;
}

[[nodiscard]] Result<runtime::ExecutionResult> run_reference(
    const ir::Function& function, memory::GuestMemory& memory, runtime::CpuState& cpu)
{
    runtime::SharedRuntimeState shared(memory);
    runtime::RuntimeContext context{&memory};
    context.shared = &shared;
    return interpreter::execute(function, cpu, context);
}

[[nodiscard]] Result<runtime::ExecutionResult> run_resumable(
    const ir::Function& function, memory::GuestMemory& memory, runtime::CpuState& cpu,
    interpreter::InterpreterFrame& frame)
{
    runtime::SharedRuntimeState shared(memory);
    runtime::RuntimeContext context{&memory};
    context.shared = &shared;
    return interpreter::execute_until_boundary(function, cpu, context, frame);
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

[[nodiscard]] std::uint32_t movz(std::uint8_t reg, std::uint16_t value)
{
    return 0xd2800000U | (static_cast<std::uint32_t>(value) << 5U) | reg;
}

[[nodiscard]] std::uint32_t ldxr_w(std::uint8_t target, std::uint8_t address_register)
{
    return 0x885f7c00U | target | (static_cast<std::uint32_t>(address_register) << 5U);
}

struct SessionFixture
{
    memory::GuestMemory memory;
    analysis::FinalizedFunctionMap map;
};

[[nodiscard]] Result<SessionFixture> make_session_fixture()
{
    SessionFixture fixture;
    constexpr memory::GuestAddress code_address = 0x6000U;
    constexpr memory::GuestAddress loaded_address = 0x7000U;
    const auto code = words({movz(10U, static_cast<std::uint16_t>(loaded_address)),
                             ldxr_w(20U, 10U), 0xd65f03c0U});
    const auto mapped = fixture.memory.map(
        code_address, std::span<const std::byte>(code),
        memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Execute,
        "m25-session-text", memory::GuestRegionKind::Text);
    if (!mapped) return Result<SessionFixture>::failure(mapped.error());
    const auto data_mapped = fixture.memory.map(
        loaded_address, 0x1000U,
        memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Write,
        "m25-session-data", memory::GuestRegionKind::Data);
    if (!data_mapped) return Result<SessionFixture>::failure(data_mapped.error());
    const std::array<std::byte, 4> value{
        std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12}};
    const auto initialized = fixture.memory.write(loaded_address, value);
    if (!initialized) return Result<SessionFixture>::failure(initialized.error());

    analysis::ModuleIdentity identity;
    identity.module = "synthetic-m25";
    identity.build_id = "synthetic-m25-build";
    identity.input_sha256 = "synthetic-m25-sha";
    identity.translator_version = version;
    identity.guest_base = code_address;
    identity.executable_ranges.push_back({code_address, static_cast<memory::GuestSize>(code.size())});
    identity.entry_points.push_back({code_address, analysis::EntryPointKind::DynamicInit,
                                     "synthetic M25 DT_INIT", analysis::FunctionConfidence::High,
                                     false, "synthetic resumable M9 entry"});
    const analysis::FunctionSeed seed{
        code_address, analysis::FunctionDiscoverySource::AnalystSeed,
        analysis::FunctionConfidence::Manual, std::nullopt, std::nullopt,
        "synthetic M25 session entry"};
    const auto finalized = analysis::FunctionMapBuilder::build(
        analysis::ModuleAnalysisInput{std::move(identity), &fixture.memory, {seed}});
    if (!finalized) return Result<SessionFixture>::failure(finalized.error());
    fixture.map = std::move(finalized).value();
    return Result<SessionFixture>::success(std::move(fixture));
}

} // namespace

TEST_CASE("M25 supported M9 operations are differential-equivalent")
{
    const auto function = make_full_m9_function();
    auto reference_memory = make_memory();
    auto resumable_memory = make_memory();
    runtime::CpuState reference_cpu{};
    runtime::CpuState resumable_cpu{};
    reference_cpu.tpidrro_el0 = 0xfeedfacecafebeefU;
    resumable_cpu.tpidrro_el0 = reference_cpu.tpidrro_el0;
    interpreter::InterpreterFrame frame;

    const auto reference = run_reference(function, reference_memory, reference_cpu);
    const auto resumable = run_resumable(function, resumable_memory, resumable_cpu, frame);
    REQUIRE(reference);
    REQUIRE(resumable);
    REQUIRE(reference.value().status == resumable.value().status);
    REQUIRE(reference.value().final_guest_pc == resumable.value().final_guest_pc);
    REQUIRE(reference_cpu.x == resumable_cpu.x);
    REQUIRE(reference_cpu.sp == resumable_cpu.sp);
    REQUIRE(reference_cpu.pc == resumable_cpu.pc);
    REQUIRE(reference_cpu.n == resumable_cpu.n);
    REQUIRE(reference_cpu.z == resumable_cpu.z);
    REQUIRE(reference_cpu.c == resumable_cpu.c);
    REQUIRE(reference_cpu.v == resumable_cpu.v);
    REQUIRE(reference_cpu.tpidr_el0 == resumable_cpu.tpidr_el0);
    REQUIRE(reference_cpu.tpidrro_el0 == resumable_cpu.tpidrro_el0);
    REQUIRE(bytes_at(reference_memory, data_address, 8U) ==
            bytes_at(resumable_memory, data_address, 8U));
    REQUIRE(reference_cpu.x[4U] == 0x1122334455667788U);
    REQUIRE(reference_cpu.x[5U] == 0x1122334455667788U);
    REQUIRE(reference_cpu.x[6U] == 0U);
    REQUIRE(reference_cpu.x[7U] == 0xabcdef0123456789U);
    REQUIRE(reference_cpu.x[8U] == 0xfeedfacecafebeefU);
}

TEST_CASE("M25 atomic load/store covers all widths, ordering, and little endian state")
{
    for (const auto width : {std::uint8_t{1U}, std::uint8_t{2U},
                            std::uint8_t{4U}, std::uint8_t{8U}})
    {
        for (const auto store_order : {ir::MemoryOrder::Relaxed,
                                       ir::MemoryOrder::Release,
                                       ir::MemoryOrder::SequentiallyConsistent})
        {
            for (const auto load_order : {ir::MemoryOrder::Relaxed,
                                          ir::MemoryOrder::Acquire,
                                          ir::MemoryOrder::SequentiallyConsistent})
            {
                const auto function = make_atomic_roundtrip(width, store_order, load_order);
                auto memory = make_memory();
                runtime::CpuState cpu{};
                interpreter::InterpreterFrame frame;
                const auto result = run_resumable(function, memory, cpu, frame);
                REQUIRE(result);
                const auto expected = width == 1U ? 0x11U : width == 2U ? 0x2211U
                                      : width == 4U ? 0x44332211U : 0x8877665544332211U;
                REQUIRE(cpu.x[0U] == expected);
                const auto bytes = bytes_at(memory, data_address, width);
                REQUIRE(std::to_integer<unsigned int>(bytes.front()) == 0x11U);
                if (width > 1U) REQUIRE(std::to_integer<unsigned int>(bytes[1U]) == 0x22U);
                if (width > 2U) REQUIRE(std::to_integer<unsigned int>(bytes[2U]) == 0x33U);
                if (width > 3U) REQUIRE(std::to_integer<unsigned int>(bytes[3U]) == 0x44U);
            }
        }
    }
}

TEST_CASE("M25 resumable exclusive operations preserve reservation semantics")
{
    const auto run_status = [](bool invalidate, bool clear, bool mismatch, bool width_mismatch,
                               ir::MemoryOrder load_order = ir::MemoryOrder::Relaxed,
                               ir::MemoryOrder store_order = ir::MemoryOrder::Relaxed) {
        auto memory = make_memory();
        runtime::CpuState cpu{};
        interpreter::InterpreterFrame frame;
        const auto function = make_exclusive_function(invalidate, clear, mismatch, width_mismatch,
                                                       load_order, store_order);
        const auto result = run_resumable(function, memory, cpu, frame);
        REQUIRE(result);
        return cpu.x[0U];
    };

    REQUIRE(run_status(false, false, false, false) == 0U);
    REQUIRE(run_status(true, false, false, false) == 1U);
    REQUIRE(run_status(false, false, true, false) == 1U);
    REQUIRE(run_status(false, false, false, true) == 1U);
    REQUIRE(run_status(false, true, false, false) == 1U);
    REQUIRE(run_status(false, false, false, false, ir::MemoryOrder::Acquire,
                      ir::MemoryOrder::Release) == 0U);
}

TEST_CASE("M25 barriers and TLS use the current CPU in resumable execution")
{
    {
        auto memory = make_memory();
        runtime::CpuState cpu{};
        interpreter::InterpreterFrame frame;
        const auto function = make_barrier_function();
        const auto result = run_resumable(function, memory, cpu, frame);
        REQUIRE(result);
    }

    auto reference_memory = make_memory();
    auto resumable_memory = make_memory();
    runtime::CpuState reference_cpu{};
    runtime::CpuState resumable_cpu{};
    reference_cpu.tpidrro_el0 = 0x9999U;
    resumable_cpu.tpidrro_el0 = reference_cpu.tpidrro_el0;
    interpreter::InterpreterFrame frame;
    const auto function = make_tls_function();
    const auto reference = run_reference(function, reference_memory, reference_cpu);
    const auto resumable = run_resumable(function, resumable_memory, resumable_cpu, frame);
    REQUIRE(reference);
    REQUIRE(resumable);
    REQUIRE(reference_cpu.x[0U] == resumable_cpu.x[0U]);
    REQUIRE(reference_cpu.x[1U] == resumable_cpu.x[1U]);
    REQUIRE(resumable_cpu.x[0U] == 0x1111222233334444U);
    REQUIRE(resumable_cpu.x[1U] == 0x9999U);
}

TEST_CASE("M25 structured M9 failures remain typed in the resumable path")
{
    {
        auto memory = make_memory();
        runtime::CpuState cpu{};
        interpreter::InterpreterFrame frame;
        const auto function = make_invalid_atomic_function(3U, ir::MemoryOrder::Relaxed);
        const auto result = run_resumable(function, memory, cpu, frame);
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == ErrorCode::IrVerificationFailed);
    }
    {
        auto memory = make_memory();
        runtime::CpuState cpu{};
        interpreter::InterpreterFrame frame;
        const auto function = make_atomic_roundtrip(8U, ir::MemoryOrder::Relaxed,
                                                    ir::MemoryOrder::Relaxed, data_address + 1U);
        const auto result = run_resumable(function, memory, cpu, frame);
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == ErrorCode::MisalignedAtomicAccess);
    }
    {
        auto memory = make_memory();
        runtime::CpuState cpu{};
        interpreter::InterpreterFrame frame;
        const auto function = make_atomic_roundtrip(8U, ir::MemoryOrder::Relaxed,
                                                    ir::MemoryOrder::Relaxed, 0xf000U);
        const auto result = run_resumable(function, memory, cpu, frame);
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == ErrorCode::UnmappedMemory);
    }
    {
        auto memory = make_memory();
        runtime::CpuState cpu{};
        interpreter::InterpreterFrame frame;
        const auto function = make_atomic_roundtrip(8U, ir::MemoryOrder::Relaxed,
                                                    ir::MemoryOrder::Relaxed, 0x3000U);
        const auto result = run_resumable(function, memory, cpu, frame);
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == ErrorCode::PermissionDenied);
    }
    {
        auto memory = make_memory();
        runtime::CpuState cpu{};
        interpreter::InterpreterFrame frame;
        const auto function = make_invalid_atomic_function(
            8U, static_cast<ir::MemoryOrder>(99U));
        const auto result = run_resumable(function, memory, cpu, frame);
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == ErrorCode::IrVerificationFailed);
    }
    {
        auto memory = make_memory();
        runtime::CpuState cpu{};
        interpreter::InterpreterFrame frame;
        // The verifier owns read-only system-register validation; keep the
        // invalid form synthetic and independent from a target instruction.
        ir::Function readonly("m25-readonly-tls", 0x5510U);
        const auto block = readonly.add_block(0x5510U, "entry");
        readonly.set_entry_block(block);
        ir::Builder builder(readonly);
        REQUIRE(builder.set_insert_block(block));
        const auto value = constant(builder, ir::i64_type(), 1U);
        write_system_register(builder, ir::SystemRegister::TpidrroEl0, value);
        finish_return(builder);
        const auto result = run_resumable(readonly, memory, cpu, frame);
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == ErrorCode::IrVerificationFailed);
    }
    {
        auto memory = make_memory();
        runtime::CpuState cpu{};
        interpreter::InterpreterFrame frame;
        ir::Function invalid_barrier("m25-invalid-barrier", 0x5520U);
        const auto block = invalid_barrier.add_block(0x5520U, "entry");
        invalid_barrier.set_entry_block(block);
        ir::Builder builder(invalid_barrier);
        REQUIRE(builder.set_insert_block(block));
        ir::Instruction instruction;
        instruction.opcode = ir::Opcode::MemoryBarrier;
        instruction.barrier_kind = static_cast<ir::BarrierKind>(99U);
        instruction.barrier_option = ir::BarrierOption::Sy;
        emit_void(builder, std::move(instruction));
        finish_return(builder);
        const auto result = run_resumable(invalid_barrier, memory, cpu, frame);
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == ErrorCode::IrVerificationFailed);
    }
    {
        auto memory = make_memory();
        runtime::CpuState cpu{};
        interpreter::InterpreterFrame frame;
        ir::Function invalid_register("m25-invalid-register", 0x5530U);
        const auto block = invalid_register.add_block(0x5530U, "entry");
        invalid_register.set_entry_block(block);
        ir::Builder builder(invalid_register);
        REQUIRE(builder.set_insert_block(block));
        ir::Instruction instruction;
        instruction.opcode = ir::Opcode::ReadSystemRegister;
        instruction.result_type = ir::i64_type();
        instruction.system_register = static_cast<ir::SystemRegister>(99U);
        (void)emit_value(builder, std::move(instruction));
        finish_return(builder);
        const auto result = run_resumable(invalid_register, memory, cpu, frame);
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == ErrorCode::IrVerificationFailed);
    }
}

TEST_CASE("M25 InterpreterFrame continues after an M9 operation and call boundary")
{
    auto memory = make_memory();
    runtime::CpuState cpu{};
    interpreter::InterpreterFrame frame;
    const auto function = make_boundary_function();
    const auto first = run_resumable(function, memory, cpu, frame);
    REQUIRE(first);
    REQUIRE(first.value().status == runtime::ExecutionStatus::Boundary);
    REQUIRE(first.value().boundary.kind == runtime::ExecutionBoundaryKind::DirectCall);
    REQUIRE(first.value().boundary.target_guest_address == 0x9000U);
    REQUIRE(first.value().executed_guest_pcs == std::vector<std::uint64_t>{0x5600U});
    REQUIRE(frame.current_block == function.entry_block());

    frame.current_block = first.value().boundary.continuation_block;
    cpu.pc = first.value().boundary.continuation_guest_pc;
    const auto second = run_resumable(function, memory, cpu, frame);
    REQUIRE(second);
    REQUIRE(second.value().status == runtime::ExecutionStatus::Returned);
    REQUIRE(second.value().boundary.kind == runtime::ExecutionBoundaryKind::Return);
    REQUIRE(second.value().executed_guest_pcs == std::vector<std::uint64_t>{0x560cU});
    REQUIRE(cpu.x[0U] == 0x42U);
    REQUIRE(cpu.pc == 0x560cU);
    REQUIRE(frame.current_block == first.value().boundary.continuation_block);
}

TEST_CASE("M25 atomic and exclusive loads retain truthful guest-load provenance")
{
    for (const auto opcode : {ir::Opcode::AtomicLoad, ir::Opcode::ExclusiveLoad})
    {
        auto memory = make_memory();
        const std::array<std::byte, 8> target{
            std::byte{0x00}, std::byte{0x70}, std::byte{0x00}, std::byte{0x00},
            std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
        REQUIRE(memory.write(data_address, target));
        runtime::CpuState cpu{};
        interpreter::InterpreterFrame frame;
        const auto function = make_provenance_function(opcode);
        const auto result = run_resumable(function, memory, cpu, frame);
        REQUIRE(result);
        REQUIRE(result.value().boundary.kind == runtime::ExecutionBoundaryKind::IndirectBranch);
        REQUIRE(result.value().boundary.target_guest_address == 0x7000U);
        REQUIRE(result.value().boundary.has_provenance_address);
        REQUIRE(result.value().boundary.provenance_address == data_address);
        REQUIRE(result.value().boundary.target_provenance ==
                "guest_load:0x0000000000001000");
    }
}

TEST_CASE("M25 ExecutionSession executes the structural LDXR frontier")
{
    auto fixture = make_session_fixture();
    REQUIRE(fixture);
    execution::ExecutionSession session(fixture.value().memory, fixture.value().map, {});
    const auto selected = execution::select_entry(
        fixture.value().map.identity(), execution::EntrySelectionKind::DynamicInit);
    REQUIRE(selected);
    const auto result = session.run(selected.value());
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(result.value().final_cpu.x[20U] == 0x12345678U);
    REQUIRE(result.value().guest_instruction_count == 3U);
    REQUIRE(result.value().runtime.imports_handled == 0U);
}
