#include "switchrecomp/ir/type.hpp"

namespace switchrecomp::ir
{

std::string_view type_name(IrType type) noexcept
{
    switch (type)
    {
    case IrType::I1:
        return "i1";
    case IrType::I8:
        return "i8";
    case IrType::I16:
        return "i16";
    case IrType::I32:
        return "i32";
    case IrType::I64:
        return "i64";
    }
    return "invalid";
}

} // namespace switchrecomp::ir
