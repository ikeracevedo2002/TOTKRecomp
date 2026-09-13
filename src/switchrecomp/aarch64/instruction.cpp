#include "switchrecomp/aarch64/instruction.hpp"

#include <algorithm>
#include <cstddef>

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
    case InstructionId::Udf: return "udf";
    case InstructionId::Nop: return "nop";
    case InstructionId::Add: return "add";
    case InstructionId::Adds: return "adds";
    case InstructionId::Sub: return "sub";
    case InstructionId::Subs: return "subs";
    case InstructionId::Adc: return "adc";
    case InstructionId::Adcs: return "adcs";
    case InstructionId::Sbc: return "sbc";
    case InstructionId::Sbcs: return "sbcs";
    case InstructionId::Ngc: return "ngc";
    case InstructionId::Ngcs: return "ngcs";
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
    case InstructionId::Extr: return "extr";
    case InstructionId::Mul: return "mul";
    case InstructionId::Madd: return "madd";
    case InstructionId::Msub: return "msub";
    case InstructionId::Mneg: return "mneg";
    case InstructionId::Umulh: return "umulh";
    case InstructionId::Smulh: return "smulh";
    case InstructionId::Umaddl: return "umaddl";
    case InstructionId::Umsubl: return "umsubl";
    case InstructionId::Smaddl: return "smaddl";
    case InstructionId::Smsubl: return "smsubl";
    case InstructionId::Umull: return "umull";
    case InstructionId::Smull: return "smull";
    case InstructionId::Udiv: return "udiv";
    case InstructionId::Sdiv: return "sdiv";
    case InstructionId::Crc32: return "crc32";
    case InstructionId::Prfm: return "prfm";
    case InstructionId::Rev: return "rev";
    case InstructionId::Rev16: return "rev16";
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
    case SimdOperation::Movi: return "movi";
    case SimdOperation::Mvni: return "mvni";
    case SimdOperation::Fadd: return "fadd";
    case SimdOperation::Faddp: return "faddp";
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
    case SimdOperation::Fccmp: return "fccmp";
    case SimdOperation::Fccmpe: return "fccmpe";
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
    case SimdOperation::Fcmlt: return "fcmlt";
    case SimdOperation::Fcmle: return "fcmle";
    case SimdOperation::Cmeq: return "cmeq";
    case SimdOperation::Cmgt: return "cmgt";
    case SimdOperation::Cmge: return "cmge";
    case SimdOperation::Cmhi: return "cmhi";
    case SimdOperation::Cmhs: return "cmhs";
    case SimdOperation::Fmla: return "fmla";
    case SimdOperation::Fmls: return "fmls";
    case SimdOperation::Umull: return "umull";
    case SimdOperation::Umull2: return "umull2";
    case SimdOperation::Smull: return "smull";
    case SimdOperation::Smull2: return "smull2";
    case SimdOperation::Umlal: return "umlal";
    case SimdOperation::Umlal2: return "umlal2";
    case SimdOperation::Smlal: return "smlal";
    case SimdOperation::Smlal2: return "smlal2";
    case SimdOperation::Umlsl: return "umlsl";
    case SimdOperation::Umlsl2: return "umlsl2";
    case SimdOperation::Smlsl: return "smlsl";
    case SimdOperation::Smlsl2: return "smlsl2";
    case SimdOperation::Tbl: return "tbl";
    case SimdOperation::Tbx: return "tbx";
    case SimdOperation::Bif: return "bif";
    case SimdOperation::Bit: return "bit";
    case SimdOperation::Bsl: return "bsl";
    case SimdOperation::St1: return "st1";
    case SimdOperation::St2: return "st2";
    case SimdOperation::St3: return "st3";
    case SimdOperation::St4: return "st4";
    case SimdOperation::Ld1: return "ld1";
    case SimdOperation::Ld1r: return "ld1r";
    case SimdOperation::Ld2: return "ld2";
    case SimdOperation::Ld2r: return "ld2r";
    case SimdOperation::Ld3: return "ld3";
    case SimdOperation::Ld3r: return "ld3r";
    case SimdOperation::Ld4: return "ld4";
    case SimdOperation::Ld4r: return "ld4r";
    }
    return "unknown";
}

