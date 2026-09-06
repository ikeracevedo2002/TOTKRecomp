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
    case InstructionId::Ands:
        return "ands";
    case InstructionId::Orr:
        return "orr";
    case InstructionId::Orn:
        return "orn";
    case InstructionId::Eor:
        return "eor";
    case InstructionId::Eon:
        return "eon";
    case InstructionId::Bic:
        return "bic";
    case InstructionId::Bics:
        return "bics";
    case InstructionId::Mov:
        return "mov";
    case InstructionId::Mvn:
        return "mvn";
    case InstructionId::Cmp:
        return "cmp";
    case InstructionId::Cmn:
        return "cmn";
    case InstructionId::Ccmp:
        return "ccmp";
    case InstructionId::Ccmn:
        return "ccmn";
    case InstructionId::Tst:
        return "tst";
    case InstructionId::Neg:
        return "neg";
    case InstructionId::Negs:
        return "negs";
    case InstructionId::Csel:
        return "csel";
    case InstructionId::Csinc:
        return "csinc";
    case InstructionId::Csinv:
        return "csinv";
    case InstructionId::Csneg:
        return "csneg";
    case InstructionId::Cset:
        return "cset";
    case InstructionId::Csetm:
        return "csetm";
    case InstructionId::Cinc:
        return "cinc";
    case InstructionId::Cinv:
        return "cinv";
    case InstructionId::Cneg:
        return "cneg";
    case InstructionId::Movz:
        return "movz";
    case InstructionId::Movk:
        return "movk";
    case InstructionId::Movn:
        return "movn";
    case InstructionId::Lsl:
        return "lsl";
    case InstructionId::Lsr:
        return "lsr";
    case InstructionId::Asr:
        return "asr";
    case InstructionId::Ror:
        return "ror";
    case InstructionId::Ubfm:
        return "ubfm";
    case InstructionId::Sbfm:
        return "sbfm";
    case InstructionId::Bfm:
        return "bfm";
    case InstructionId::Mul:
        return "mul";
    case InstructionId::Madd:
        return "madd";
    case InstructionId::Msub:
        return "msub";
    case InstructionId::Mneg:
        return "mneg";
    case InstructionId::Udiv:
        return "udiv";
    case InstructionId::Sdiv:
        return "sdiv";
    case InstructionId::Adr:
        return "adr";
    case InstructionId::Adrp:
        return "adrp";
    case InstructionId::Ldr:
        return "ldr";
    case InstructionId::Ldrb:
        return "ldrb";
    case InstructionId::Ldrh:
        return "ldrh";
    case InstructionId::Ldrsb:
        return "ldrsb";
    case InstructionId::Ldrsh:
        return "ldrsh";
    case InstructionId::Ldrsw:
        return "ldrsw";
    case InstructionId::Str:
        return "str";
    case InstructionId::Strb:
        return "strb";
    case InstructionId::Strh:
        return "strh";
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
