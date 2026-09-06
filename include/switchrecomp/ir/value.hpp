#pragma once

#include <cstdint>
#include <limits>

namespace switchrecomp::ir
{

struct ValueId
{
    std::uint32_t value = invalid_value;
    static constexpr std::uint32_t invalid_value = std::numeric_limits<std::uint32_t>::max();

    [[nodiscard]] constexpr bool valid() const noexcept { return value != invalid_value; }
    friend constexpr bool operator==(ValueId, ValueId) = default;
    friend constexpr bool operator<(ValueId left, ValueId right) noexcept
    {
        return left.value < right.value;
    }
};

struct BlockId
{
    std::uint32_t value = invalid_value;
    static constexpr std::uint32_t invalid_value = std::numeric_limits<std::uint32_t>::max();

    [[nodiscard]] constexpr bool valid() const noexcept { return value != invalid_value; }
    friend constexpr bool operator==(BlockId, BlockId) = default;
    friend constexpr bool operator<(BlockId left, BlockId right) noexcept
    {
        return left.value < right.value;
    }
};

} // namespace switchrecomp::ir
