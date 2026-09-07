#pragma once

#include <cstdint>

namespace switchrecomp::common
{

// Returns the upper 64 bits of the mathematical unsigned 64x64 product.
// The calculation is deliberately expressed with 32-bit limbs so that the
// semantic reference path does not depend on a compiler-specific 128-bit
// integer extension.
[[nodiscard]] constexpr std::uint64_t multiply_high_unsigned_64(
    std::uint64_t left, std::uint64_t right) noexcept
{
    constexpr std::uint64_t limb_mask = 0xffffffffULL;
    const auto left_low = left & limb_mask;
    const auto left_high = left >> 32U;
    const auto right_low = right & limb_mask;
    const auto right_high = right >> 32U;

    const auto low_product = left_low * right_low;
    const auto cross_left = left_low * right_high;
    const auto cross_right = left_high * right_low;
    const auto high_product = left_high * right_high;

    // The low 64-bit product is only used to detect carries caused by the
    // two cross terms. Unsigned addition is defined modulo 2^64.
    std::uint64_t low = low_product;
    std::uint64_t carry = 0U;
    auto previous = low;
    low += cross_left << 32U;
    carry += low < previous ? 1U : 0U;
    previous = low;
    low += cross_right << 32U;
    carry += low < previous ? 1U : 0U;

    return high_product + (cross_left >> 32U) + (cross_right >> 32U) + carry;
}

} // namespace switchrecomp::common
