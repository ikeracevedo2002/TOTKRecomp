#include "switchrecomp/aarch64/decoder.hpp"

#include "switchrecomp/common/checked_arithmetic.hpp"

#include <capstone/arm64.h>
#include <capstone/capstone.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <new>
#include <sstream>
#include <string>
#include <utility>

namespace switchrecomp::aarch64
{

namespace
{

[[nodiscard]] std::string hex_value(std::uint64_t value)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::setw(16) << std::setfill('0') << value;
    return output.str();
}

[[nodiscard]] std::int64_t sign_extend(std::uint64_t value, unsigned int bit_count) noexcept
{
    const std::uint64_t sign_bit = std::uint64_t{1} << (bit_count - 1U);
    const std::uint64_t mask = (std::uint64_t{1} << bit_count) - 1U;
    value &= mask;
    const auto signed_value = static_cast<std::int64_t>(value);
    return (value & sign_bit) != 0U
               ? signed_value - static_cast<std::int64_t>(std::uint64_t{1} << bit_count)
               : signed_value;
}

[[nodiscard]] Result<std::int64_t> scaled_signed(std::int64_t value, std::uint64_t scale)
{
    if (value >= 0)
    {
        const auto magnitude = static_cast<std::uint64_t>(value);
        if (magnitude > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) /
                           scale)
        {
            return Result<std::int64_t>::failure(
                make_error(ErrorCode::ArithmeticOverflow, "scaled AArch64 immediate overflows"));
        }
        return Result<std::int64_t>::success(
            static_cast<std::int64_t>(magnitude * scale));
    }

    const auto magnitude = std::uint64_t{0} - static_cast<std::uint64_t>(value);
    if (magnitude > (static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U) /
                        scale)
    {
        return Result<std::int64_t>::failure(
            make_error(ErrorCode::ArithmeticUnderflow, "scaled AArch64 immediate underflows"));
    }
    const auto scaled = magnitude * scale;
    if (scaled == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U)
    {
        return Result<std::int64_t>::success(std::numeric_limits<std::int64_t>::min());
    }
    return Result<std::int64_t>::success(-static_cast<std::int64_t>(scaled));
}

[[nodiscard]] Result<GuestAddress> relative_target(GuestAddress address, std::int64_t immediate,
                                                    std::uint64_t scale)
{
    const auto offset = scaled_signed(immediate, scale);
    if (!offset)
    {
        return Result<GuestAddress>::failure(offset.error());
    }
    const auto target = checked_add_signed_u64(address, offset.value());
    if (!target)
    {
        return Result<GuestAddress>::failure(target.error());
    }
    return target;
}

[[nodiscard]] Result<GuestAddress> page_relative_target(GuestAddress address,
                                                         std::int64_t page_immediate)
{
    const auto page = address & ~GuestAddress{0xfffU};
    return relative_target(page, page_immediate, 0x1000U);
}

[[nodiscard]] bool is_b_cond(std::uint32_t opcode) noexcept
{
    return (opcode & 0xff000010U) == 0x54000000U;
}

[[nodiscard]] bool is_b(std::uint32_t opcode) noexcept
{
    return (opcode & 0xfc000000U) == 0x14000000U;
}

[[nodiscard]] bool is_bl(std::uint32_t opcode) noexcept
{
    return (opcode & 0xfc000000U) == 0x94000000U;
}

[[nodiscard]] bool is_cbz(std::uint32_t opcode) noexcept
{
    return (opcode & 0x7f000000U) == 0x34000000U;
}

[[nodiscard]] bool is_cbnz(std::uint32_t opcode) noexcept
{
    return (opcode & 0x7f000000U) == 0x35000000U;
}

[[nodiscard]] bool is_tbz(std::uint32_t opcode) noexcept
{
    return (opcode & 0x7f000000U) == 0x36000000U;
}

[[nodiscard]] bool is_tbnz(std::uint32_t opcode) noexcept
{
    return (opcode & 0x7f000000U) == 0x37000000U;
}

[[nodiscard]] bool is_br(std::uint32_t opcode) noexcept
{
    return (opcode & 0xfffffc1fU) == 0xd61f0000U;
}

