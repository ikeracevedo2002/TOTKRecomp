#pragma once

#include "switchrecomp/ir/register.hpp"
#include "switchrecomp/ir/opcode.hpp"
#include "switchrecomp/runtime/fp.hpp"

#include <array>
#include <cstddef>
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
    std::uint32_t fpcr = 0U;
    std::uint32_t fpsr = 0U;
    std::array<Vector128, 32> vreg{};
    std::uint64_t tpidr_el0 = 0U;
    std::uint64_t tpidrro_el0 = 0U;
};

static_assert(std::is_standard_layout_v<CpuState>);
static_assert(sizeof(CpuState::x) == sizeof(std::uint64_t) * 31U);
static_assert(sizeof(CpuState::vreg) == sizeof(Vector128) * 32U);
static_assert(offsetof(CpuState, sp) == 31U * sizeof(std::uint64_t));
static_assert(offsetof(CpuState, pc) == 32U * sizeof(std::uint64_t));
static_assert(offsetof(CpuState, fpcr) == 268U);
static_assert(offsetof(CpuState, fpsr) == 272U);
static_assert(offsetof(CpuState, vreg) == 280U);
static_assert(offsetof(CpuState, tpidr_el0) == 792U);
static_assert(offsetof(CpuState, tpidrro_el0) == 800U);
static_assert(sizeof(CpuState) == 808U);

[[nodiscard]] std::uint64_t read_register(const CpuState& state, const ir::GuestRegister& reg) noexcept;
void write_register(CpuState& state, const ir::GuestRegister& reg, std::uint64_t value) noexcept;

[[nodiscard]] Vector128 read_vector_register(const CpuState& state, std::uint8_t index) noexcept;
void write_vector_register(CpuState& state, std::uint8_t index, Vector128 value) noexcept;
[[nodiscard]] std::uint64_t read_vector_lane(const CpuState& state, std::uint8_t index,
                                             std::uint8_t element_bits, std::uint8_t lane) noexcept;
void write_vector_lane(CpuState& state, std::uint8_t index, std::uint8_t element_bits,
                       std::uint8_t lane, std::uint64_t value) noexcept;
void write_scalar_vector(CpuState& state, std::uint8_t index, std::uint8_t element_bits,
                         std::uint64_t value) noexcept;

[[nodiscard]] bool read_flag(const CpuState& state, ir::Flag flag) noexcept;
void write_flag(CpuState& state, ir::Flag flag, bool value) noexcept;

[[nodiscard]] bool evaluate_condition(ir::ConditionCode condition, bool n, bool z, bool c,
                                      bool v) noexcept;

} // namespace switchrecomp::runtime
