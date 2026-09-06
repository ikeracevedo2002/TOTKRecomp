#include "switchrecomp/analysis/cfg_analyzer.hpp"
#include "switchrecomp/interpreter/interpreter.hpp"
#include "switchrecomp/ir/printer.hpp"
#include "switchrecomp/lifter/lifter.hpp"
#include "switchrecomp/memory/guest_memory.hpp"
#include "switchrecomp/runtime/atomic_memory.hpp"
#include "switchrecomp/runtime/context.hpp"
#include "switchrecomp/runtime/thread.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

namespace
{
using namespace switchrecomp;

memory::GuestMemory make_memory()
{
    memory::GuestMemory memory;
    REQUIRE(memory.map(0x1000U, 0x3ffcU,
        memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Write,
        "m9-shared"));
    REQUIRE(memory.map(0x8000U, 0x1000U, memory::GuestMemoryPermissions::Read,
        "m9-ro"));
    return memory;
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
}

TEST_CASE("Milestone 9 decoder CFG lifter and interpreter execute an exclusive sequence")
{
    auto memory = make_memory();
    const auto code = words({
        0xc85f7c20U, // ldxr x0, [x1]
        0xc8027c20U, // stxr w2, x0, [x1]
        0xd65f03c0U, // ret
    });
    REQUIRE(memory.map(0x6000U, std::span<const std::byte>(code),
        memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Execute,
        "m9-code", memory::GuestRegionKind::Text));

    analysis::AnalysisOptions options;
    options.allowed_code_range = analysis::GuestAddressRange{0x6000U, code.size()};
    const auto cfg = analysis::analyze_control_flow(memory, 0x6000U, options);
    REQUIRE(cfg);
    REQUIRE(cfg.value().instruction_count == 3U);
    REQUIRE(cfg.value().blocks.begin()->second.instructions[0].id == aarch64::InstructionId::Ldxr);
    REQUIRE(cfg.value().blocks.begin()->second.instructions[1].id == aarch64::InstructionId::Stxr);

    const auto lifted = lifter::lift_function(cfg.value());
    if (!lifted) UNSCOPED_INFO(lifted.error().message);
    REQUIRE(lifted);
    bool has_exclusive_load = false;
    bool has_exclusive_store = false;
    for (const auto& block : lifted.value().blocks())
    {
        for (const auto& instruction : block.instructions)
        {
            has_exclusive_load |= instruction.opcode == ir::Opcode::ExclusiveLoad;
            has_exclusive_store |= instruction.opcode == ir::Opcode::ExclusiveStore;
        }
    }
    REQUIRE(has_exclusive_load);
    REQUIRE(has_exclusive_store);

    runtime::SharedRuntimeState shared(memory);
    REQUIRE(runtime::synchronized_store(shared, 0x1000U, 8U, 0x1122334455667788U,
        ir::MemoryOrder::Relaxed));
    runtime::CpuState cpu{};
    cpu.x[1] = 0x1000U;
    runtime::RuntimeContext context{&memory};
    context.shared = &shared;
    context.cpu = &cpu;
    const auto executed = interpreter::execute(lifted.value(), cpu, context);
    REQUIRE(executed);
    REQUIRE(cpu.x[0] == 0x1122334455667788U);
    REQUIRE(cpu.x[2] == 0U);
}

TEST_CASE("Milestone 9 normalization preserves acquire release TLS and barrier semantics")
{
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);