[[nodiscard]] bool is_blr(std::uint32_t opcode) noexcept
{
    return (opcode & 0xfffffc1fU) == 0xd63f0000U;
}

[[nodiscard]] bool is_ret(std::uint32_t opcode) noexcept
{
    return (opcode & 0xfffffc1fU) == 0xd65f0000U;
}

[[nodiscard]] Register from_capstone_register(arm64_reg value)
{
    if (value == ARM64_REG_INVALID)
    {
        return Register{};
    }
    const auto raw = static_cast<int>(value);
    const auto in_range = [raw](arm64_reg first, arm64_reg last) {
        return raw >= static_cast<int>(first) && raw <= static_cast<int>(last);
    };

    if (value == ARM64_REG_SP || value == ARM64_REG_WSP)
    {
        return Register{RegisterKind::General,
                        value == ARM64_REG_WSP ? RegisterWidth::W32 : RegisterWidth::X64,
                        31U,
                        true,
                        false};
    }
    if (value == ARM64_REG_XZR || value == ARM64_REG_WZR)
    {
        return Register{RegisterKind::General,
                        value == ARM64_REG_WZR ? RegisterWidth::W32 : RegisterWidth::X64,
                        31U,
                        false,
                        true};
    }
    if (value == ARM64_REG_FP)
    {
        return Register{RegisterKind::General, RegisterWidth::X64, 29U, false, false};
    }
    if (value == ARM64_REG_LR)
    {
        return Register{RegisterKind::General, RegisterWidth::X64, 30U, false, false};
    }
    if (raw >= static_cast<int>(ARM64_REG_X0) &&
        raw <= static_cast<int>(ARM64_REG_X0) + 28)
    {
        return Register{RegisterKind::General,
                        RegisterWidth::X64,
                        static_cast<std::uint8_t>(raw - static_cast<int>(ARM64_REG_X0)),
                        false,
                        false};
    }
    if (in_range(ARM64_REG_W0, ARM64_REG_W30))
    {
        return Register{RegisterKind::General,
                        RegisterWidth::W32,
                        static_cast<std::uint8_t>(raw - static_cast<int>(ARM64_REG_W0)),
                        false,
                        false};
    }
    if (in_range(ARM64_REG_B0, ARM64_REG_B31))
    {
        return Register{RegisterKind::Vector,
                        RegisterWidth::B8,
                        static_cast<std::uint8_t>(raw - static_cast<int>(ARM64_REG_B0)),
                        false,
                        false};
    }
    if (in_range(ARM64_REG_H0, ARM64_REG_H31))
    {
        return Register{RegisterKind::Vector,
                        RegisterWidth::H16,
                        static_cast<std::uint8_t>(raw - static_cast<int>(ARM64_REG_H0)),
                        false,
                        false};
    }
    if (in_range(ARM64_REG_S0, ARM64_REG_S31))
    {
        return Register{RegisterKind::Vector,
                        RegisterWidth::S32,
                        static_cast<std::uint8_t>(raw - static_cast<int>(ARM64_REG_S0)),
                        false,
                        false};
    }
    if (in_range(ARM64_REG_D0, ARM64_REG_D31))
    {
        return Register{RegisterKind::Vector,
                        RegisterWidth::D64,
                        static_cast<std::uint8_t>(raw - static_cast<int>(ARM64_REG_D0)),
                        false,
                        false};
    }
    if (in_range(ARM64_REG_Q0, ARM64_REG_Q31))
    {
        return Register{RegisterKind::Vector,
                        RegisterWidth::Q128,
                        static_cast<std::uint8_t>(raw - static_cast<int>(ARM64_REG_Q0)),
                        false,
                        false};
    }
    if (in_range(ARM64_REG_V0, ARM64_REG_V31))
    {
        return Register{RegisterKind::Vector,
                        RegisterWidth::Q128,
                        static_cast<std::uint8_t>(raw - static_cast<int>(ARM64_REG_V0)),
                        false,
                        false};
    }
    return Register{RegisterKind::System, RegisterWidth::None, 0U, false, false};
}

