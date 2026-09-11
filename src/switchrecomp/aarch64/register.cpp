#include "switchrecomp/aarch64/register.hpp"

#include <sstream>

namespace switchrecomp::aarch64
{

std::string_view register_kind_name(RegisterKind kind) noexcept
{
    switch (kind)
    {
    case RegisterKind::Invalid:
        return "invalid";
    case RegisterKind::General:
        return "general";
    case RegisterKind::Vector:
        return "vector";
    case RegisterKind::Predicate:
        return "predicate";
    case RegisterKind::System:
        return "system";
    }
    return "unknown";
}

std::string_view register_width_name(RegisterWidth width) noexcept
{
    switch (width)
    {
    case RegisterWidth::None:
        return "";
    case RegisterWidth::W32:
        return "w";
    case RegisterWidth::X64:
        return "x";
    case RegisterWidth::B8:
        return "b";
    case RegisterWidth::H16:
        return "h";
    case RegisterWidth::S32:
        return "s";
    case RegisterWidth::D64:
        return "d";
    case RegisterWidth::Q128:
        return "q";
    }
    return "";
}

std::string register_name(const Register& reg)
{
    if (reg.kind == RegisterKind::General)
    {
        if (reg.is_stack_pointer)
        {
            return reg.width == RegisterWidth::W32 ? "wsp" : "sp";
        }
        if (reg.is_zero)
        {
            return reg.width == RegisterWidth::W32 ? "wzr" : "xzr";
        }
        std::ostringstream output;
        output << register_width_name(reg.width) << static_cast<unsigned int>(reg.index);
        return output.str();
    }

    if (reg.kind == RegisterKind::Vector)
    {
        std::ostringstream output;
        output << register_width_name(reg.width) << static_cast<unsigned int>(reg.index);
        return output.str();
    }

    if (reg.kind == RegisterKind::Predicate)
    {
        return "p" + std::to_string(reg.index);
    }

    return "system";
}

} // namespace switchrecomp::aarch64
