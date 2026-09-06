#include "switchrecomp/ir/instruction.hpp"

namespace switchrecomp::ir
{

std::string_view opcode_name(IrOpcode opcode) noexcept
{
    switch (opcode)
    {
    case IrOpcode::Constant:
        return "constant";
    case IrOpcode::ReadRegister:
        return "read_reg";
    case IrOpcode::WriteRegister:
        return "write_reg";
    case IrOpcode::Add:
        return "add";
    case IrOpcode::Sub:
        return "sub";
    case IrOpcode::And:
        return "and";
    case IrOpcode::Or:
        return "or";
    case IrOpcode::Xor:
        return "xor";
    case IrOpcode::CompareEqual:
        return "cmp_eq";
    case IrOpcode::CompareNotEqual:
        return "cmp_ne";
    case IrOpcode::GuestLoad:
        return "guest_load";
    case IrOpcode::GuestStore:
        return "guest_store";
    case IrOpcode::Branch:
        return "branch";
    case IrOpcode::ConditionalBranch:
        return "cond_branch";
    case IrOpcode::Return:
        return "return";
    case IrOpcode::Nop:
        return "nop";
    }
    return "invalid";
}

} // namespace switchrecomp::ir