[[nodiscard]] ConditionCode from_capstone_condition(arm64_cc value)
{
    switch (value)
    {
    case ARM64_CC_EQ:
        return ConditionCode::Eq;
    case ARM64_CC_NE:
        return ConditionCode::Ne;
    case ARM64_CC_HS:
        return ConditionCode::Cs;
    case ARM64_CC_LO:
        return ConditionCode::Cc;
    case ARM64_CC_MI:
        return ConditionCode::Mi;
    case ARM64_CC_PL:
        return ConditionCode::Pl;
    case ARM64_CC_VS:
        return ConditionCode::Vs;
    case ARM64_CC_VC:
        return ConditionCode::Vc;
    case ARM64_CC_HI:
        return ConditionCode::Hi;
    case ARM64_CC_LS:
        return ConditionCode::Ls;
    case ARM64_CC_GE:
        return ConditionCode::Ge;
    case ARM64_CC_LT:
        return ConditionCode::Lt;
    case ARM64_CC_GT:
        return ConditionCode::Gt;
    case ARM64_CC_LE:
        return ConditionCode::Le;
    case ARM64_CC_AL:
        return ConditionCode::Al;
    case ARM64_CC_NV:
        return ConditionCode::Nv;
    case ARM64_CC_INVALID:
        break;
    }
    return ConditionCode::Al;
}

[[nodiscard]] InstructionId normalize_id(const cs_insn& instruction)
{
    const auto opcode = static_cast<std::uint32_t>(instruction.bytes[0]) |
                        (static_cast<std::uint32_t>(instruction.bytes[1]) << 8U) |
                        (static_cast<std::uint32_t>(instruction.bytes[2]) << 16U) |
                        (static_cast<std::uint32_t>(instruction.bytes[3]) << 24U);
    if (is_b_cond(opcode))
    {
        return InstructionId::BCond;
    }
    if (is_b(opcode))
    {
        return InstructionId::B;
    }
    if (is_bl(opcode))
    {
        return InstructionId::Bl;
    }
    if (is_br(opcode))
    {
        return InstructionId::Br;
    }
    if (is_blr(opcode))
    {
        return InstructionId::Blr;
    }
    if (is_ret(opcode))
    {
        return InstructionId::Ret;
    }
    if (is_cbz(opcode))
    {
        return InstructionId::Cbz;
    }
    if (is_cbnz(opcode))
    {
        return InstructionId::Cbnz;
    }
    if (is_tbz(opcode))
    {
        return InstructionId::Tbz;
    }
    if (is_tbnz(opcode))
    {
        return InstructionId::Tbnz;
    }

    switch (instruction.id)
    {
    case ARM64_INS_NOP:
        return InstructionId::Nop;
    case ARM64_INS_ADD:
        return InstructionId::Add;
    case ARM64_INS_ADDS:
        return InstructionId::Adds;
    case ARM64_INS_SUB:
        return InstructionId::Sub;
    case ARM64_INS_SUBS:
        return InstructionId::Subs;
    case ARM64_INS_AND:
        return InstructionId::And;
    case ARM64_INS_ORR:
        return InstructionId::Orr;
    case ARM64_INS_EOR:
        return InstructionId::Eor;
    case ARM64_INS_MOV:
        return InstructionId::Mov;
    case ARM64_INS_CMP:
        return InstructionId::Cmp;
    case ARM64_INS_CSEL:
        return InstructionId::Csel;
    case ARM64_INS_MOVZ:
        return InstructionId::Movz;
    case ARM64_INS_MOVK:
        return InstructionId::Movk;
    case ARM64_INS_ADR:
        return InstructionId::Adr;
    case ARM64_INS_ADRP:
        return InstructionId::Adrp;
    case ARM64_INS_LDR:
        return (opcode & 0x3b000000U) == 0x18000000U ? InstructionId::LdrLiteral
                                                     : InstructionId::Ldr;
    case ARM64_INS_STR:
        return InstructionId::Str;
    case ARM64_INS_LDP:
        return InstructionId::Ldp;
    case ARM64_INS_STP:
        return InstructionId::Stp;
    case ARM64_INS_LDUR:
        return InstructionId::Ldur;
    case ARM64_INS_STUR:
        return InstructionId::Stur;
    case ARM64_INS_LDXR:
        return InstructionId::Ldxr;
    case ARM64_INS_STXR:
        return InstructionId::Stxr;
    case ARM64_INS_LDAXR:
        return InstructionId::Ldaxr;
    case ARM64_INS_STLXR:
        return InstructionId::Stlxr;
    case ARM64_INS_DMB:
        return InstructionId::Dmb;
    case ARM64_INS_DSB:
        return InstructionId::Dsb;
    case ARM64_INS_ISB:
        return InstructionId::Isb;
    case ARM64_INS_SVC:
        return InstructionId::Svc;
    case ARM64_INS_BRK:
        return InstructionId::Brk;
    case ARM64_INS_HLT:
        return InstructionId::Hlt;
    case ARM64_INS_HVC:
        return InstructionId::Hvc;
    case ARM64_INS_SMC:
        return InstructionId::Smc;
    case ARM64_INS_ERET:
    case ARM64_INS_ERETAA:
    case ARM64_INS_ERETAB:
        return InstructionId::Eret;
    default:
        break;
    }
    return InstructionId::Unknown;
}

