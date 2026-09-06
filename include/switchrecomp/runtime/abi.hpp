#pragma once

#include "switchrecomp/common/error.hpp"
#include "switchrecomp/memory/guest_memory.hpp"
#include "switchrecomp/runtime/cpu_state.hpp"

#include <cstdint>

namespace switchrecomp::runtime
{

// This is an opaque-to-LLVM carrier. Generated code accesses it only through the
// C ABI functions below; it never relies on the C++ layout of GuestMemory.
struct RuntimeExecutionContext
{
    CpuState* cpu_state = nullptr;
    memory::GuestMemory* guest_memory = nullptr;
    Error last_error{ErrorCode::InvalidArgument, "no runtime error"};
    bool failed = false;
};

inline constexpr int execution_success = 0;
inline constexpr int execution_failure = 1;

} // namespace switchrecomp::runtime

extern "C"
{

std::uint64_t switchrecomp_read_register(switchrecomp::runtime::RuntimeExecutionContext* context,
                                         std::uint8_t index, std::uint8_t width,
                                         std::uint8_t is_stack_pointer, std::uint8_t is_zero);
int switchrecomp_write_register(switchrecomp::runtime::RuntimeExecutionContext* context,
                                std::uint8_t index, std::uint8_t width,
                                std::uint8_t is_stack_pointer, std::uint8_t is_zero,
                                std::uint64_t value);
int switchrecomp_guest_read_u32(switchrecomp::runtime::RuntimeExecutionContext* context,
                                std::uint64_t address, std::uint32_t* value);
int switchrecomp_guest_read_u64(switchrecomp::runtime::RuntimeExecutionContext* context,
                                std::uint64_t address, std::uint64_t* value);
int switchrecomp_guest_write_u32(switchrecomp::runtime::RuntimeExecutionContext* context,
                                 std::uint64_t address, std::uint32_t value);
int switchrecomp_guest_write_u64(switchrecomp::runtime::RuntimeExecutionContext* context,
                                 std::uint64_t address, std::uint64_t value);
int switchrecomp_set_guest_pc(switchrecomp::runtime::RuntimeExecutionContext* context,
                              std::uint64_t pc);

}
