#include "switchrecomp/memory/guest_memory.hpp"
#include "switchrecomp/runtime/atomic_memory.hpp"
#include "switchrecomp/runtime/context.hpp"
#include "switchrecomp/runtime/thread.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace
{
using namespace switchrecomp;

memory::GuestMemory make_memory()
{
    memory::GuestMemory memory;
    REQUIRE(memory.map(0x1000U, 0x4000U,
        memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Write,
        "m9-shared"));
    REQUIRE(memory.map(0x8000U, 0x1000U, memory::GuestMemoryPermissions::Read,
        "m9-ro"));
    return memory;
}
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