[[nodiscard]] bool is_control_flow_group(csh handle, const cs_insn& instruction)
{
    return cs_insn_group(handle, &instruction, ARM64_GRP_JUMP) != 0U ||
           cs_insn_group(handle, &instruction, ARM64_GRP_CALL) != 0U ||
           cs_insn_group(handle, &instruction, ARM64_GRP_RET) != 0U;
}

[[nodiscard]] Operand normalize_operand(const cs_arm64_op& operand, const cs_arm64& detail)
{
    Operand result;
    switch (operand.type)
    {
    case ARM64_OP_REG:
        result.kind = OperandKind::Register;
        result.reg = from_capstone_register(operand.reg);
        break;
    case ARM64_OP_IMM:
    case ARM64_OP_CIMM:
        result.kind = OperandKind::Immediate;
        result.immediate = operand.imm;
        break;
    case ARM64_OP_MEM:
        result.kind = OperandKind::Memory;
        result.memory.base = from_capstone_register(operand.mem.base);
        result.memory.index = from_capstone_register(operand.mem.index);
        result.memory.displacement = operand.mem.disp;
        result.memory.shift = static_cast<std::uint8_t>(operand.shift.value);
        result.memory.writeback = detail.writeback;
        result.memory.addressing = detail.writeback
                                       ? (detail.post_index ? MemoryAddressingMode::PostIndex
                                                            : MemoryAddressingMode::PreIndex)
                                       : (operand.mem.base == ARM64_REG_INVALID
                                              ? MemoryAddressingMode::Literal
                                              : (operand.mem.index != ARM64_REG_INVALID
                                                     ? MemoryAddressingMode::RegisterOffset
                                                     : MemoryAddressingMode::Base));
        break;
    case ARM64_OP_BARRIER:
    case ARM64_OP_SYS:
    case ARM64_OP_PSTATE:
    case ARM64_OP_REG_MRS:
    case ARM64_OP_REG_MSR:
        result.kind = OperandKind::System;
        result.text = "system";
        break;
    default:
        result.kind = OperandKind::Other;
        result.text = "unsupported";
        break;
    }
    return result;
}