bool is_scalar_widening_multiply_form_liftable(const DecodedInstruction& instruction) noexcept
{
    if (instruction.id != InstructionId::Umull && instruction.id != InstructionId::Smull) return false;
    if (instruction.operands.size() != 3U) return false;
    for (const auto& operand : instruction.operands)
    {
        if (operand.kind != OperandKind::Register || operand.reg.kind != RegisterKind::General ||
            operand.reg.index >= 32U)
            return false;
    }
    return instruction.operands[0].reg.width == RegisterWidth::X64 &&
           instruction.operands[1].reg.width == RegisterWidth::W32 &&
           instruction.operands[2].reg.width == RegisterWidth::W32;
}

bool is_simd_widening_multiply_form_liftable(const DecodedInstruction& instruction) noexcept
{
    using Op = SimdOperation;
    const auto op = instruction.simd_operation;
    const bool supported = op == Op::Umull || op == Op::Umull2 || op == Op::Smull || op == Op::Smull2 ||
                           op == Op::Umlal || op == Op::Umlal2 || op == Op::Smlal || op == Op::Smlal2 ||
                           op == Op::Umlsl || op == Op::Umlsl2 || op == Op::Smlsl || op == Op::Smlsl2;
    if (!supported || instruction.operands.size() != 3U) return false;
    for (const auto& operand : instruction.operands)
    {
        if (operand.kind != OperandKind::Register || operand.reg.kind != RegisterKind::Vector ||
            operand.reg.index >= 32U)
            return false;
    }
    const auto destination_bits = vector_element_bits(instruction.operands[0].arrangement);
    const auto source_bits = vector_element_bits(instruction.operands[1].arrangement);
    const auto destination_lanes = vector_lane_count(instruction.operands[0].arrangement);
    const auto source_lanes = vector_lane_count(instruction.operands[1].arrangement);
    if (destination_bits == 0U || source_bits == 0U || destination_bits != source_bits * 2U ||
        destination_lanes == 0U || source_lanes == 0U ||
        instruction.operands[1].arrangement != instruction.operands[2].arrangement)
        return false;
    const bool upper = op == Op::Umull2 || op == Op::Smull2 || op == Op::Umlal2 ||
                       op == Op::Smlal2 || op == Op::Umlsl2 || op == Op::Smlsl2;
    return (!upper && source_lanes >= destination_lanes) ||
           (upper && source_lanes >= static_cast<std::uint8_t>(destination_lanes * 2U));
}

bool is_table_lookup_form_liftable(const DecodedInstruction& instruction) noexcept
{
    using Op = SimdOperation;
    if (instruction.simd_operation != Op::Tbl && instruction.simd_operation != Op::Tbx) return false;
    if (instruction.operands.size() < 3U || instruction.operands.size() > 6U) return false;
    const auto& destination = instruction.operands[0];
    if (destination.kind != OperandKind::Register || destination.reg.kind != RegisterKind::Vector ||
        destination.reg.index >= 32U ||
        (destination.arrangement != VectorArrangement::B8 &&
         destination.arrangement != VectorArrangement::B16))
        return false;
    const auto table_count = instruction.operands.size() - 2U;
    const auto& index = instruction.operands[1U + table_count];
    if (index.kind != OperandKind::Register || index.reg.kind != RegisterKind::Vector ||
        index.reg.index >= 32U || index.arrangement != destination.arrangement)
        return false;
    const auto& first_table = instruction.operands[1];
    if (first_table.kind != OperandKind::Register || first_table.reg.kind != RegisterKind::Vector ||
        first_table.arrangement != VectorArrangement::B16)
        return false;
    for (std::size_t table = 0U; table < table_count; ++table)
    {
        const auto& operand = instruction.operands[1U + table];
        if (operand.kind != OperandKind::Register || operand.reg.kind != RegisterKind::Vector ||
            operand.reg.index >= 32U || operand.arrangement != VectorArrangement::B16 ||
            operand.reg.index != static_cast<std::uint8_t>((first_table.reg.index + table) % 32U))
            return false;
    }
    return true;
}

