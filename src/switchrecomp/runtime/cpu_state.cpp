#include "switchrecomp/runtime/cpu_state.hpp"

#include <limits>
#include <string>

namespace switchrecomp::runtime
{

namespace
{

[[nodiscard]] bool is_gpr_width(aarch64::RegisterWidth width) noexcept
{
    return width == aarch64::RegisterWidth::W32 || width == aarch64::RegisterWidth::X64;
}

[[nodiscard]] std::uint64_t width_mask(aarch64::RegisterWidth width) noexcept
{
    return width == aarch64::RegisterWidth::W32 ? std::uint64_t{0xffffffffU}
                                                : std::numeric_limits<std::uint64_t>::max();
}

[[nodiscard]] Error invalid_register(const aarch64::Register& reg, std::string message)
{
    return make_error(ErrorCode::InvalidArgument,
                      "invalid register " + aarch64::register_name(reg) + ": " + message);
}

} // namespace

Result<std::uint64_t> read_register(const CpuState& state, const aarch64::Register& reg)
{
    if (reg.kind != aarch64::RegisterKind::General || !is_gpr_width(reg.width))
    {
        return Result<std::uint64_t>::failure(
            invalid_register(reg, "only W/X general registers are supported"));
    }
    if (reg.is_zero)
    {
        return Result<std::uint64_t>::success(0U);
    }
    if (reg.is_stack_pointer)
    {
        return Result<std::uint64_t>::success(state.sp & width_mask(reg.width));
    }
    if (reg.index >= state.x.size())
    {
        return Result<std::uint64_t>::failure(invalid_register(reg, "general register index is out of range"));
    }
    return Result<std::uint64_t>::success(state.x[reg.index] & width_mask(reg.width));
}

Result<void> write_register(CpuState& state, const aarch64::Register& reg, std::uint64_t value)
{
    if (reg.kind != aarch64::RegisterKind::General || !is_gpr_width(reg.width))
    {
        return Result<void>::failure(
            invalid_register(reg, "only W/X general registers are supported"));
    }
    if (reg.is_zero)
    {
        return Result<void>::success();
    }
    const auto narrowed = value & width_mask(reg.width);
    if (reg.is_stack_pointer)
    {
        state.sp = narrowed;
        return Result<void>::success();
    }
    if (reg.index >= state.x.size())
    {
        return Result<void>::failure(invalid_register(reg, "general register index is out of range"));
    }
    // Assigning a W register is deliberately a complete X-register write.
    state.x[reg.index] = narrowed;
    return Result<void>::success();
}

Result<Vector128> read_vector(const CpuState& state, const aarch64::Register& reg)
{
    if (reg.kind != aarch64::RegisterKind::Vector || reg.index >= state.v.size())
    {
        return Result<Vector128>::failure(
            invalid_register(reg, "vector register index is out of range"));
    }
    return Result<Vector128>::success(state.v[reg.index]);
}

Result<void> write_vector(CpuState& state, const aarch64::Register& reg, Vector128 value)
{
    if (reg.kind != aarch64::RegisterKind::Vector || reg.index >= state.v.size())
    {
        return Result<void>::failure(
            invalid_register(reg, "vector register index is out of range"));
    }
    state.v[reg.index] = value;
    return Result<void>::success();
}

} // namespace switchrecomp::runtime