[[nodiscard]] Result<ControlFlowInfo> classify_control_flow(const cs_insn& instruction,
                                                              InstructionId id,
                                                              GuestAddress address,
                                                              std::uint32_t opcode,
                                                              csh handle)
{
    ControlFlowInfo result;
    result.kind = ControlFlowKind::Fallthrough;
    result.has_fallthrough = true;

    const auto direct = [address](std::int64_t immediate, std::uint64_t scale) {
        return relative_target(address, immediate, scale);
    };
    if (id == InstructionId::B || id == InstructionId::Bl)
    {
        const auto immediate = sign_extend((opcode >> 0U) & 0x03ffffffU, 26U);
        const auto target = direct(immediate, 4U);
        if (!target)
        {
            return Result<ControlFlowInfo>::failure(make_error(
                target.error().code,
                "failed to calculate branch target from " + hex_value(address) + ": " +
                    target.error().message));
        }
        result.target = target.value();
        result.kind = id == InstructionId::Bl ? ControlFlowKind::DirectCall
                                              : ControlFlowKind::DirectBranch;
        result.has_fallthrough = id == InstructionId::Bl;
        return Result<ControlFlowInfo>::success(result);
    }
    if (id == InstructionId::BCond)
    {
        const auto immediate = sign_extend((opcode >> 5U) & 0x7ffffU, 19U);
        const auto target = direct(immediate, 4U);
        if (!target)
        {
            return Result<ControlFlowInfo>::failure(make_error(
                target.error().code,
                "failed to calculate conditional branch target from " + hex_value(address) +
                    ": " + target.error().message));
        }
        result.target = target.value();
        result.kind = ControlFlowKind::ConditionalBranch;
        result.condition = from_capstone_condition(instruction.detail->arm64.cc);
        return Result<ControlFlowInfo>::success(result);
    }
    if (id == InstructionId::Cbz || id == InstructionId::Cbnz)
    {
        const auto immediate = sign_extend((opcode >> 5U) & 0x7ffffU, 19U);
        const auto target = direct(immediate, 4U);
        if (!target)
        {
            return Result<ControlFlowInfo>::failure(make_error(
                target.error().code,
                "failed to calculate compare-and-branch target from " + hex_value(address) +
                    ": " + target.error().message));
        }
        result.target = target.value();
        result.kind = ControlFlowKind::ConditionalBranch;
        return Result<ControlFlowInfo>::success(result);
    }
    if (id == InstructionId::Tbz || id == InstructionId::Tbnz)
    {
        const auto immediate = sign_extend((opcode >> 5U) & 0x3fffU, 14U);
        const auto target = direct(immediate, 4U);
        if (!target)
        {
            return Result<ControlFlowInfo>::failure(make_error(
                target.error().code,
                "failed to calculate test-and-branch target from " + hex_value(address) +
                    ": " + target.error().message));
        }
        result.target = target.value();
        result.kind = ControlFlowKind::ConditionalBranch;
        return Result<ControlFlowInfo>::success(result);
    }
    if (id == InstructionId::Br || id == InstructionId::Blr)
    {
        result.kind = id == InstructionId::Blr ? ControlFlowKind::IndirectCall
                                               : ControlFlowKind::IndirectBranch;
        result.has_fallthrough = id == InstructionId::Blr;
        if (instruction.detail->arm64.op_count > 0U &&
            instruction.detail->arm64.operands[0].type == ARM64_OP_REG)
        {
            result.register_target =
                from_capstone_register(instruction.detail->arm64.operands[0].reg);
        }
        return Result<ControlFlowInfo>::success(result);
    }
    if (id == InstructionId::Ret)
    {
        result.kind = ControlFlowKind::Return;
        result.has_fallthrough = false;
        if (instruction.detail->arm64.op_count > 0U &&
            instruction.detail->arm64.operands[0].type == ARM64_OP_REG)
        {
            result.return_register =
                from_capstone_register(instruction.detail->arm64.operands[0].reg);
        }
        else
        {
            result.return_register = Register{RegisterKind::General,
                                              RegisterWidth::X64,
                                              30U,
                                              false,
                                              false};
        }
        return Result<ControlFlowInfo>::success(result);
    }
    if (id == InstructionId::Adr || id == InstructionId::Adrp)
    {
        const auto immediate = sign_extend((((opcode >> 5U) & 0x7ffffU) << 2U) |
                                               ((opcode >> 29U) & 0x3U),
                                           21U);
        const auto target = id == InstructionId::Adrp
                                ? page_relative_target(address, immediate)
                                : relative_target(address, immediate, 1U);
        if (!target)
        {
            return Result<ControlFlowInfo>::failure(make_error(
                target.error().code,
                "failed to calculate PC-relative value from " + hex_value(address) + ": " +
                    target.error().message));
        }
        return Result<ControlFlowInfo>::success(result);
    }
    if (id == InstructionId::LdrLiteral)
    {
        const auto immediate = sign_extend((opcode >> 5U) & 0x7ffffU, 19U);
        const auto target = relative_target(address, immediate, 4U);
        if (!target)
        {
            return Result<ControlFlowInfo>::failure(make_error(
                target.error().code,
                "failed to calculate literal-load target from " + hex_value(address) + ": " +
                    target.error().message));
        }
        return Result<ControlFlowInfo>::success(result);
    }
    if (instruction.id == ARM64_INS_UDF || id == InstructionId::Brk ||
        id == InstructionId::Hlt)
    {
        result.kind = ControlFlowKind::Trap;
        result.has_fallthrough = false;
        return Result<ControlFlowInfo>::success(result);
    }
    if (id == InstructionId::Svc || id == InstructionId::Hvc || id == InstructionId::Smc ||
        id == InstructionId::Eret)
    {
        result.kind = ControlFlowKind::Exception;
        result.has_fallthrough = false;
        return Result<ControlFlowInfo>::success(result);
    }

    if (is_control_flow_group(handle, instruction))
    {
        result.kind = ControlFlowKind::Unknown;
        result.has_fallthrough = false;
    }
    return Result<ControlFlowInfo>::success(result);
}

} // namespace