bool is_structure_memory_form_liftable(const DecodedInstruction& instruction) noexcept
{
    using Op = SimdOperation;
    const auto op = instruction.simd_operation;
    const bool store = op == Op::St1 || op == Op::St2 || op == Op::St3 || op == Op::St4;
    const bool load = op == Op::Ld1 || op == Op::Ld1r || op == Op::Ld2 || op == Op::Ld2r ||
                      op == Op::Ld3 || op == Op::Ld3r || op == Op::Ld4 || op == Op::Ld4r;
    if (!store && !load) return false;

    const auto vector_register = [](const Operand& operand) {
        return operand.kind == OperandKind::Register && operand.reg.kind == RegisterKind::Vector &&
               operand.reg.index < 32U;
    };
    const auto fixed_count = [&]() -> std::size_t {
        switch (op)
        {
        case Op::St2: case Op::Ld2: case Op::Ld2r: return 2U;
        case Op::St3: case Op::Ld3: case Op::Ld3r: return 3U;
        case Op::St4: case Op::Ld4: case Op::Ld4r: return 4U;
        case Op::Ld1r: return 1U;
        default: return 0U;
        }
    };
    const auto memory = std::find_if(
        instruction.operands.begin(), instruction.operands.end(),
        [](const Operand& operand) { return operand.kind == OperandKind::Memory; });
    if (memory == instruction.operands.end()) return false;
    const auto register_count = static_cast<std::size_t>(memory - instruction.operands.begin());
    const bool variable_count = op == Op::St1 || op == Op::Ld1;
    if ((variable_count && (register_count < 1U || register_count > 4U)) ||
        (!variable_count && register_count != fixed_count()))
        return false;
    const bool register_post_index = instruction.operands.size() == register_count + 2U;
    if (instruction.operands.size() != register_count + 1U && !register_post_index) return false;
    if (register_post_index &&
        (memory->memory.addressing != MemoryAddressingMode::PostIndex ||
         instruction.operands.back().kind != OperandKind::Register ||
         instruction.operands.back().reg.kind != RegisterKind::General ||
         instruction.operands.back().reg.width != RegisterWidth::X64))
        return false;
    if (memory->memory.base.kind != RegisterKind::General ||
        memory->memory.base.width != RegisterWidth::X64 ||
        (memory->memory.addressing != MemoryAddressingMode::Base &&
         memory->memory.addressing != MemoryAddressingMode::PreIndex &&
         memory->memory.addressing != MemoryAddressingMode::PostIndex))
        return false;
    if (!vector_register(instruction.operands[0]) ||
        instruction.operands[0].arrangement == VectorArrangement::Invalid)
        return false;

    const auto arrangement = instruction.operands[0].arrangement;
    const auto lanes = vector_lane_count(arrangement);
    const auto bits = vector_element_bits(arrangement);
    if (lanes == 0U || bits == 0U || bits % 8U != 0U) return false;
    const bool lane_form = op == Op::St1 || op == Op::Ld1;
    const bool lane = lane_form && instruction.operands[0].vector_index >= 0;
    if (lane && (register_count != 1U ||
                 static_cast<std::uint8_t>(instruction.operands[0].vector_index) >= lanes))
        return false;
    if (!lane && (op == Op::St2 || op == Op::St3 || op == Op::St4 ||
                  op == Op::Ld2 || op == Op::Ld2r || op == Op::Ld3 || op == Op::Ld3r ||
                  op == Op::Ld4 || op == Op::Ld4r) && arrangement == VectorArrangement::D1)
        return false;
    const bool replicate = op == Op::Ld1r || op == Op::Ld2r || op == Op::Ld3r || op == Op::Ld4r;
    if (replicate && register_count != fixed_count()) return false;
    for (std::size_t index = 0U; index < register_count; ++index)
    {
        const auto& operand = instruction.operands[index];
        if (!vector_register(operand) || operand.arrangement != arrangement ||
            operand.reg.index != static_cast<std::uint8_t>((instruction.operands[0].reg.index + index) % 32U))
            return false;
    }
    return true;
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
