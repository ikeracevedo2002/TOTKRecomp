#pragma once

#include "switchrecomp/ir/register.hpp"
#include "switchrecomp/ir/opcode.hpp"

#include <array>
#include <cstdint>
#include <type_traits>

namespace switchrecomp::runtime
{

struct CpuState
{
    std::array<std::uint64_t, 31> x{};
    std::uint64_t sp = 0U;
    std::uint64_t pc = 0U;
    std::uint8_t n = 0U;
    std::uint8_t z = 0U;
    std::uint8_t c = 0U;
    std::uint8_t v = 0U;
};

static_assert(std::is_standard_layout_v<CpuState>);
static_assert(sizeof(CpuState::x) == sizeof(std::uint64_t) * 31U);

[[nodiscard]] std::uint64_t read_register(const CpuState& state, const ir::GuestRegister& reg) noexcept;
void write_register(CpuState& state, const ir::GuestRegister& reg, std::uint64_t value) noexcept;

[[nodiscard]] bool read_flag(const CpuState& state, ir::Flag flag) noexcept;
void write_flag(CpuState& state, ir::Flag flag, bool value) noexcept;

} // namespace switchrecomp::runtime
