#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/ir/opcode.hpp"
#include "switchrecomp/memory/guest_memory.hpp"

#include <cstdint>
#include <mutex>
#include <span>
#include <unordered_map>

namespace switchrecomp::runtime
{

inline constexpr std::uint64_t exclusive_reservation_granule_size = 64U;

struct ExclusiveReservation
{
    bool valid = false;
    std::uint64_t address = 0U;
    std::uint64_t granule = 0U;
    std::uint64_t generation = 0U;
    std::uint8_t size = 0U;

    void clear() noexcept { *this = {}; }
};

struct SharedRuntimeState
{
    explicit SharedRuntimeState(memory::GuestMemory& guest_memory) : memory(&guest_memory) {}

    memory::GuestMemory* memory = nullptr;
    mutable std::mutex memory_mutex;
    std::unordered_map<std::uint64_t, std::uint64_t> reservation_generations;
};

[[nodiscard]] Result<void> synchronized_read_bytes(SharedRuntimeState& shared,
                                                   std::uint64_t address,
                                                   std::span<std::byte> destination);
[[nodiscard]] Result<void> synchronized_write_bytes(SharedRuntimeState& shared,
                                                    std::uint64_t address,
                                                    std::span<const std::byte> source);
[[nodiscard]] Result<std::uint64_t> synchronized_load(SharedRuntimeState& shared,
                                                      std::uint64_t address,
                                                      std::uint8_t size,
                                                      ir::MemoryOrder order);
[[nodiscard]] Result<void> synchronized_store(SharedRuntimeState& shared,
                                              std::uint64_t address,
                                              std::uint8_t size,
                                              std::uint64_t value,
                                              ir::MemoryOrder order);
[[nodiscard]] Result<std::uint64_t> exclusive_load(SharedRuntimeState& shared,
                                                   ExclusiveReservation& reservation,
                                                   std::uint64_t address,
                                                   std::uint8_t size,
                                                   ir::MemoryOrder order);
[[nodiscard]] Result<std::uint32_t> exclusive_store(SharedRuntimeState& shared,
                                                    ExclusiveReservation& reservation,
                                                    std::uint64_t address,
                                                    std::uint8_t size,
                                                    std::uint64_t value,
                                                    ir::MemoryOrder order);
[[nodiscard]] Result<void> memory_barrier(ir::BarrierKind kind,
                                          ir::BarrierOption option);

} // namespace switchrecomp::runtime
