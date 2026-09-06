#include "switchrecomp/ir/register.hpp"

#include <string>

namespace switchrecomp::ir
{

std::string register_name(const GuestRegister& reg)
{
    if (reg.is_stack_pointer)
    {
        return reg.width == RegisterWidth::W32 ? "wsp" : "sp";
    }
    if (reg.is_zero)
    {
        return reg.width == RegisterWidth::W32 ? "wzr" : "xzr";
    }
    return std::string(reg.width == RegisterWidth::W32 ? "w" : "x") +
           std::to_string(static_cast<unsigned int>(reg.index));
}

} // namespace switchrecomp::ir
