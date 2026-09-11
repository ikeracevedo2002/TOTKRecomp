#include "switchrecomp/runtime/atomic_memory.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <span>

namespace switchrecomp::runtime
{
namespace
{
[[nodiscard]] Result<void> validate_width(std::uint8_t size)
{
    if (size != 1U && size != 2U && size != 4U && size != 8U)
        return Result<void>::failure(make_error(ErrorCode::InvalidAtomicWidth,
            "atomic guest access width must be 1, 2, 4, or 8 bytes"));
    return Result<void>::success();
}
[[nodiscard]] Result<void> validate_alignment(std::uint64_t address, std::uint8_t size)
{
    if ((address & (static_cast<std::uint64_t>(size) - 1U)) != 0U)
        return Result<void>::failure(make_error(ErrorCode::MisalignedAtomicAccess,
            "atomic guest access is not naturally aligned"));
    return Result<void>::success();
}
[[nodiscard]] bool valid_load_order(ir::MemoryOrder order) noexcept
{
    return order == ir::MemoryOrder::Relaxed || order == ir::MemoryOrder::Acquire ||
           order == ir::MemoryOrder::SequentiallyConsistent;
}
[[nodiscard]] bool valid_store_order(ir::MemoryOrder order) noexcept
{
    return order == ir::MemoryOrder::Relaxed || order == ir::MemoryOrder::Release ||
           order == ir::MemoryOrder::SequentiallyConsistent;
}
[[nodiscard]] std::uint64_t load_le(std::span<const std::byte> bytes) noexcept
{
    std::uint64_t value = 0U;
    for (std::size_t i = 0; i < bytes.size(); ++i)
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[i])) << (i * 8U);
    return value;
}
void store_le(std::span<std::byte> bytes, std::uint64_t value) noexcept
{
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
}
[[nodiscard]] std::uint64_t granule_for(std::uint64_t address) noexcept
{
    return address & ~(exclusive_reservation_granule_size - 1U);
}
[[nodiscard]] std::uint64_t generation_for(const SharedRuntimeState& shared,
                                            std::uint64_t granule) noexcept
{
    const auto it = shared.reservation_generations.find(granule);
    return it == shared.reservation_generations.end() ? 0U : it->second;
}
void invalidate_written_range(SharedRuntimeState& shared, std::uint64_t address,
                              std::uint64_t size)
{
    if (size == 0U) return;
    const auto first = granule_for(address);
    const auto last = granule_for(address + size - 1U);
    for (auto granule = first;; granule += exclusive_reservation_granule_size)
    {
        ++shared.reservation_generations[granule];
        if (granule == last) break;
    }
}
[[nodiscard]] Result<void> validate_common(SharedRuntimeState& shared, std::uint64_t address,
                                           std::uint8_t size)
{
    if (shared.memory == nullptr)
        return Result<void>::failure(make_error(ErrorCode::InvalidRuntimeContext,
            "shared runtime state has no guest memory"));
    const auto width = validate_width(size); if (!width) return width;
    const auto alignment = validate_alignment(address, size); if (!alignment) return alignment;
    if (address > std::numeric_limits<std::uint64_t>::max() - (static_cast<std::uint64_t>(size) - 1U))
        return Result<void>::failure(make_error(ErrorCode::ArithmeticOverflow,
            "atomic guest address range overflows"));
    return Result<void>::success();
}
[[nodiscard]] Result<std::uint64_t> load_locked(SharedRuntimeState& shared,
                                                std::uint64_t address,
                                                std::uint8_t size)
{
    std::array<std::byte, 8> bytes{};
    const auto read = shared.memory->read(address, std::span<std::byte>(bytes).first(size));
    if (!read) return Result<std::uint64_t>::failure(read.error());
    return Result<std::uint64_t>::success(load_le(std::span<const std::byte>(bytes).first(size)));
}
[[nodiscard]] Result<void> validate_store_target_locked(SharedRuntimeState& shared,
                                                        std::uint64_t address,
                                                        std::uint8_t size)
{
    const auto permissions = shared.memory->permissions_at(address, size);
    if (!permissions) return Result<void>::failure(permissions.error());
    if (!memory::has_permission(permissions.value(), memory::GuestMemoryPermissions::Write))
        return Result<void>::failure(make_error(ErrorCode::PermissionDenied,
            "atomic guest store target does not grant write permission"));
    return Result<void>::success();
}
[[nodiscard]] Result<void> store_locked(SharedRuntimeState& shared, std::uint64_t address,
                                        std::uint8_t size, std::uint64_t value)
{
    std::array<std::byte, 8> bytes{};
    store_le(std::span<std::byte>(bytes).first(size), value);
    const auto write = shared.memory->write(address, std::span<const std::byte>(bytes).first(size));
    if (!write) return write;
    invalidate_written_range(shared, address, size);
    return Result<void>::success();
}
void acquire_fence(ir::MemoryOrder order) noexcept
{
    if (order == ir::MemoryOrder::Acquire)
        std::atomic_thread_fence(std::memory_order_acquire);
    else if (order == ir::MemoryOrder::SequentiallyConsistent)
        std::atomic_thread_fence(std::memory_order_seq_cst);
}
void release_fence(ir::MemoryOrder order) noexcept
{
    if (order == ir::MemoryOrder::Release)
        std::atomic_thread_fence(std::memory_order_release);
    else if (order == ir::MemoryOrder::SequentiallyConsistent)
        std::atomic_thread_fence(std::memory_order_seq_cst);
}
} // namespace

