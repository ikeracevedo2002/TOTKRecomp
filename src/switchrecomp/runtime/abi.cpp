#include "switchrecomp/runtime/abi.hpp"

#include <array>
#include <utility>

namespace
{

using switchrecomp::Error;
using switchrecomp::ErrorCode;
using switchrecomp::make_error;
using switchrecomp::runtime::RuntimeExecutionContext;

void fail(RuntimeExecutionContext* context, Error error)
{
    if (context != nullptr)
    {
        context->last_error = std::move(error);
        context->failed = true;
    }
}

[[nodiscard]] RuntimeExecutionContext* require_context(RuntimeExecutionContext* context)
{
    if (context == nullptr)
    {
        return nullptr;
    }
    if (context->cpu_state == nullptr)
    {
        fail(context, make_error(ErrorCode::InvalidArgument, "runtime context has no CpuState"));
        return nullptr;
    }
    return context;
}

[[nodiscard]] switchrecomp::aarch64::Register make_register(
    std::uint8_t index, std::uint8_t width, std::uint8_t is_stack_pointer,
    std::uint8_t is_zero)
{
    const auto register_width = width == 32U ? switchrecomp::aarch64::RegisterWidth::W32
                                             : switchrecomp::aarch64::RegisterWidth::X64;
    return switchrecomp::aarch64::Register{switchrecomp::aarch64::RegisterKind::General,
                                           register_width, index, is_stack_pointer != 0U,
                                           is_zero != 0U};
}

template <typename T>
int read_integer(RuntimeExecutionContext* context, std::uint64_t address, T* value)
{
    if (context == nullptr || context->guest_memory == nullptr || value == nullptr)
    {
        fail(context, make_error(ErrorCode::InvalidArgument, "invalid guest memory helper argument"));
        return switchrecomp::runtime::execution_failure;
    }
    std::array<std::byte, sizeof(T)> bytes{};
    const auto read = context->guest_memory->read(address, bytes);
    if (!read)
    {
        fail(context, make_error(ErrorCode::ExecutionMemoryFault,
                                 "guest memory read failed: " + read.error().message));
        return switchrecomp::runtime::execution_failure;
    }
    T result = 0U;
    for (std::size_t index = 0U; index < sizeof(T); ++index)
    {
        result |= static_cast<T>(std::to_integer<std::uint8_t>(bytes[index])) << (index * 8U);
    }
    *value = result;
    return switchrecomp::runtime::execution_success;
}

template <typename T>
int write_integer(RuntimeExecutionContext* context, std::uint64_t address, T value)
{
    if (context == nullptr || context->guest_memory == nullptr)
    {
        fail(context, make_error(ErrorCode::InvalidArgument, "invalid guest memory helper argument"));
        return switchrecomp::runtime::execution_failure;
    }
    std::array<std::byte, sizeof(T)> bytes{};
    for (std::size_t index = 0U; index < sizeof(T); ++index)
    {
        bytes[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
    const auto written = context->guest_memory->write(address, bytes);
    if (!written)
    {
        fail(context, make_error(ErrorCode::ExecutionMemoryFault,
                                 "guest memory write failed: " + written.error().message));
        return switchrecomp::runtime::execution_failure;
    }
    return switchrecomp::runtime::execution_success;
}

} // namespace

extern "C" std::uint64_t switchrecomp_read_register(RuntimeExecutionContext* context,
                                                      std::uint8_t index, std::uint8_t width,
                                                      std::uint8_t is_stack_pointer,
                                                      std::uint8_t is_zero)
{
    if (require_context(context) == nullptr)
    {
        return 0U;
    }
    const auto value = switchrecomp::runtime::read_register(
        *context->cpu_state, make_register(index, width, is_stack_pointer, is_zero));
    if (!value)
    {
        fail(context, value.error());
        return 0U;
    }
    return value.value();
}

extern "C" int switchrecomp_write_register(RuntimeExecutionContext* context, std::uint8_t index,
                                             std::uint8_t width, std::uint8_t is_stack_pointer,
                                             std::uint8_t is_zero, std::uint64_t value)
{
    if (require_context(context) == nullptr)
    {
        return switchrecomp::runtime::execution_failure;
    }
    const auto written = switchrecomp::runtime::write_register(
        *context->cpu_state, make_register(index, width, is_stack_pointer, is_zero), value);
    if (!written)
    {
        fail(context, written.error());
        return switchrecomp::runtime::execution_failure;
    }
    return switchrecomp::runtime::execution_success;
}

extern "C" int switchrecomp_guest_read_u32(RuntimeExecutionContext* context,
                                             std::uint64_t address, std::uint32_t* value)
{
    return read_integer(context, address, value);
}

extern "C" int switchrecomp_guest_read_u64(RuntimeExecutionContext* context,
                                             std::uint64_t address, std::uint64_t* value)
{
    return read_integer(context, address, value);
}

extern "C" int switchrecomp_guest_write_u32(RuntimeExecutionContext* context,
                                              std::uint64_t address, std::uint32_t value)
{
    return write_integer(context, address, value);
}

extern "C" int switchrecomp_guest_write_u64(RuntimeExecutionContext* context,
                                              std::uint64_t address, std::uint64_t value)
{
    return write_integer(context, address, value);
}

extern "C" int switchrecomp_set_guest_pc(RuntimeExecutionContext* context, std::uint64_t pc)
{
    if (require_context(context) == nullptr)
    {
        return switchrecomp::runtime::execution_failure;
    }
    context->cpu_state->pc = pc;
    return switchrecomp::runtime::execution_success;
}
