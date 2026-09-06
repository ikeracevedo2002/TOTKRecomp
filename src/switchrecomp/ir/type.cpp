#include "switchrecomp/ir/type.hpp"

namespace switchrecomp::ir
{

std::string_view type_name(Type type) noexcept
{
    switch (type.kind())
    {
    case TypeKind::Void:
        return "void";
    case TypeKind::I1:
        return "i1";
    case TypeKind::I8:
        return "i8";
    case TypeKind::I16:
        return "i16";
    case TypeKind::I32:
        return "i32";
    case TypeKind::I64:
        return "i64";
    }
    return "invalid";
}

} // namespace switchrecomp::ir