    const auto ldaxr = decoder.value()->decode(0x6000U, 0xc85ffc20U); // ldaxr x0, [x1]
    const auto stlxr = decoder.value()->decode(0x6004U, 0xc802fc20U); // stlxr w2, x0, [x1]
    const auto ldar = decoder.value()->decode(0x6008U, 0xc8dffc20U); // ldar x3, [x1]
    const auto stlr = decoder.value()->decode(0x600cU, 0xc89ffc20U); // stlr x3, [x1]
    const auto clrex = decoder.value()->decode(0x6010U, 0xd503305fU); // clrex
    const auto dmb = decoder.value()->decode(0x6014U, 0xd5033fbfU); // dmb sy
    const auto dsb = decoder.value()->decode(0x6018U, 0xd5033f9fU); // dsb sy
    const auto isb = decoder.value()->decode(0x601cU, 0xd5033fdfU); // isb
    const auto mrs = decoder.value()->decode(0x6020U, 0xd53bd040U); // mrs x0, tpidr_el0
    const auto msr = decoder.value()->decode(0x6024U, 0xd51bd040U); // msr tpidr_el0, x0
    REQUIRE(ldaxr); REQUIRE(stlxr); REQUIRE(ldar); REQUIRE(stlr);
    REQUIRE(clrex); REQUIRE(dmb); REQUIRE(dsb); REQUIRE(isb);
    REQUIRE(mrs); REQUIRE(msr);
    REQUIRE(ldaxr.value().id == aarch64::InstructionId::Ldaxr);
    REQUIRE(ldaxr.value().memory_order == aarch64::AtomicMemoryOrder::Acquire);
    REQUIRE(stlxr.value().id == aarch64::InstructionId::Stlxr);
    REQUIRE(stlxr.value().memory_order == aarch64::AtomicMemoryOrder::Release);
    REQUIRE(ldar.value().id == aarch64::InstructionId::Ldar);
    REQUIRE(stlr.value().id == aarch64::InstructionId::Stlr);
    REQUIRE(clrex.value().id == aarch64::InstructionId::Clrex);
    REQUIRE(dmb.value().barrier_kind == aarch64::BarrierKind::Dmb);
    REQUIRE(dmb.value().barrier_option == aarch64::BarrierOption::Sy);
    REQUIRE(dsb.value().barrier_kind == aarch64::BarrierKind::Dsb);
    REQUIRE(isb.value().barrier_kind == aarch64::BarrierKind::Isb);
    REQUIRE(mrs.value().id == aarch64::InstructionId::Mrs);
    REQUIRE(mrs.value().system_register == aarch64::SystemRegister::TpidrEl0);
    REQUIRE(msr.value().id == aarch64::InstructionId::Msr);
    REQUIRE(msr.value().system_register == aarch64::SystemRegister::TpidrEl0);
}

TEST_CASE("Milestone 9 lifted acquire release barriers and TLS execute through the interpreter")
{
    auto memory = make_memory();
    const auto code = words({
        0xaa0503e0U, // mov x0, x5
        0xd51bd040U, // msr tpidr_el0, x0
        0xd53bd044U, // mrs x4, tpidr_el0
        0xc85ffc20U, // ldaxr x0, [x1]
        0xc802fc20U, // stlxr w2, x0, [x1]
        0xc8dffc23U, // ldar x3, [x1]
        0xc89ffc23U, // stlr x3, [x1]
        0xd503305fU, // clrex
        0xd5033fbfU, // dmb sy
        0xd5033f9fU, // dsb sy
        0xd5033fdfU, // isb
        0xd65f03c0U, // ret
    });
    REQUIRE(memory.map(0x6000U, std::span<const std::byte>(code),
        memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Execute,
        "m9-code", memory::GuestRegionKind::Text));
    analysis::AnalysisOptions options;
    options.allowed_code_range = analysis::GuestAddressRange{0x6000U, code.size()};
    const auto cfg = analysis::analyze_control_flow(memory, 0x6000U, options);
    REQUIRE(cfg);
    const auto lifted = lifter::lift_function(cfg.value());
    REQUIRE(lifted);

    bool saw_acquire = false;
    bool saw_release_exclusive = false;
    bool saw_barrier_dmb = false;
    bool saw_barrier_dsb = false;
    bool saw_barrier_isb = false;
    for (const auto& block : lifted.value().blocks())
    {
        for (const auto& instruction : block.instructions)
        {
            saw_acquire |= instruction.opcode == ir::Opcode::ExclusiveLoad &&
                instruction.memory_order == ir::MemoryOrder::Acquire;
            saw_release_exclusive |= instruction.opcode == ir::Opcode::ExclusiveStore &&
                instruction.memory_order == ir::MemoryOrder::Release;
            saw_barrier_dmb |= instruction.opcode == ir::Opcode::MemoryBarrier &&
                instruction.barrier_kind == ir::BarrierKind::Dmb;
            saw_barrier_dsb |= instruction.opcode == ir::Opcode::MemoryBarrier &&
                instruction.barrier_kind == ir::BarrierKind::Dsb;
            saw_barrier_isb |= instruction.opcode == ir::Opcode::MemoryBarrier &&
                instruction.barrier_kind == ir::BarrierKind::Isb;
        }
    }
    REQUIRE(saw_acquire);
    REQUIRE(saw_release_exclusive);
    REQUIRE(saw_barrier_dmb);
    REQUIRE(saw_barrier_dsb);
    REQUIRE(saw_barrier_isb);
    const auto printed = ir::print(lifted.value());
    REQUIRE(printed.find("memory_barrier kind=dmb option=sy") != std::string::npos);
    REQUIRE(printed.find("memory_barrier kind=dsb option=sy") != std::string::npos);
    REQUIRE(printed.find("memory_barrier kind=isb option=sy") != std::string::npos);

    runtime::SharedRuntimeState shared(memory);
    REQUIRE(runtime::synchronized_store(shared, 0x1000U, 8U, 0x55U, ir::MemoryOrder::Relaxed));
    runtime::CpuState cpu{};
    cpu.x[1] = 0x1000U;
    cpu.x[5] = 0x123456789abcdef0U;
    runtime::RuntimeContext context{&memory};
    context.shared = &shared;
    context.cpu = &cpu;
    const auto executed = interpreter::execute(lifted.value(), cpu, context);
    if (!executed) UNSCOPED_INFO(executed.error().message);
    REQUIRE(executed);
    REQUIRE(cpu.x[4] == 0x123456789abcdef0U);
    REQUIRE(cpu.x[0] == 0x55U);
    REQUIRE(cpu.x[2] == 0U);
}

