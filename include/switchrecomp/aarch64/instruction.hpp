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
    None, Fmov, Movi, Mvni, Fadd, Faddp, Fsub, Fmul, Fdiv, Fneg, Fabs, Fsqrt, Fmin, Fmax, Fcmp, Fcmpe,
    Fccmp, Fccmpe,
    Fcsel, Scvtf, Ucvtf, Fcvtzs, Fcvtzu, Fcvt, Frintn, Frintp, Frintm, Frintz,
    Fmadd, Fmsub, Fnmadd, Fnmsub, Dup, Ins, Umov, Smov, Ext, Zip1, Zip2, Uzp1, Uzp2,
    Trn1, Trn2, Fcmeq, Fcmgt, Fcmge, Fcmlt, Fcmle, Cmeq, Cmgt, Cmge, Cmhi, Cmhs,
    Fmla, Fmls, Umull, Umull2, Smull, Smull2, Umlal, Umlal2, Smlal, Smlal2,
    Umlsl, Umlsl2, Smlsl, Smlsl2, Tbl, Tbx,
    Bif, Bit, Bsl, St1, St2, St3, St4, Ld1, Ld1r, Ld2, Ld2r, Ld3, Ld3r, Ld4, Ld4r,
};

enum class AtomicMemoryOrder : std::uint8_t { Relaxed, Acquire, Release };
enum class BarrierKind : std::uint8_t { Dmb, Dsb, Isb };
enum class BarrierOption : std::uint8_t {
    Sy, St, Ld, Ish, Ishst, Ishld, Nsh, Nshst, Nshld, Osh, Oshst, Oshld, Invalid
};
enum class SystemRegister : std::uint8_t { None, TpidrEl0, TpidrroEl0 };

enum class InstructionId : std::uint16_t
{
    Unknown,
    Udf,
    Nop,
    Add, Adds, Sub, Subs, Adc, Adcs, Sbc, Sbcs, Ngc, Ngcs,
    And, Ands, Orr, Orn, Eor, Eon, Bic, Bics,
    Mov, Mvn, Cmp, Cmn, Ccmp, Ccmn, Tst, Neg, Negs,
    Csel, Csinc, Csinv, Csneg, Cset, Csetm, Cinc, Cinv, Cneg,
    Movz, Movk, Movn, Lsl, Lsr, Asr, Ror, Ubfm, Sbfm, Bfm, Extr,
    Mul, Madd, Msub, Mneg, Umulh, Smulh, Umaddl, Umsubl, Smaddl, Smsubl, Umull, Smull,
    Udiv, Sdiv, Crc32, Prfm, Rev, Rev16, Adr, Adrp,
    Ldr, Ldrb, Ldrh, Ldrsb, Ldrsh, Ldrsw, Str, Strb, Strh,
    Ldp, Stp, Ldur, Stur, LdrLiteral,
    B, Bl, BCond, Br, Blr, Ret, Cbz, Cbnz, Tbz, Tbnz,
    FpSimd,
    Ldxr, Ldxrb, Ldxrh, Stxr, Stxrb, Stxrh,
    Ldaxr, Ldaxrb, Ldaxrh, Stlxr, Stlxrb, Stlxrh,
    Ldar, Ldarb, Ldarh, Stlr, Stlrb, Stlrh,
    Ldxp, Ldaxp, Stxp, Stlxp,
    Clrex,
    Dmb, Dsb, Isb,
    Mrs, Msr,
    Svc, Brk, Hlt, Hvc, Smc, Eret,
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
    AtomicMemoryOrder memory_order = AtomicMemoryOrder::Relaxed;
    std::uint8_t atomic_width = 0U;
    std::optional<Register> exclusive_status_register;
    std::uint8_t crc_width = 0U;
    bool crc32c = false;
    BarrierKind barrier_kind = BarrierKind::Dmb;
    BarrierOption barrier_option = BarrierOption::Sy;
    SystemRegister system_register = SystemRegister::None;
};

[[nodiscard]] std::string_view instruction_id_name(InstructionId id) noexcept;
[[nodiscard]] std::string_view control_flow_kind_name(ControlFlowKind kind) noexcept;
[[nodiscard]] std::string_view simd_operation_name(SimdOperation operation) noexcept;
[[nodiscard]] bool is_structure_memory_form_liftable(const DecodedInstruction& instruction) noexcept;
[[nodiscard]] bool is_scalar_widening_multiply_form_liftable(const DecodedInstruction& instruction) noexcept;
[[nodiscard]] bool is_simd_widening_multiply_form_liftable(const DecodedInstruction& instruction) noexcept;
[[nodiscard]] bool is_table_lookup_form_liftable(const DecodedInstruction& instruction) noexcept;
[[nodiscard]] std::string_view barrier_option_name(BarrierOption option) noexcept;
[[nodiscard]] std::string_view system_register_name(SystemRegister reg) noexcept;

} // namespace switchrecomp::aarch64
