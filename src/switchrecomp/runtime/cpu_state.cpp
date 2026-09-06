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