TEST_CASE("Milestone 9 guest thread lifecycle uses stable project-owned IDs")
{
    auto memory = make_memory();
    runtime::SharedRuntimeState shared(memory);
    runtime::ThreadManager manager(shared);
    std::atomic<unsigned int> ran{0U};
    const auto first = manager.create([&](runtime::GuestThread&) {
        ++ran; return Result<void>::success();
    });
    const auto second = manager.create([&](runtime::GuestThread&) {
        ++ran; return Result<void>::success();
    });
    REQUIRE(first); REQUIRE(second);
    REQUIRE(first.value()->id() != second.value()->id());
    REQUIRE(first.value()->state() == runtime::GuestThreadState::Created);
    REQUIRE(first.value()->start());
    REQUIRE(second.value()->start());
    REQUIRE(first.value()->join());
    REQUIRE(second.value()->join());
    REQUIRE(ran.load() == 2U);
    REQUIRE(first.value()->state() == runtime::GuestThreadState::Joined);
    REQUIRE_FALSE(first.value()->start());
    REQUIRE_FALSE(first.value()->join());
}

TEST_CASE("Milestone 9 CpuState and TLS are isolated per guest thread")
{
    auto memory = make_memory();
    runtime::SharedRuntimeState shared(memory);
    runtime::ThreadManager manager(shared);
    const auto a = manager.create([](runtime::GuestThread& thread) {
        thread.cpu().x[0] = 0xaaaaU;
        thread.cpu().sp = 0x1800U;
        thread.cpu().tpidr_el0 = 0x2000U;
        return Result<void>::success();
    });
    const auto b = manager.create([](runtime::GuestThread& thread) {
        thread.cpu().x[0] = 0xbbbbU;
        thread.cpu().sp = 0x2800U;
        thread.cpu().tpidr_el0 = 0x3000U;
        return Result<void>::success();
    });
    REQUIRE(a); REQUIRE(b); REQUIRE(a.value()->start()); REQUIRE(b.value()->start());
    REQUIRE(a.value()->join()); REQUIRE(b.value()->join());
    REQUIRE(a.value()->cpu().x[0] == 0xaaaaU);
    REQUIRE(b.value()->cpu().x[0] == 0xbbbbU);
    REQUIRE(a.value()->cpu().tpidr_el0 == 0x2000U);
    REQUIRE(b.value()->cpu().tpidr_el0 == 0x3000U);
}

