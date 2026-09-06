#pragma once

#include "switchrecomp/common/error.hpp"
#include "switchrecomp/memory/guest_memory.hpp"
#include "switchrecomp/runtime/fp.hpp"

#include <cstdint>

namespace switchrecomp::runtime
{

struct RuntimeContext
{
    memory::GuestMemory* memory = nullptr;
    bool has_error = false;
    Error last_error{ErrorCode::InvalidRuntimeContext, "runtime context has no error"};

    void clear_error() noexcept
    {
        has_error = false;
        last_error = Error{ErrorCode::InvalidRuntimeContext, "runtime context has no error"};
    }

    void set_error(Error error) noexcept;
};

// These C-linkage helpers are the only guest-memory boundary used by generated code.
// A non-zero return value means that RuntimeContext::last_error contains the diagnostic.
extern "C"
{
std::uint32_t switchrecomp_runtime_guest_load(RuntimeContext* runtime, std::uint64_t address,
                                              std::uint8_t size, std::uint64_t* result) noexcept;
std::uint32_t switchrecomp_runtime_guest_store(RuntimeContext* runtime, std::uint64_t address,
                                               std::uint8_t size, std::uint64_t value) noexcept;
std::uint32_t switchrecomp_runtime_guest_load_vector(RuntimeContext* runtime, std::uint64_t address,
                                                     Vector128* result) noexcept;
std::uint32_t switchrecomp_runtime_guest_store_vector(RuntimeContext* runtime, std::uint64_t address,
                                                      const Vector128* value) noexcept;
std::uint32_t switchrecomp_runtime_guest_address_add(RuntimeContext* runtime,
                                                     std::uint64_t base, std::int64_t offset,
                                                     std::uint64_t* result) noexcept;
std::uint32_t switchrecomp_runtime_guest_address_add_value(RuntimeContext* runtime,
                                                           std::uint64_t base,
                                                           std::uint64_t offset,
                                                           std::uint8_t signed_offset,
                                                           std::uint64_t* result) noexcept;
std::uint32_t switchrecomp_runtime_trap(RuntimeContext* runtime, const char* reason) noexcept;
}

} // namespace switchrecomp::runtime
