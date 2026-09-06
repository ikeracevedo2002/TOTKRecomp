#include "switchrecomp/runtime/context.hpp"
#include "switchrecomp/runtime/execution.hpp"

#include "switchrecomp/common/checked_arithmetic.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <span>
#include <utility>

namespace switchrecomp::runtime
{

void RuntimeContext::set_error(Error error) noexcept
{
    try
    {
        last_error = std::move(error);
        has_error = true;
    }
    catch (const std::bad_alloc&)
    {
        last_error = Error{ErrorCode::ResourceLimit, "runtime error diagnostic allocation failed"};
        has_error = true;
    }
}

namespace
{

[[nodiscard]] std::uint32_t failure(RuntimeContext* runtime, Error error) noexcept
{
    if (runtime == nullptr)
    {
        return 1U;
    }
    runtime->set_error(std::move(error));
    return 1U;
}

[[nodiscard]] Result<void> validate_size(std::uint8_t size)
{
    if (size != 1U && size != 2U && size != 4U && size != 8U)
    {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidArgument, "guest access size must be 1, 2, 4, or 8"));
    }
    return Result<void>::success();
}

[[nodiscard]] std::uint64_t load_little_endian(std::span<const std::byte> bytes) noexcept
{
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < bytes.size(); ++index)
    {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[index]))
                 << (index * 8U);
    }
    return value;
}

void store_little_endian(std::span<std::byte> bytes, std::uint64_t value) noexcept
{
    for (std::size_t index = 0U; index < bytes.size(); ++index)
    {
        bytes[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

} // namespace

extern "C" std::uint32_t switchrecomp_runtime_guest_load(RuntimeContext* runtime,
                                                           std::uint64_t address,
                                                           std::uint8_t size,
                                                           std::uint64_t* result) noexcept
{
    if (runtime == nullptr || runtime->memory == nullptr || result == nullptr)
    {
        return failure(runtime, make_error(ErrorCode::InvalidRuntimeContext,
                                            "guest load requires memory, result, and context"));
    }
    const auto valid_size = validate_size(size);
    if (!valid_size)
    {
        return failure(runtime, valid_size.error());
    }
    std::array<std::byte, 8> bytes{};
    const auto read = runtime->memory->read(address, std::span<std::byte>(bytes).first(size));
    if (!read)
    {
        return failure(runtime, read.error());
    }
    *result = load_little_endian(std::span<const std::byte>(bytes).first(size));
    return 0U;
}

extern "C" std::uint32_t switchrecomp_runtime_guest_store(RuntimeContext* runtime,
                                                            std::uint64_t address,
                                                            std::uint8_t size,
                                                            std::uint64_t value) noexcept
{
    if (runtime == nullptr || runtime->memory == nullptr)
    {
        return failure(runtime, make_error(ErrorCode::InvalidRuntimeContext,
                                            "guest store requires memory and context"));
    }
    const auto valid_size = validate_size(size);
    if (!valid_size)
    {
        return failure(runtime, valid_size.error());
    }
    std::array<std::byte, 8> bytes{};
    store_little_endian(std::span<std::byte>(bytes).first(size), value);
    const auto write = runtime->memory->write(address, std::span<const std::byte>(bytes).first(size));
    if (!write)
    {
        return failure(runtime, write.error());
    }
    return 0U;
}

extern "C" std::uint32_t switchrecomp_runtime_guest_address_add(RuntimeContext* runtime,
                                                                  std::uint64_t base,
                                                                  std::int64_t offset,
                                                                  std::uint64_t* result) noexcept
{
    if (runtime == nullptr || result == nullptr)
    {
        return failure(runtime, make_error(ErrorCode::InvalidRuntimeContext,
                                            "guest address addition requires context and result"));
    }
    const auto sum = checked_add_signed_u64(base, offset);
    if (!sum)
    {
        return failure(runtime, sum.error());
    }
    *result = sum.value();
    return 0U;
}

extern "C" std::uint32_t switchrecomp_runtime_trap(RuntimeContext* runtime,
                                                     const char* reason) noexcept
{
    return failure(runtime, make_error(ErrorCode::ExecutionTrap,
                                       std::string("guest execution trapped: ") +
                                           (reason == nullptr ? "unknown reason" : reason)));
}

const char* execution_status_name(ExecutionStatus status) noexcept
{
    switch (status)
    {
    case ExecutionStatus::Returned: return "returned";
    case ExecutionStatus::Trapped: return "trapped";
    case ExecutionStatus::Fault: return "fault";
    case ExecutionStatus::LimitExceeded: return "limit_exceeded";
    }
    return "unknown";
}

} // namespace switchrecomp::runtime