TEST_CASE("Milestone 9 exclusive success failure CLREX and monitor replacement")
{
    auto memory = make_memory();
    runtime::SharedRuntimeState shared(memory);
    runtime::ExclusiveReservation a{};
    runtime::ExclusiveReservation b{};
    REQUIRE(runtime::synchronized_store(shared, 0x1000U, 8U, 10U, ir::MemoryOrder::Relaxed));

    const auto loaded = runtime::exclusive_load(shared, a, 0x1000U, 8U, ir::MemoryOrder::Relaxed);
    REQUIRE(loaded); REQUIRE(loaded.value() == 10U);
    const auto success = runtime::exclusive_store(shared, a, 0x1000U, 8U, 11U,
                                                   ir::MemoryOrder::Relaxed);
    REQUIRE(success); REQUIRE(success.value() == 0U); REQUIRE_FALSE(a.valid);

    REQUIRE(runtime::exclusive_load(shared, a, 0x1000U, 8U, ir::MemoryOrder::Relaxed));
    REQUIRE(runtime::synchronized_store(shared, 0x1000U, 8U, 20U, ir::MemoryOrder::Relaxed));
    const auto conflict = runtime::exclusive_store(shared, a, 0x1000U, 8U, 21U,
                                                    ir::MemoryOrder::Relaxed);
    REQUIRE(conflict); REQUIRE(conflict.value() == 1U);
    const auto current = runtime::synchronized_load(shared, 0x1000U, 8U, ir::MemoryOrder::Relaxed);
    REQUIRE(current); REQUIRE(current.value() == 20U);

    REQUIRE(runtime::exclusive_load(shared, a, 0x1000U, 8U, ir::MemoryOrder::Relaxed));
    a.clear();
    const auto cleared = runtime::exclusive_store(shared, a, 0x1000U, 8U, 30U,
                                                   ir::MemoryOrder::Relaxed);
    REQUIRE(cleared); REQUIRE(cleared.value() == 1U);

    REQUIRE(runtime::exclusive_load(shared, a, 0x1000U, 8U, ir::MemoryOrder::Relaxed));
    REQUIRE(runtime::exclusive_load(shared, a, 0x1080U, 8U, ir::MemoryOrder::Relaxed));
    const auto replaced = runtime::exclusive_store(shared, a, 0x1000U, 8U, 31U,
                                                    ir::MemoryOrder::Relaxed);
    REQUIRE(replaced); REQUIRE(replaced.value() == 1U);

    REQUIRE(runtime::exclusive_load(shared, a, 0x1000U, 8U, ir::MemoryOrder::Relaxed));
    REQUIRE(runtime::exclusive_load(shared, b, 0x1080U, 8U, ir::MemoryOrder::Relaxed));
    REQUIRE(a.valid); REQUIRE(b.valid);
}

TEST_CASE("Milestone 9 exclusive increment survives native contention")
{
    auto memory = make_memory();
    runtime::SharedRuntimeState shared(memory);
    REQUIRE(runtime::synchronized_store(shared, 0x1100U, 8U, 0U, ir::MemoryOrder::Relaxed));
    runtime::ThreadManager manager(shared);
    constexpr std::uint64_t threads = 4U;
    constexpr std::uint64_t increments = 250U;
    for (std::uint64_t t = 0; t < threads; ++t)
    {
        const auto thread = manager.create([&](runtime::GuestThread& guest) {
            for (std::uint64_t i = 0; i < increments; ++i)
            {
                bool done = false;
                for (std::uint64_t retry = 0; retry < 100000U && !done; ++retry)
                {
                    const auto old = runtime::exclusive_load(shared, guest.runtime().exclusive,
                        0x1100U, 8U, ir::MemoryOrder::Relaxed);
                    if (!old) return Result<void>::failure(old.error());
                    const auto status = runtime::exclusive_store(shared, guest.runtime().exclusive,
                        0x1100U, 8U, old.value() + 1U, ir::MemoryOrder::Relaxed);
                    if (!status) return Result<void>::failure(status.error());
                    done = status.value() == 0U;
                }
                if (!done) return Result<void>::failure(make_error(ErrorCode::ExecutionLimitExceeded,
                    "bounded exclusive increment retry limit exceeded"));
            }
            return Result<void>::success();
        });
        REQUIRE(thread); REQUIRE(thread.value()->start());
    }
    REQUIRE(manager.join_all());
    const auto result = runtime::synchronized_load(shared, 0x1100U, 8U, ir::MemoryOrder::Relaxed);
    REQUIRE(result); REQUIRE(result.value() == threads * increments);
}