Result<void> synchronized_read_bytes(SharedRuntimeState& shared, std::uint64_t address,
                                     std::span<std::byte> destination)
{
    if (shared.memory == nullptr)
        return Result<void>::failure(make_error(ErrorCode::InvalidRuntimeContext,
            "shared runtime state has no guest memory"));
    std::lock_guard lock(shared.memory_mutex);
    return shared.memory->read(address, destination);
}

Result<void> synchronized_write_bytes(SharedRuntimeState& shared, std::uint64_t address,
                                      std::span<const std::byte> source)
{
    if (shared.memory == nullptr)
        return Result<void>::failure(make_error(ErrorCode::InvalidRuntimeContext,
            "shared runtime state has no guest memory"));
    std::lock_guard lock(shared.memory_mutex);
    const auto write = shared.memory->write(address, source);
    if (!write) return write;
    invalidate_written_range(shared, address, static_cast<std::uint64_t>(source.size()));
    return Result<void>::success();
}

Result<std::uint64_t> synchronized_load(SharedRuntimeState& shared, std::uint64_t address,
                                        std::uint8_t size, ir::MemoryOrder order)
{
    const auto common = validate_common(shared, address, size);
    if (!common) return Result<std::uint64_t>::failure(common.error());
    if (!valid_load_order(order))
        return Result<std::uint64_t>::failure(make_error(ErrorCode::InvalidMemoryOrder,
            "load memory order must be relaxed, acquire, or sequentially consistent"));
    std::lock_guard lock(shared.memory_mutex);
    const auto value = load_locked(shared, address, size);
    if (!value) return value;
    acquire_fence(order);
    return value;
}

Result<void> synchronized_store(SharedRuntimeState& shared, std::uint64_t address,
                                std::uint8_t size, std::uint64_t value, ir::MemoryOrder order)
{
    const auto common = validate_common(shared, address, size); if (!common) return common;
    if (!valid_store_order(order))
        return Result<void>::failure(make_error(ErrorCode::InvalidMemoryOrder,
            "store memory order must be relaxed, release, or sequentially consistent"));
    std::lock_guard lock(shared.memory_mutex);
    release_fence(order);
    return store_locked(shared, address, size, value);
}

Result<std::uint64_t> exclusive_load(SharedRuntimeState& shared,
                                     ExclusiveReservation& reservation,
                                     std::uint64_t address, std::uint8_t size,
                                     ir::MemoryOrder order)
{
    reservation.clear();
    const auto common = validate_common(shared, address, size);
    if (!common) return Result<std::uint64_t>::failure(common.error());
    if (!valid_load_order(order))
        return Result<std::uint64_t>::failure(make_error(ErrorCode::InvalidMemoryOrder,
            "exclusive load has an invalid memory order"));
    std::lock_guard lock(shared.memory_mutex);
    const auto value = load_locked(shared, address, size); if (!value) return value;
    acquire_fence(order);
    reservation = {true, address, granule_for(address), generation_for(shared, granule_for(address)), size};
    return value;
}

Result<std::uint32_t> exclusive_store(SharedRuntimeState& shared,
                                      ExclusiveReservation& reservation,
                                      std::uint64_t address, std::uint8_t size,
                                      std::uint64_t value, ir::MemoryOrder order)
{
    const auto common = validate_common(shared, address, size);
    if (!common) { reservation.clear(); return Result<std::uint32_t>::failure(common.error()); }
    if (!valid_store_order(order))
    {
        reservation.clear();
        return Result<std::uint32_t>::failure(make_error(ErrorCode::InvalidMemoryOrder,
            "exclusive store has an invalid memory order"));
    }
    std::lock_guard lock(shared.memory_mutex);
    const auto target = validate_store_target_locked(shared, address, size);
    if (!target) { reservation.clear(); return Result<std::uint32_t>::failure(target.error()); }
    const auto granule = granule_for(address);
    const bool succeeds = reservation.valid && reservation.address == address &&
                          reservation.size == size && reservation.granule == granule &&
                          reservation.generation == generation_for(shared, granule);
    reservation.clear();
    if (!succeeds) return Result<std::uint32_t>::success(1U);
    release_fence(order);
    const auto stored = store_locked(shared, address, size, value);
    if (!stored) return Result<std::uint32_t>::failure(stored.error());
    return Result<std::uint32_t>::success(0U);
}

Result<void> memory_barrier(ir::BarrierKind kind, ir::BarrierOption option)
{
    switch (option)
    {
    case ir::BarrierOption::Sy: case ir::BarrierOption::St: case ir::BarrierOption::Ld:
    case ir::BarrierOption::Ish: case ir::BarrierOption::Ishst: case ir::BarrierOption::Ishld:
    case ir::BarrierOption::Nsh: case ir::BarrierOption::Nshst: case ir::BarrierOption::Nshld:
    case ir::BarrierOption::Osh: case ir::BarrierOption::Oshst: case ir::BarrierOption::Oshld: break;
    default: return Result<void>::failure(make_error(ErrorCode::InvalidBarrier,
        "invalid AArch64 barrier option"));
    }
    switch (kind)
    {
    case ir::BarrierKind::Dmb: case ir::BarrierKind::Dsb: case ir::BarrierKind::Isb:
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return Result<void>::success();
    }
    return Result<void>::failure(make_error(ErrorCode::InvalidBarrier,
        "invalid AArch64 barrier kind"));
}

} // namespace switchrecomp::runtime
