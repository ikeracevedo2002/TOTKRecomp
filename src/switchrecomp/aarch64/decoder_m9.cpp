#include "switchrecomp/aarch64/decoder.hpp"

#include <string_view>

namespace switchrecomp::aarch64
{
namespace
{
[[nodiscard]] std::string_view mnemonic(const DecodedInstruction& instruction) noexcept
{
    const std::string_view text(instruction.disassembly);
    const auto space = text.find(' ');
    return text.substr(0U, space);
}

[[nodiscard]] BarrierOption barrier_option(std::uint32_t opcode) noexcept
{
    switch ((opcode >> 8U) & 0xfU)
    {
    case 0xfU: return BarrierOption::Sy;
    case 0xeU: return BarrierOption::St;
    case 0xdU: return BarrierOption::Ld;
    case 0xbU: return BarrierOption::Ish;
    case 0xaU: return BarrierOption::Ishst;
    case 0x9U: return BarrierOption::Ishld;
    case 0x7U: return BarrierOption::Nsh;
    case 0x6U: return BarrierOption::Nshst;
    case 0x5U: return BarrierOption::Nshld;
    case 0x3U: return BarrierOption::Osh;
    case 0x2U: return BarrierOption::Oshst;
    case 0x1U: return BarrierOption::Oshld;
    default: return BarrierOption::Invalid;
    }
}

[[nodiscard]] std::uint8_t register_width_bytes(const Register& reg) noexcept
{
    return reg.width == RegisterWidth::W32 ? 4U : reg.width == RegisterWidth::X64 ? 8U : 0U;
}

void normalize_concurrency(DecodedInstruction& result) noexcept
{
    const auto name = mnemonic(result);
    if (name == "ldxr") result.id = InstructionId::Ldxr;
    else if (name == "ldxrb") result.id = InstructionId::Ldxrb;
    else if (name == "ldxrh") result.id = InstructionId::Ldxrh;
    else if (name == "stxr") result.id = InstructionId::Stxr;
    else if (name == "stxrb") result.id = InstructionId::Stxrb;
    else if (name == "stxrh") result.id = InstructionId::Stxrh;
    else if (name == "ldaxr") result.id = InstructionId::Ldaxr;
    else if (name == "ldaxrb") result.id = InstructionId::Ldaxrb;
    else if (name == "ldaxrh") result.id = InstructionId::Ldaxrh;
    else if (name == "stlxr") result.id = InstructionId::Stlxr;
    else if (name == "stlxrb") result.id = InstructionId::Stlxrb;
    else if (name == "stlxrh") result.id = InstructionId::Stlxrh;
    else if (name == "ldar") result.id = InstructionId::Ldar;
    else if (name == "ldarb") result.id = InstructionId::Ldarb;
    else if (name == "ldarh") result.id = InstructionId::Ldarh;
    else if (name == "stlr") result.id = InstructionId::Stlr;
    else if (name == "stlrb") result.id = InstructionId::Stlrb;
    else if (name == "stlrh") result.id = InstructionId::Stlrh;
    else if (name == "ldxp") result.id = InstructionId::Ldxp;
    else if (name == "ldaxp") result.id = InstructionId::Ldaxp;
    else if (name == "stxp") result.id = InstructionId::Stxp;
    else if (name == "stlxp") result.id = InstructionId::Stlxp;
    else if (name == "clrex") result.id = InstructionId::Clrex;
    else if (name == "dmb") result.id = InstructionId::Dmb;
    else if (name == "dsb") result.id = InstructionId::Dsb;
    else if (name == "isb") result.id = InstructionId::Isb;
    else if (name == "mrs") result.id = InstructionId::Mrs;
    else if (name == "msr") result.id = InstructionId::Msr;
    else return;

    result.normalized = true;
    switch (result.id)
    {
    case InstructionId::Ldxrb: case InstructionId::Ldaxrb: case InstructionId::Ldarb:
    case InstructionId::Stxrb: case InstructionId::Stlxrb: case InstructionId::Stlrb:
        result.atomic_width = 1U; break;
    case InstructionId::Ldxrh: case InstructionId::Ldaxrh: case InstructionId::Ldarh:
    case InstructionId::Stxrh: case InstructionId::Stlxrh: case InstructionId::Stlrh:
        result.atomic_width = 2U; break;
    case InstructionId::Ldxr: case InstructionId::Ldaxr: case InstructionId::Ldar:
        if (!result.operands.empty() && result.operands[0].kind == OperandKind::Register)
            result.atomic_width = register_width_bytes(result.operands[0].reg);
        break;
    case InstructionId::Stxr: case InstructionId::Stlxr:
        if (result.operands.size() > 1U && result.operands[1].kind == OperandKind::Register)
            result.atomic_width = register_width_bytes(result.operands[1].reg);
        break;
    case InstructionId::Stlr:
        if (!result.operands.empty() && result.operands[0].kind == OperandKind::Register)
            result.atomic_width = register_width_bytes(result.operands[0].reg);
        break;
    default: break;
    }

    switch (result.id)
    {
    case InstructionId::Ldaxr: case InstructionId::Ldaxrb: case InstructionId::Ldaxrh:
    case InstructionId::Ldar: case InstructionId::Ldarb: case InstructionId::Ldarh:
        result.memory_order = AtomicMemoryOrder::Acquire; break;
    case InstructionId::Stlxr: case InstructionId::Stlxrb: case InstructionId::Stlxrh:
    case InstructionId::Stlr: case InstructionId::Stlrb: case InstructionId::Stlrh:
        result.memory_order = AtomicMemoryOrder::Release; break;
    default: result.memory_order = AtomicMemoryOrder::Relaxed; break;
    }

    switch (result.id)
    {
    case InstructionId::Stxr: case InstructionId::Stxrb: case InstructionId::Stxrh:
    case InstructionId::Stlxr: case InstructionId::Stlxrb: case InstructionId::Stlxrh:
        if (!result.operands.empty() && result.operands[0].kind == OperandKind::Register)
            result.exclusive_status_register = result.operands[0].reg;
        break;
    default: break;
    }

    if (result.id == InstructionId::Dmb || result.id == InstructionId::Dsb || result.id == InstructionId::Isb)
    {
        result.barrier_kind = result.id == InstructionId::Dmb ? BarrierKind::Dmb
                            : result.id == InstructionId::Dsb ? BarrierKind::Dsb : BarrierKind::Isb;
        result.barrier_option = barrier_option(result.opcode);
        if (result.id == InstructionId::Isb && result.barrier_option == BarrierOption::Invalid)
            result.barrier_option = BarrierOption::Sy;
    }

    if (result.id == InstructionId::Mrs || result.id == InstructionId::Msr)
    {
        const std::string_view text(result.disassembly);
        if (text.find("tpidrro_el0") != std::string_view::npos)
            result.system_register = SystemRegister::TpidrroEl0;
        else if (text.find("tpidr_el0") != std::string_view::npos)
            result.system_register = SystemRegister::TpidrEl0;
        else
            result.normalized = false;
    }
}
} // namespace

Result<DecodedInstruction> AArch64Decoder::decode(GuestAddress address, std::uint32_t opcode) const
{
    auto decoded = decode_legacy(address, opcode);
    if (!decoded) return decoded;
    normalize_concurrency(decoded.value());
    return decoded;
}

Result<DecodedInstruction> fetch_and_decode(const memory::GuestMemory& memory,
                                            const AArch64Decoder& decoder,
                                            GuestAddress address)
{
    const auto opcode = fetch_instruction(memory, address);
    if (!opcode) return Result<DecodedInstruction>::failure(opcode.error());
    return decoder.decode(address, opcode.value());
}

} // namespace switchrecomp::aarch64
