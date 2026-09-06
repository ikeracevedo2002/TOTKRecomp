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

} // namespace switchrecomp::runtime
