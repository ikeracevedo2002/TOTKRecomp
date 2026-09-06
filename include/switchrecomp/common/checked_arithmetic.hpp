#pragma once

#include "switchrecomp/common/result.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace switchrecomp
{

struct CheckedRange
{
    std::size_t offset;
    std::size_t size;

    [[nodiscard]] std::size_t end() const noexcept
    {
        return offset + size;
    }
};

struct CheckedGuestRange
{
    std::uint64_t address;
    std::uint64_t size;

    [[nodiscard]] std::uint64_t end() const noexcept
    {
        return address + size;
    }
};

[[nodiscard]] inline Result<std::size_t> checked_add(std::size_t left, std::size_t right)
{
    if (right > std::numeric_limits<std::size_t>::max() - left)
    {
        return Result<std::size_t>::failure(
            make_error(ErrorCode::ArithmeticOverflow, "checked addition overflow"));
    }
    return Result<std::size_t>::success(left + right);
}

[[nodiscard]] inline Result<std::uint64_t> checked_add_u64(std::uint64_t left,
                                                           std::uint64_t right)
{
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
    {
        return Result<std::uint64_t>::failure(
            make_error(ErrorCode::ArithmeticOverflow, "checked 64-bit addition overflow"));
    }
    return Result<std::uint64_t>::success(left + right);
}

[[nodiscard]] inline Result<std::uint64_t> checked_mul_u64(std::uint64_t left,
                                                           std::uint64_t right)
{
    if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left)
    {
        return Result<std::uint64_t>::failure(
            make_error(ErrorCode::ArithmeticOverflow, "checked 64-bit multiplication overflow"));
    }
    return Result<std::uint64_t>::success(left * right);
}

// Add a signed module-relative offset without first converting it to unsigned. This is
// intentionally kept in the common checked-arithmetic layer because MOD0 and relocation
// metadata both use signed values in some Switch toolchains.
[[nodiscard]] inline Result<std::uint64_t> checked_add_signed_u64(std::uint64_t base,
                                                                  std::int64_t offset)
{
    if (offset >= 0)
    {
        return checked_add_u64(base, static_cast<std::uint64_t>(offset));
    }

    // Converting through int64_t makes INT64_MIN safe; negating it directly would overflow.
    const auto magnitude = std::uint64_t{0} - static_cast<std::uint64_t>(offset);
    if (magnitude > base)
    {
        return Result<std::uint64_t>::failure(
            make_error(ErrorCode::ArithmeticUnderflow, "checked signed addition underflow"));
    }
    return Result<std::uint64_t>::success(base - magnitude);
}

// Interpret a sign-extended two's-complement bit pattern without relying on a
// target compiler's implementation-defined unsigned-to-signed conversion.
[[nodiscard]] inline std::int64_t signed_value_from_u64(std::uint64_t value) noexcept
{
    if ((value & (std::uint64_t{1} << 63U)) == 0U)
    {
        return static_cast<std::int64_t>(value);
    }
    const auto magnitude = std::uint64_t{0} - value;
    if (magnitude == (std::uint64_t{1} << 63U))
    {
        return std::numeric_limits<std::int64_t>::min();
    }
    return -static_cast<std::int64_t>(magnitude);
}

[[nodiscard]] inline Result<CheckedGuestRange> checked_guest_range(std::uint64_t address,
                                                                  std::uint64_t size)
{
    const auto end = checked_add_u64(address, size);
    if (!end)
    {
        return Result<CheckedGuestRange>::failure(end.error());
    }
    return Result<CheckedGuestRange>::success(CheckedGuestRange{address, size});
}

[[nodiscard]] inline Result<std::size_t> checked_sub(std::size_t left, std::size_t right)
{
    if (right > left)
    {
        return Result<std::size_t>::failure(
            make_error(ErrorCode::ArithmeticUnderflow, "checked subtraction underflow"));
    }
    return Result<std::size_t>::success(left - right);
}

[[nodiscard]] inline Result<CheckedRange> checked_range(
    std::size_t offset,
    std::size_t size,
    std::size_t container_size)
{
    const auto end = checked_add(offset, size);
    if (!end)
    {
        return Result<CheckedRange>::failure(end.error());
    }
    if (end.value() > container_size)
    {
        return Result<CheckedRange>::failure(make_error(
            ErrorCode::OutOfBounds,
            "requested range exceeds the available container size"));
    }
    return Result<CheckedRange>::success(CheckedRange{offset, size});
}

[[nodiscard]] inline bool contains_range(
    std::size_t container_offset,
    std::size_t container_size,
    std::size_t range_offset,
    std::size_t range_size) noexcept
{
    if (range_offset < container_offset)
    {
        return false;
    }
    const auto range_end = range_offset > std::numeric_limits<std::size_t>::max() - range_size
                               ? std::numeric_limits<std::size_t>::max()
                               : range_offset + range_size;
    const auto container_end = container_offset > std::numeric_limits<std::size_t>::max() - container_size
                                   ? std::numeric_limits<std::size_t>::max()
                                   : container_offset + container_size;
    return range_end <= container_end;
}

} // namespace switchrecomp
