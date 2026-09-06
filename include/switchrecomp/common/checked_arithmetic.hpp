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

[[nodiscard]] inline Result<std::size_t> checked_add(std::size_t left, std::size_t right)
{
    if (right > std::numeric_limits<std::size_t>::max() - left)
    {
        return Result<std::size_t>::failure(
            make_error(ErrorCode::ArithmeticOverflow, "checked addition overflow"));
    }
    return Result<std::size_t>::success(left + right);
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