TEST_CASE("Milestone 9 acquire release message passing and faults are deterministic")
{
    auto memory = make_memory();
    runtime::SharedRuntimeState shared(memory);
    REQUIRE(runtime::synchronized_store(shared, 0x1200U, 8U, 0U, ir::MemoryOrder::Relaxed));
    REQUIRE(runtime::synchronized_store(shared, 0x1280U, 8U, 0U, ir::MemoryOrder::Relaxed));
    runtime::ThreadManager manager(shared);
    std::uint64_t observed = 0U;
    const auto reader = manager.create([&](runtime::GuestThread&) {
        for (std::uint64_t retry = 0; retry < 100000U; ++retry)
        {
            const auto ready = runtime::synchronized_load(shared, 0x1280U, 8U, ir::MemoryOrder::Acquire);
            if (!ready) return Result<void>::failure(ready.error());
            if (ready.value() != 0U)
            {
                const auto data = runtime::synchronized_load(shared, 0x1200U, 8U, ir::MemoryOrder::Relaxed);
                if (!data) return Result<void>::failure(data.error());
                observed = data.value(); return Result<void>::success();
            }
            std::this_thread::yield();
        }
        return Result<void>::failure(make_error(ErrorCode::ExecutionLimitExceeded,
            "bounded message-passing wait exceeded"));
    });
    const auto writer = manager.create([&](runtime::GuestThread&) {
        const auto data = runtime::synchronized_store(shared, 0x1200U, 8U, 0xfeedbeefU,
                                                       ir::MemoryOrder::Relaxed);
        if (!data) return data;
        return runtime::synchronized_store(shared, 0x1280U, 8U, 1U, ir::MemoryOrder::Release);
    });
    REQUIRE(reader); REQUIRE(writer); REQUIRE(reader.value()->start()); REQUIRE(writer.value()->start());
    REQUIRE(reader.value()->join()); REQUIRE(writer.value()->join());
    REQUIRE(observed == 0xfeedbeefU);

    runtime::ExclusiveReservation reservation{};
    const auto misaligned = runtime::exclusive_load(shared, reservation, 0x1001U, 8U,
                                                     ir::MemoryOrder::Relaxed);
    REQUIRE_FALSE(misaligned); REQUIRE(misaligned.error().code == ErrorCode::MisalignedAtomicAccess);
    const auto unmapped = runtime::synchronized_load(shared, 0xf000U, 8U, ir::MemoryOrder::Acquire);
    REQUIRE_FALSE(unmapped); REQUIRE(unmapped.error().code == ErrorCode::UnmappedMemory);
    const auto readonly = runtime::synchronized_store(shared, 0x8000U, 8U, 1U, ir::MemoryOrder::Release);
    REQUIRE_FALSE(readonly); REQUIRE(readonly.error().code == ErrorCode::PermissionDenied);
    const auto crossing = runtime::synchronized_load(shared, 0x4ff8U, 8U, ir::MemoryOrder::Relaxed);
    REQUIRE_FALSE(crossing); REQUIRE(crossing.error().code == ErrorCode::UnmappedMemory);
    const auto invalid_width = runtime::synchronized_load(shared, 0x1000U, 3U, ir::MemoryOrder::Relaxed);
    REQUIRE_FALSE(invalid_width); REQUIRE(invalid_width.error().code == ErrorCode::InvalidAtomicWidth);
}

TEST_CASE("Milestone 9 TPIDR runtime ABI and barriers are explicit")
{
    auto memory = make_memory();
    runtime::SharedRuntimeState shared(memory);
    runtime::CpuState cpu{};
    runtime::RuntimeContext context{&memory};
    context.shared = &shared; context.cpu = &cpu; context.guest_thread_id = 42U;
    REQUIRE(runtime::switchrecomp_runtime_write_system_register(&context,
        static_cast<std::uint8_t>(ir::SystemRegister::TpidrEl0), 0x12345678U) == 0U);
    std::uint64_t value = 0U;
    REQUIRE(runtime::switchrecomp_runtime_read_system_register(&context,
        static_cast<std::uint8_t>(ir::SystemRegister::TpidrEl0), &value) == 0U);
    REQUIRE(value == 0x12345678U);
    REQUIRE(runtime::switchrecomp_runtime_write_system_register(&context,
        static_cast<std::uint8_t>(ir::SystemRegister::TpidrroEl0), 1U) != 0U);
    context.clear_error();
    REQUIRE(runtime::switchrecomp_runtime_memory_barrier(&context,
        static_cast<std::uint8_t>(ir::BarrierKind::Dmb),
        static_cast<std::uint8_t>(ir::BarrierOption::Sy)) == 0U);
}
