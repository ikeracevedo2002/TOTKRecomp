#pragma once

#include "switchrecomp/aarch64/register.hpp"
#include "switchrecomp/common/result.hpp"

#include <array>
#include <cstdint>

namespace switchrecomp::runtime
{

// A portable representation of one AArch64 Q register.  It intentionally does not
// expose the host's SIMD ABI or alignment requirements.
struct Vector128
{
    std::uint64_t low = 0U;
    std::uint64_t high = 0U;

    friend bool operator==(const Vector128&, const Vector128&) = default;
};

struct CpuState
{
    std::array<std::uint64_t, 31> x{};
    std::uint64_t sp = 0U;
    std::uint64_t pc = 0U;
    std::uint32_t nzcv = 0U;
    std::uint32_t fpcr = 0U;
    std::uint32_t fpsr = 0U;
    std::array<Vector128, 32> v{};
};

[[nodiscard]] Result<std::uint64_t> read_register(
    const CpuState& state, const aarch64::Register& reg);

// The value is interpreted in the register's declared width.  W-register writes
// always replace the complete X register with a zero-extended 32-bit value.
[[nodiscard]] Result<void> write_register(
    CpuState& state, const aarch64::Register& reg, std::uint64_t value);

[[nodiscard]] Result<Vector128> read_vector(const CpuState& state,
                                            const aarch64::Register& reg);
[[nodiscard]] Result<void> write_vector(CpuState& state, const aarch64::Register& reg,
                                        Vector128 value);

} // namespace switchrecomp::runtime
