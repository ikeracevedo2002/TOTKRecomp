#pragma once

#include "switchrecomp/common/error.hpp"
#include "switchrecomp/memory/guest_memory.hpp"
#include "switchrecomp/runtime/atomic_memory.hpp"
#include "switchrecomp/runtime/cpu_state.hpp"
#include "switchrecomp/runtime/fp.hpp"

#include <cstdint>

namespace switchrecomp::runtime
{

struct RuntimeContext
{
    // Kept first for source compatibility with existing RuntimeContext{&memory} callers.
    memory::GuestMemory* memory = nullptr;
    SharedRuntimeState* shared = nullptr;
    CpuState* cpu = nullptr;
    std::uint64_t guest_thread_id = 0U;
    ExclusiveReservation exclusive{};
    bool has_error = false;
    Error last_error{ErrorCode::InvalidRuntimeContext, "runtime context has no error"};

    void clear_error() noexcept
    {
        has_error = false;
        last_error = Error{ErrorCode::InvalidRuntimeContext, "runtime context has no error"};
    }
    void set_error(Error error) noexcept;
};

extern "C"
{
std::uint32_t switchrecomp_runtime_guest_load(RuntimeContext*, std::uint64_t, std::uint8_t,
                                              std::uint64_t*) noexcept;
std::uint32_t switchrecomp_runtime_guest_store(RuntimeContext*, std::uint64_t, std::uint8_t,
                                               std::uint64_t) noexcept;
std::uint32_t switchrecomp_runtime_guest_load_vector(RuntimeContext*, std::uint64_t,
                                                     Vector128*) noexcept;
std::uint32_t switchrecomp_runtime_guest_store_vector(RuntimeContext*, std::uint64_t,
                                                      const Vector128*) noexcept;
std::uint32_t switchrecomp_runtime_atomic_load(RuntimeContext*, std::uint64_t, std::uint8_t,
                                               std::uint8_t, std::uint64_t*) noexcept;
std::uint32_t switchrecomp_runtime_atomic_store(RuntimeContext*, std::uint64_t, std::uint8_t,
                                                std::uint64_t, std::uint8_t) noexcept;
std::uint32_t switchrecomp_runtime_exclusive_load(RuntimeContext*, std::uint64_t, std::uint8_t,
                                                  std::uint8_t, std::uint64_t*) noexcept;
std::uint32_t switchrecomp_runtime_exclusive_store(RuntimeContext*, std::uint64_t, std::uint8_t,
                                                   std::uint64_t, std::uint8_t,
                                                   std::uint32_t*) noexcept;
std::uint32_t switchrecomp_runtime_clear_exclusive(RuntimeContext*) noexcept;
std::uint32_t switchrecomp_runtime_memory_barrier(RuntimeContext*, std::uint8_t,
                                                  std::uint8_t) noexcept;
std::uint32_t switchrecomp_runtime_read_system_register(RuntimeContext*, std::uint8_t,
                                                        std::uint64_t*) noexcept;
std::uint32_t switchrecomp_runtime_write_system_register(RuntimeContext*, std::uint8_t,
                                                         std::uint64_t) noexcept;
std::uint32_t switchrecomp_runtime_guest_address_add(RuntimeContext*, std::uint64_t, std::int64_t,
                                                     std::uint64_t*) noexcept;
std::uint32_t switchrecomp_runtime_guest_address_add_value(RuntimeContext*, std::uint64_t,
                                                           std::uint64_t, std::uint8_t,
                                                           std::uint64_t*) noexcept;
std::uint32_t switchrecomp_runtime_trap(RuntimeContext*, const char*) noexcept;
}

} // namespace switchrecomp::runtime
