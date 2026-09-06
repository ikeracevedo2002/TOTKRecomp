#include "switchrecomp/aarch64/instruction.hpp"

namespace switchrecomp::aarch64
{

std::string_view operand_kind_name(OperandKind kind) noexcept
{
    switch (kind)
    {
    case OperandKind::Register:
        return "register";
    case OperandKind::Immediate:
        return "immediate";
    case OperandKind::Memory:
        return "memory";
    case OperandKind::Condition:
        return "condition";
    case OperandKind::System:
        return "system";
    case OperandKind::Other:
        return "other";
    }
    return "unknown";
}

std::string_view condition_code_name(ConditionCode condition) noexcept
{
    switch (condition)
    {
    case ConditionCode::Eq:
        return "eq";
    case ConditionCode::Ne:
        return "ne";
    case ConditionCode::Cs:
        return "cs";
    case ConditionCode::Cc:
        return "cc";
    case ConditionCode::Mi:
        return "mi";
    case ConditionCode::Pl:
        return "pl";
    case ConditionCode::Vs:
        return "vs";
    case ConditionCode::Vc:
        return "vc";
    case ConditionCode::Hi:
        return "hi";
    case ConditionCode::Ls:
        return "ls";
    case ConditionCode::Ge:
        return "ge";
    case ConditionCode::Lt:
        return "lt";
    case ConditionCode::Gt:
        return "gt";
    case ConditionCode::Le:
        return "le";
    case ConditionCode::Al:
        return "al";
    case ConditionCode::Nv:
        return "nv";
    }
    return "unknown";
}

std::string_view instruction_id_name(InstructionId id) noexcept
{
    switch (id)
    {
    case InstructionId::Unknown:
        return "unknown";
    case InstructionId::Nop:
        return "nop";
    case InstructionId::Add:
        return "add";
    case InstructionId::Adds:
        return "adds";
    case InstructionId::Sub:
        return "sub";
    case InstructionId::Subs:
        return "subs";
    case InstructionId::And:
        return "and";
    case InstructionId::Orr:
        return "orr";
    case InstructionId::Eor:
        return "eor";
    case InstructionId::Mov:
        return "mov";
    case InstructionId::Cmp:
        return "cmp";
    case InstructionId::Csel:
        return "csel";
    case InstructionId::Movz:
        return "movz";
    case InstructionId::Movk:
        return "movk";
    case InstructionId::Adr:
        return "adr";
    case InstructionId::Adrp:
        return "adrp";
    case InstructionId::Ldr:
        return "ldr";
    case InstructionId::Str:
        return "str";
    case InstructionId::Ldp:
        return "ldp";
    case InstructionId::Stp:
        return "stp";
    case InstructionId::Ldur:
        return "ldur";
    case InstructionId::Stur:
        return "stur";
    case InstructionId::LdrLiteral:
        return "ldr_literal";
    case InstructionId::B:
        return "b";
    case InstructionId::Bl:
        return "bl";
    case InstructionId::BCond:
        return "b.cond";
    case InstructionId::Br:
        return "br";
    case InstructionId::Blr:
        return "blr";
    case InstructionId::Ret:
        return "ret";
    case InstructionId::Cbz:
        return "cbz";
    case InstructionId::Cbnz:
        return "cbnz";
    case InstructionId::Tbz:
        return "tbz";
    case InstructionId::Tbnz:
        return "tbnz";
    case InstructionId::FpSimd:
        return "fp_simd";
    case InstructionId::Ldxr:
        return "ldxr";
    case InstructionId::Stxr:
        return "stxr";
    case InstructionId::Ldaxr:
        return "ldaxr";
    case InstructionId::Stlxr:
        return "stlxr";
    case InstructionId::Dmb:
        return "dmb";
    case InstructionId::Dsb:
        return "dsb";
    case InstructionId::Isb:
        return "isb";
    case InstructionId::Svc:
        return "svc";
    case InstructionId::Brk:
        return "brk";
    case InstructionId::Hlt:
        return "hlt";
    case InstructionId::Hvc:
        return "hvc";
    case InstructionId::Smc:
        return "smc";
    case InstructionId::Eret:
        return "eret";
    }
    return "unknown";
}

std::string_view control_flow_kind_name(ControlFlowKind kind) noexcept
{
    switch (kind)
    {
    case ControlFlowKind::Fallthrough:
        return "fallthrough";
    case ControlFlowKind::DirectBranch:
        return "direct_branch";
    case ControlFlowKind::ConditionalBranch:
        return "conditional_branch";
    case ControlFlowKind::DirectCall:
        return "direct_call";
    case ControlFlowKind::IndirectBranch:
        return "indirect_branch";
    case ControlFlowKind::IndirectCall:
        return "indirect_call";
    case ControlFlowKind::Return:
        return "return";
    case ControlFlowKind::Trap:
        return "trap";
    case ControlFlowKind::Exception:
        return "exception";
    case ControlFlowKind::Unknown:
        return "unknown";
    }
    return "unknown";
}

} // namespace switchrecomp::aarch64
