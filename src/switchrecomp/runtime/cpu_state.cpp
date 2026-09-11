#include "switchrecomp/runtime/cpu_state.hpp"

namespace switchrecomp::runtime
{

std::uint64_t read_register(const CpuState& state, const ir::GuestRegister& reg) noexcept
{
    if (reg.is_zero)
    {
        return 0U;
    }
    if (reg.is_stack_pointer)
    {
        return reg.width == ir::RegisterWidth::W32 ? static_cast<std::uint32_t>(state.sp)
                                                   : state.sp;
    }
    if (reg.index >= state.x.size())
    {
        return 0U;
    }
    return reg.width == ir::RegisterWidth::W32 ? static_cast<std::uint32_t>(state.x[reg.index])
                                               : state.x[reg.index];
}

void write_register(CpuState& state, const ir::GuestRegister& reg, std::uint64_t value) noexcept
{
    if (reg.is_zero)
    {
        return;
    }
    const auto narrowed = reg.width == ir::RegisterWidth::W32
                              ? static_cast<std::uint64_t>(static_cast<std::uint32_t>(value))
                              : value;
    if (reg.is_stack_pointer)
    {
        state.sp = narrowed;
    }
    else if (reg.index < state.x.size())
    {
        state.x[reg.index] = narrowed;
    }
}

Vector128 read_vector_register(const CpuState& state, std::uint8_t index) noexcept
{
    return index < state.vreg.size() ? state.vreg[index] : Vector128{};
}

void write_vector_register(CpuState& state, std::uint8_t index, Vector128 value) noexcept
{
    if (index < state.vreg.size())
    {
        state.vreg[index] = value;
    }
}

std::uint64_t read_vector_lane(const CpuState& state, std::uint8_t index,
                               std::uint8_t element_bits, std::uint8_t lane) noexcept
{
    return read_lane_bits(read_vector_register(state, index), element_bits, lane);
}

void write_vector_lane(CpuState& state, std::uint8_t index, std::uint8_t element_bits,
                       std::uint8_t lane, std::uint64_t value) noexcept
{
    auto vector = read_vector_register(state, index);
    write_lane_bits(vector, element_bits, lane, value);
    write_vector_register(state, index, vector);
}

void write_scalar_vector(CpuState& state, std::uint8_t index, std::uint8_t element_bits,
                         std::uint64_t value) noexcept
{
    Vector128 vector{};
    write_lane_bits(vector, element_bits, 0U, value);
    write_vector_register(state, index, vector);
}

bool read_flag(const CpuState& state, ir::Flag flag) noexcept
{
    switch (flag)
    {
    case ir::Flag::N: return state.n != 0U;
    case ir::Flag::Z: return state.z != 0U;
    case ir::Flag::C: return state.c != 0U;
    case ir::Flag::V: return state.v != 0U;
    }
    return false;
}

void write_flag(CpuState& state, ir::Flag flag, bool value) noexcept
{
    const auto bit = static_cast<std::uint8_t>(value ? 1U : 0U);
    switch (flag)
    {
    case ir::Flag::N: state.n = bit; break;
    case ir::Flag::Z: state.z = bit; break;
    case ir::Flag::C: state.c = bit; break;
    case ir::Flag::V: state.v = bit; break;
    }
}

bool evaluate_condition(ir::ConditionCode condition, bool n, bool z, bool c, bool v) noexcept
{
    switch (condition)
    {
    case ir::ConditionCode::Eq: return z;
    case ir::ConditionCode::Ne: return !z;
    case ir::ConditionCode::Cs: return c;
    case ir::ConditionCode::Cc: return !c;
    case ir::ConditionCode::Mi: return n;
    case ir::ConditionCode::Pl: return !n;
    case ir::ConditionCode::Vs: return v;
    case ir::ConditionCode::Vc: return !v;
    case ir::ConditionCode::Hi: return c && !z;
    case ir::ConditionCode::Ls: return !c || z;
    case ir::ConditionCode::Ge: return n == v;
    case ir::ConditionCode::Lt: return n != v;
    case ir::ConditionCode::Gt: return !z && n == v;
    case ir::ConditionCode::Le: return z || n != v;
    case ir::ConditionCode::Al: return true;
    case ir::ConditionCode::Nv: return false;
    }
    return false;
}

} // namespace switchrecomp::runtime