struct AArch64Decoder::Impl
{
    csh handle = 0U;

    ~Impl()
    {
        if (handle != 0U)
        {
            cs_close(&handle);
        }
    }
};

AArch64Decoder::AArch64Decoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
AArch64Decoder::AArch64Decoder(AArch64Decoder&&) noexcept = default;
AArch64Decoder& AArch64Decoder::operator=(AArch64Decoder&&) noexcept = default;
AArch64Decoder::~AArch64Decoder() = default;

Result<std::unique_ptr<AArch64Decoder>> AArch64Decoder::create()
{
    try
    {
        auto impl = std::make_unique<Impl>();
        const auto error = cs_open(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN, &impl->handle);
        if (error != CS_ERR_OK)
        {
            return Result<std::unique_ptr<AArch64Decoder>>::failure(make_error(
                ErrorCode::DecodeFailed,
                "Capstone ARM64 initialization failed: " +
                    std::string(cs_strerror(error))));
        }
        if (cs_option(impl->handle, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK)
        {
            return Result<std::unique_ptr<AArch64Decoder>>::failure(make_error(
                ErrorCode::DecodeFailed, "Capstone detail mode could not be enabled"));
        }
        return Result<std::unique_ptr<AArch64Decoder>>::success(
            std::unique_ptr<AArch64Decoder>(new AArch64Decoder(std::move(impl))));
    }
    catch (const std::bad_alloc&)
    {
        return Result<std::unique_ptr<AArch64Decoder>>::failure(
            make_error(ErrorCode::ResourceLimit, "unable to allocate the AArch64 decoder"));
    }
}

Result<DecodedInstruction> AArch64Decoder::decode(GuestAddress address,
                                                   std::uint32_t opcode) const
{
    if (impl_ == nullptr || impl_->handle == 0U)
    {
        return Result<DecodedInstruction>::failure(
            make_error(ErrorCode::DecodeFailed, "AArch64 decoder is not initialized"));
    }
    if ((address & 0x3U) != 0U)
    {
        return Result<DecodedInstruction>::failure(make_error(
            ErrorCode::MisalignedInstructionAddress,
            "instruction address " + hex_value(address) + " is not 4-byte aligned"));
    }

    const std::array<std::uint8_t, 4> bytes{
        static_cast<std::uint8_t>(opcode & 0xffU),
        static_cast<std::uint8_t>((opcode >> 8U) & 0xffU),
        static_cast<std::uint8_t>((opcode >> 16U) & 0xffU),
        static_cast<std::uint8_t>((opcode >> 24U) & 0xffU)};
    cs_insn* instructions = nullptr;
    const auto count = cs_disasm(impl_->handle, bytes.data(), bytes.size(), address, 1U,
                                 &instructions);
    if (count != 1U || instructions == nullptr)
    {
        if (instructions != nullptr)
        {
            cs_free(instructions, count);
        }
        return Result<DecodedInstruction>::failure(make_error(
            ErrorCode::DecodeFailed,
            "Capstone could not decode opcode " + hex_value(opcode) + " at " +
                hex_value(address)));
    }

    const cs_insn& instruction = instructions[0];
    if (instruction.size != 4U || instruction.detail == nullptr)
    {
        cs_free(instructions, count);
        return Result<DecodedInstruction>::failure(make_error(
            ErrorCode::DecodeFailed,
            "AArch64 backend returned an incomplete instruction at " + hex_value(address)));
    }

    DecodedInstruction result;
    result.address = address;
    result.opcode = opcode;
    result.id = normalize_id(instruction);
    if (result.id == InstructionId::Unknown &&
        (cs_insn_group(impl_->handle, &instruction, ARM64_GRP_NEON) != 0U ||
         cs_insn_group(impl_->handle, &instruction, ARM64_GRP_FPARMV8) != 0U ||
         cs_insn_group(impl_->handle, &instruction, ARM64_GRP_CRYPTO) != 0U))
    {
        result.id = InstructionId::FpSimd;
    }
    result.backend_decoded = true;
    result.normalized = result.id != InstructionId::Unknown;
    result.disassembly = instruction.mnemonic;
    if (instruction.op_str[0] != '\0')
    {
        result.disassembly += " ";
        result.disassembly += instruction.op_str;
    }
    const auto& detail = instruction.detail->arm64;
    result.operands.reserve(detail.op_count);
    for (std::uint8_t index = 0U; index < detail.op_count; ++index)
    {
        result.operands.push_back(normalize_operand(detail.operands[index], detail));
    }
    const auto control_flow =
        classify_control_flow(instruction, result.id, address, opcode, impl_->handle);
    if (!control_flow)
    {
        cs_free(instructions, count);
        return Result<DecodedInstruction>::failure(control_flow.error());
    }
    result.control_flow = control_flow.value();
    // The PC-relative value is stored on the instruction, not in the flow object. Keep this
    // assignment explicit so the public model remains independent of backend lifetime.
    if (result.id == InstructionId::Adr || result.id == InstructionId::Adrp)
    {
        const auto immediate = sign_extend((((opcode >> 5U) & 0x7ffffU) << 2U) |
                                               ((opcode >> 29U) & 0x3U),
                                           21U);
        const auto value = result.id == InstructionId::Adrp
                               ? page_relative_target(address, immediate)
                               : relative_target(address, immediate, 1U);
        if (!value)
        {
            cs_free(instructions, count);
            return Result<DecodedInstruction>::failure(value.error());
        }
        result.pc_relative_value = value.value();
    }
    if (result.id == InstructionId::LdrLiteral)
    {
        const auto immediate = sign_extend((opcode >> 5U) & 0x7ffffU, 19U);
        const auto value = relative_target(address, immediate, 4U);
        if (!value)
        {
            cs_free(instructions, count);
            return Result<DecodedInstruction>::failure(value.error());
        }
        result.pc_relative_value = value.value();
    }

    cs_free(instructions, count);
    return Result<DecodedInstruction>::success(std::move(result));
}

Result<std::uint32_t> fetch_instruction(const memory::GuestMemory& memory, GuestAddress address)
{
    if ((address & 0x3U) != 0U)
    {
        return Result<std::uint32_t>::failure(make_error(
            ErrorCode::MisalignedInstructionAddress,
            "instruction fetch address " + hex_value(address) + " is not 4-byte aligned"));
    }
    const auto executable = memory.is_executable(address, 4U);
    if (!executable)
    {
        return Result<std::uint32_t>::failure(executable.error());
    }
    if (!executable.value())
    {
        return Result<std::uint32_t>::failure(make_error(
            ErrorCode::NonExecutableAddress,
            "instruction fetch at " + hex_value(address) + " requires executable memory"));
    }

    std::array<std::byte, 4> bytes{};
    const auto read = memory.read(address, bytes);
    if (!read)
    {
        return Result<std::uint32_t>::failure(make_error(
            ErrorCode::InstructionFetchFailed,
            "instruction fetch at " + hex_value(address) + " failed: " + read.error().message));
    }
    const auto byte = [&bytes](std::size_t index) {
        return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[index]));
    };
    return Result<std::uint32_t>::success(byte(0U) | (byte(1U) << 8U) | (byte(2U) << 16U) |
                                           (byte(3U) << 24U));
}

Result<DecodedInstruction> fetch_and_decode(const memory::GuestMemory& memory,
                                             const AArch64Decoder& decoder,
                                             GuestAddress address)
{
    const auto opcode = fetch_instruction(memory, address);
    if (!opcode)
    {
        return Result<DecodedInstruction>::failure(opcode.error());
    }
    return decoder.decode(address, opcode.value());
}

} // namespace switchrecomp::aarch64
