#pragma once

#include <cstdint>
#include <string_view>

namespace switchrecomp::ir
{

enum class IrType : std::uint8_t
{
    I1,
    I8,
    I16,
    I32,
    I64,
};

[[nodiscard]] constexpr unsigned int bit_width(IrType type) noexcept
{
    switch (type)
    {
    case IrType::I1:
        return 1U;
    case IrType::I8:
        return 8U;
    case IrType::I16:
        return 16U;
    case IrType::I32:
        return 32U;
    case IrType::I64:
        return 64U;
    }
    return 0U;
}

[[nodiscard]] constexpr std::uint64_t mask_value(IrType type, std::uint64_t value) noexcept
{
    return type == IrType::I64 ? value : value & ((std::uint64_t{1} << bit_width(type)) - 1U);
}

[[nodiscard]] std::string_view type_name(IrType type) noexcept;

} // namespace switchrecomp::ir
