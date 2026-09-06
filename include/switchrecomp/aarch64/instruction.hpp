#pragma once

#include "switchrecomp/aarch64/operand.hpp"
#include "switchrecomp/memory/guest_memory.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace switchrecomp::aarch64
{

using GuestAddress = memory::GuestAddress;

enum class SimdOperation : std::uint8_t
{
    None, Fmov, Fadd, Fsub, Fmul, Fdiv, Fneg, Fabs, Fsqrt, Fmin, Fmax, Fcmp, Fcmpe,
    Fcsel, Scvtf, Ucvtf, Fcvtzs, Fcvtzu, Fcvt, Frintn, Frintp, Frintm, Frintz,
    Fmadd, Fmsub, Fnmadd, Fnmsub, Dup, Ins, Umov, Smov, Ext, Zip1, Zip2, Uzp1, Uzp2,
    Trn1, Trn2, Fcmeq, Fcmgt, Fcmge, Cmeq, Cmgt, Cmge, Cmhi, Cmhs,
};

enum class InstructionId : std::uint16_t
{
    Unknown,
    Nop,
    Add,
    Adds,
    Sub,
    Subs,
    And,
    Ands,
    Orr,
    Orn,
    Eor,
    Eon,
    Bic,
    Bics,
    Mov,
    Mvn,
    Cmp,
    Cmn,
    Ccmp,
    Ccmn,
    Tst,
    Neg,
    Negs,
    Csel,
    Csinc,
    Csinv,
    Csneg,
    Cset,
    Csetm,
    Cinc,
    Cinv,
    Cneg,
    Movz,
    Movk,
    Movn,
    Lsl,
    Lsr,
    Asr,
    Ror,
    Ubfm,
    Sbfm,
    Bfm,
    Mul,
    Madd,
    Msub,
    Mneg,
    Udiv,
    Sdiv,
    Adr,
    Adrp,
    Ldr,
    Ldrb,
    Ldrh,
    Ldrsb,
    Ldrsh,
    Ldrsw,
    Str,
    Strb,
    Strh,
    Ldp,
    Stp,
    Ldur,
    Stur,
    LdrLiteral,
    B,
    Bl,
    BCond,
    Br,
    Blr,
    Ret,
    Cbz,
    Cbnz,
    Tbz,
    Tbnz,
    FpSimd,
    Ldxr,
    Stxr,
    Ldaxr,
    Stlxr,
    Dmb,
    Dsb,
    Isb,
    Svc,
    Brk,
    Hlt,
    Hvc,
    Smc,
    Eret,
};

enum class ControlFlowKind : std::uint8_t
{
    Fallthrough,
    DirectBranch,
    ConditionalBranch,
    DirectCall,
    IndirectBranch,
    IndirectCall,
    Return,
    Trap,
    Exception,
    Unknown,
};

struct ControlFlowInfo
{
    ControlFlowKind kind = ControlFlowKind::Fallthrough;
    std::optional<GuestAddress> target;
    std::optional<Register> register_target;
    std::optional<Register> return_register;
    std::optional<ConditionCode> condition;
    bool has_fallthrough = true;
};

struct DecodedInstruction
{
    GuestAddress address = 0U;
    std::uint32_t opcode = 0U;
    InstructionId id = InstructionId::Unknown;
    std::vector<Operand> operands;
    ControlFlowInfo control_flow;
    std::optional<GuestAddress> pc_relative_value;
    std::optional<ConditionCode> condition;
    std::string disassembly;
    bool backend_decoded = false;
    bool normalized = false;
    SimdOperation simd_operation = SimdOperation::None;
};

[[nodiscard]] std::string_view instruction_id_name(InstructionId id) noexcept;
[[nodiscard]] std::string_view control_flow_kind_name(ControlFlowKind kind) noexcept;
[[nodiscard]] std::string_view simd_operation_name(SimdOperation operation) noexcept;

} // namespace switchrecomp::aarch64
