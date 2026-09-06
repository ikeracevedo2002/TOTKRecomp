#include "switchrecomp/aarch64/instruction.hpp"

namespace switchrecomp::aarch64
{

std::string_view operand_kind_name(OperandKind kind) noexcept
{
    switch (kind)
    {
    case OperandKind::Register: return "register";
    case OperandKind::Immediate: return "immediate";
    case OperandKind::FloatingImmediate: return "floating_immediate";
    case OperandKind::Memory: return "memory";
    case OperandKind::Condition: return "condition";
    case OperandKind::System: return "system";
    case OperandKind::Other: return "other";
    }
    return "unknown";
}

std::string_view vector_arrangement_name(VectorArrangement arrangement) noexcept
{
    switch (arrangement)
    {
    case VectorArrangement::Invalid: return "invalid";
    case VectorArrangement::B8: return "8b";
    case VectorArrangement::B16: return "16b";
    case VectorArrangement::H4: return "4h";
    case VectorArrangement::H8: return "8h";
    case VectorArrangement::S2: return "2s";
    case VectorArrangement::S4: return "4s";
    case VectorArrangement::D1: return "1d";
    case VectorArrangement::D2: return "2d";
    case VectorArrangement::Q1: return "1q";
    }
    return "invalid";
}

std::uint8_t vector_element_bits(VectorArrangement arrangement) noexcept
{
    switch (arrangement)
    {
    case VectorArrangement::B8: case VectorArrangement::B16: return 8U;
    case VectorArrangement::H4: case VectorArrangement::H8: return 16U;
    case VectorArrangement::S2: case VectorArrangement::S4: return 32U;
    case VectorArrangement::D1: case VectorArrangement::D2: return 64U;
    case VectorArrangement::Invalid: case VectorArrangement::Q1: return 0U;
    }
    return 0U;
}

std::uint8_t vector_lane_count(VectorArrangement arrangement) noexcept
{
    switch (arrangement)
    {
    case VectorArrangement::B8: return 8U;
    case VectorArrangement::B16: return 16U;
    case VectorArrangement::H4: return 4U;
    case VectorArrangement::H8: return 8U;
    case VectorArrangement::S2: return 2U;
    case VectorArrangement::S4: return 4U;
    case VectorArrangement::D1: return 1U;
    case VectorArrangement::D2: return 2U;
    case VectorArrangement::Q1: return 1U;
    case VectorArrangement::Invalid: return 0U;
    }
    return 0U;
}

std::string_view condition_code_name(ConditionCode condition) noexcept
{
    switch (condition)
    {
    case ConditionCode::Eq: return "eq";
    case ConditionCode::Ne: return "ne";
    case ConditionCode::Cs: return "cs";
    case ConditionCode::Cc: return "cc";
    case ConditionCode::Mi: return "mi";
    case ConditionCode::Pl: return "pl";
    case ConditionCode::Vs: return "vs";
    case ConditionCode::Vc: return "vc";
    case ConditionCode::Hi: return "hi";
    case ConditionCode::Ls: return "ls";
    case ConditionCode::Ge: return "ge";
    case ConditionCode::Lt: return "lt";
    case ConditionCode::Gt: return "gt";
    case ConditionCode::Le: return "le";
    case ConditionCode::Al: return "al";
    case ConditionCode::Nv: return "nv";
    }
    return "unknown";
}

std::string_view instruction_id_name(InstructionId id) noexcept
{
    switch (id)
    {
    case InstructionId::Unknown: return "unknown";
    case InstructionId::Nop: return "nop";
    case InstructionId::Add: return "add";
    case InstructionId::Adds: return "adds";
    case InstructionId::Sub: return "sub";
    case InstructionId::Subs: return "subs";
    case InstructionId::And: return "and";
    case InstructionId::Ands: return "ands";
    case InstructionId::Orr: return "orr";
    case InstructionId::Orn: return "orn";
    case InstructionId::Eor: return "eor";
    case InstructionId::Eon: return "eon";
    case InstructionId::Bic: return "bic";
    case InstructionId::Bics: return "bics";
    case InstructionId::Mov: return "mov";
    case InstructionId::Mvn: return "mvn";
    case InstructionId::Cmp: return "cmp";
    case InstructionId::Cmn: return "cmn";
    case InstructionId::Ccmp: return "ccmp";
    case InstructionId::Ccmn: return "ccmn";
    case InstructionId::Tst: return "tst";
    case InstructionId::Neg: return "neg";
    case InstructionId::Negs: return "negs";
    case InstructionId::Csel: return "csel";
    case InstructionId::Csinc: return "csinc";
    case InstructionId::Csinv: return "csinv";
    case InstructionId::Csneg: return "csneg";
    case InstructionId::Cset: return "cset";
    case InstructionId::Csetm: return "csetm";
    case InstructionId::Cinc: return "cinc";
    case InstructionId::Cinv: return "cinv";
    case InstructionId::Cneg: return "cneg";
    case InstructionId::Movz: return "movz";
    case InstructionId::Movk: return "movk";
    case InstructionId::Movn: return "movn";
    case InstructionId::Lsl: return "lsl";
    case InstructionId::Lsr: return "lsr";
    case InstructionId::Asr: return "asr";
    case InstructionId::Ror: return "ror";
    case InstructionId::Ubfm: return "ubfm";
    case InstructionId::Sbfm: return "sbfm";
    case InstructionId::Bfm: return "bfm";
    case InstructionId::Mul: return "mul";
    case InstructionId::Madd: return "madd";
    case InstructionId::Msub: return "msub";
    case InstructionId::Mneg: return "mneg";
    case InstructionId::Udiv: return "udiv";
    case InstructionId::Sdiv: return "sdiv";
    case InstructionId::Adr: return "adr";
    case InstructionId::Adrp: return "adrp";
    case InstructionId::Ldr: return "ldr";
    case InstructionId::Ldrb: return "ldrb";
    case InstructionId::Ldrh: return "ldrh";
    case InstructionId::Ldrsb: return "ldrsb";
    case InstructionId::Ldrsh: return "ldrsh";
    case InstructionId::Ldrsw: return "ldrsw";
    case InstructionId::Str: return "str";
    case InstructionId::Strb: return "strb";
    case InstructionId::Strh: return "strh";
    case InstructionId::Ldp: return "ldp";
    case InstructionId::Stp: return "stp";
    case InstructionId::Ldur: return "ldur";
    case InstructionId::Stur: return "stur";
    case InstructionId::LdrLiteral: return "ldr_literal";
    case InstructionId::B: return "b";
    case InstructionId::Bl: return "bl";
    case InstructionId::BCond: return "b.cond";
    case InstructionId::Br: return "br";
    case InstructionId::Blr: return "blr";
    case InstructionId::Ret: return "ret";
    case InstructionId::Cbz: return "cbz";
    case InstructionId::Cbnz: return "cbnz";
    case InstructionId::Tbz: return "tbz";
    case InstructionId::Tbnz: return "tbnz";
    case InstructionId::FpSimd: return "fp_simd";
    case InstructionId::Ldxr: return "ldxr";
    case InstructionId::Ldxrb: return "ldxrb";
    case InstructionId::Ldxrh: return "ldxrh";
    case InstructionId::Stxr: return "stxr";
    case InstructionId::Stxrb: return "stxrb";
    case InstructionId::Stxrh: return "stxrh";
    case InstructionId::Ldaxr: return "ldaxr";
    case InstructionId::Ldaxrb: return "ldaxrb";
    case InstructionId::Ldaxrh: return "ldaxrh";
    case InstructionId::Stlxr: return "stlxr";
    case InstructionId::Stlxrb: return "stlxrb";
    case InstructionId::Stlxrh: return "stlxrh";
    case InstructionId::Ldar: return "ldar";
    case InstructionId::Ldarb: return "ldarb";
    case InstructionId::Ldarh: return "ldarh";
    case InstructionId::Stlr: return "stlr";
    case InstructionId::Stlrb: return "stlrb";
    case InstructionId::Stlrh: return "stlrh";
    case InstructionId::Ldxp: return "ldxp";
    case InstructionId::Ldaxp: return "ldaxp";
    case InstructionId::Stxp: return "stxp";
    case InstructionId::Stlxp: return "stlxp";
    case InstructionId::Clrex: return "clrex";
    case InstructionId::Dmb: return "dmb";
    case InstructionId::Dsb: return "dsb";
    case InstructionId::Isb: return "isb";
    case InstructionId::Mrs: return "mrs";
    case InstructionId::Msr: return "msr";
    case InstructionId::Svc: return "svc";
    case InstructionId::Brk: return "brk";
    case InstructionId::Hlt: return "hlt";
    case InstructionId::Hvc: return "hvc";
    case InstructionId::Smc: return "smc";
    case InstructionId::Eret: return "eret";
    }
    return "unknown";
}

std::string_view control_flow_kind_name(ControlFlowKind kind) noexcept
{
    switch (kind)
    {
    case ControlFlowKind::Fallthrough: return "fallthrough";
    case ControlFlowKind::DirectBranch: return "direct_branch";
    case ControlFlowKind::ConditionalBranch: return "conditional_branch";
    case ControlFlowKind::DirectCall: return "direct_call";
    case ControlFlowKind::IndirectBranch: return "indirect_branch";
    case ControlFlowKind::IndirectCall: return "indirect_call";
    case ControlFlowKind::Return: return "return";
    case ControlFlowKind::Trap: return "trap";
    case ControlFlowKind::Exception: return "exception";
    case ControlFlowKind::Unknown: return "unknown";
    }
    return "unknown";
}

std::string_view simd_operation_name(SimdOperation operation) noexcept
{
    switch (operation)
    {
    case SimdOperation::None: return "none";
    case SimdOperation::Fmov: return "fmov";
    case SimdOperation::Fadd: return "fadd";
    case SimdOperation::Fsub: return "fsub";
    case SimdOperation::Fmul: return "fmul";
    case SimdOperation::Fdiv: return "fdiv";
    case SimdOperation::Fneg: return "fneg";
    case SimdOperation::Fabs: return "fabs";
    case SimdOperation::Fsqrt: return "fsqrt";
    case SimdOperation::Fmin: return "fmin";
    case SimdOperation::Fmax: return "fmax";
    case SimdOperation::Fcmp: return "fcmp";
    case SimdOperation::Fcmpe: return "fcmpe";
    case SimdOperation::Fcsel: return "fcsel";
    case SimdOperation::Scvtf: return "scvtf";
    case SimdOperation::Ucvtf: return "ucvtf";
    case SimdOperation::Fcvtzs: return "fcvtzs";
    case SimdOperation::Fcvtzu: return "fcvtzu";
    case SimdOperation::Fcvt: return "fcvt";
    case SimdOperation::Frintn: return "frintn";
    case SimdOperation::Frintp: return "frintp";
    case SimdOperation::Frintm: return "frintm";
    case SimdOperation::Frintz: return "frintz";
    case SimdOperation::Fmadd: return "fmadd";
    case SimdOperation::Fmsub: return "fmsub";
    case SimdOperation::Fnmadd: return "fnmadd";
    case SimdOperation::Fnmsub: return "fnmsub";
    case SimdOperation::Dup: return "dup";
    case SimdOperation::Ins: return "ins";
    case SimdOperation::Umov: return "umov";
    case SimdOperation::Smov: return "smov";
    case SimdOperation::Ext: return "ext";
    case SimdOperation::Zip1: return "zip1";
    case SimdOperation::Zip2: return "zip2";
    case SimdOperation::Uzp1: return "uzp1";
    case SimdOperation::Uzp2: return "uzp2";
    case SimdOperation::Trn1: return "trn1";
    case SimdOperation::Trn2: return "trn2";
    case SimdOperation::Fcmeq: return "fcmeq";
    case SimdOperation::Fcmgt: return "fcmgt";
    case SimdOperation::Fcmge: return "fcmge";
    case SimdOperation::Cmeq: return "cmeq";
    case SimdOperation::Cmgt: return "cmgt";
    case SimdOperation::Cmge: return "cmge";
    case SimdOperation::Cmhi: return "cmhi";
    case SimdOperation::Cmhs: return "cmhs";
    }
    return "unknown";
}

std::string_view barrier_option_name(BarrierOption option) noexcept
{
    switch (option)
    {
    case BarrierOption::Sy: return "sy";
    case BarrierOption::St: return "st";
    case BarrierOption::Ld: return "ld";
    case BarrierOption::Ish: return "ish";
    case BarrierOption::Ishst: return "ishst";
    case BarrierOption::Ishld: return "ishld";
    case BarrierOption::Nsh: return "nsh";
    case BarrierOption::Nshst: return "nshst";
    case BarrierOption::Nshld: return "nshld";
    case BarrierOption::Osh: return "osh";
    case BarrierOption::Oshst: return "oshst";
    case BarrierOption::Oshld: return "oshld";
    case BarrierOption::Invalid: return "invalid";
    }
    return "invalid";
}

std::string_view system_register_name(SystemRegister reg) noexcept
{
    switch (reg)
    {
    case SystemRegister::None: return "none";
    case SystemRegister::TpidrEl0: return "tpidr_el0";
    case SystemRegister::TpidrroEl0: return "tpidrro_el0";
    }
    return "unknown";
}

} // namespace switchrecomp::aarch64
