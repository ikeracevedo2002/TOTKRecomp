#include "switchrecomp/aarch64/decoder.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace
{

using switchrecomp::ErrorCode;
using switchrecomp::aarch64::AArch64Decoder;
using switchrecomp::aarch64::ConditionCode;
using switchrecomp::aarch64::ControlFlowKind;
using switchrecomp::aarch64::InstructionId;
using switchrecomp::aarch64::OperandKind;
using switchrecomp::aarch64::RegisterWidth;
using switchrecomp::memory::GuestAddress;
using switchrecomp::memory::GuestMemory;
using switchrecomp::memory::GuestMemoryPermissions;
using switchrecomp::memory::GuestRegionKind;

[[nodiscard]] std::vector<std::byte> words(std::initializer_list<std::uint32_t> values)
{
    std::vector<std::byte> result;
    result.reserve(values.size() * 4U);
    for (const auto value : values)
    {
        result.push_back(static_cast<std::byte>(value & 0xffU));
        result.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
        result.push_back(static_cast<std::byte>((value >> 16U) & 0xffU));
        result.push_back(static_cast<std::byte>((value >> 24U) & 0xffU));
    }
    return result;
}

[[nodiscard]] GuestMemory code_memory(GuestAddress base, std::initializer_list<std::uint32_t> code)
{
    GuestMemory memory;
    const auto bytes = words(code);
    REQUIRE(memory.map(base, bytes, GuestMemoryPermissions::Read | GuestMemoryPermissions::Execute,
                       ".text", GuestRegionKind::Text));
    return memory;
}

} // namespace

TEST_CASE("AArch64 decoder preserves opcode, address, registers, immediates and flow")
{
    const auto decoder = AArch64Decoder::create();
    REQUIRE(decoder);

    // add x0, x1, x2
    const auto add = decoder.value()->decode(0x1000U, 0x8b020020U);
    REQUIRE(add);
    REQUIRE(add.value().address == 0x1000U);
    REQUIRE(add.value().opcode == 0x8b020020U);
    REQUIRE(add.value().id == InstructionId::Add);
    REQUIRE(add.value().backend_decoded);
    REQUIRE(add.value().normalized);
    REQUIRE(add.value().control_flow.kind == ControlFlowKind::Fallthrough);
    REQUIRE(add.value().operands.size() == 3U);
    REQUIRE(add.value().operands[0].kind == OperandKind::Register);
    REQUIRE(add.value().operands[0].reg.width == RegisterWidth::X64);
    REQUIRE(add.value().operands[0].reg.index == 0U);
    REQUIRE(add.value().operands[1].reg.index == 1U);
    REQUIRE(add.value().operands[2].reg.index == 2U);

    // sub x2, x2, #1
    const auto sub = decoder.value()->decode(0x1004U, 0xd1000442U);
    REQUIRE(sub);
    REQUIRE(sub.value().id == InstructionId::Sub);
    REQUIRE(sub.value().operands[2].kind == OperandKind::Immediate);
    REQUIRE(sub.value().operands[2].immediate == 1);
}

TEST_CASE("AArch64 decoder classifies direct and conditional branches with checked targets")
{
    const auto decoder = AArch64Decoder::create();
    REQUIRE(decoder);

    // b +8, bl -8, b.eq +8
    const auto branch = decoder.value()->decode(0x1000U, 0x14000002U);
    REQUIRE(branch);
    REQUIRE(branch.value().id == InstructionId::B);
    REQUIRE(branch.value().control_flow.kind == ControlFlowKind::DirectBranch);
    REQUIRE(branch.value().control_flow.target == 0x1008U);
    REQUIRE_FALSE(branch.value().control_flow.has_fallthrough);

    const auto call = decoder.value()->decode(0x1008U, 0x97fffffeU);
    REQUIRE(call);
    REQUIRE(call.value().id == InstructionId::Bl);
    REQUIRE(call.value().control_flow.kind == ControlFlowKind::DirectCall);
    REQUIRE(call.value().control_flow.target == 0x1000U);
    REQUIRE(call.value().control_flow.has_fallthrough);

    const auto conditional = decoder.value()->decode(0x1000U, 0x54000040U);
    REQUIRE(conditional);
    REQUIRE(conditional.value().id == InstructionId::BCond);
    REQUIRE(conditional.value().control_flow.kind == ControlFlowKind::ConditionalBranch);
    REQUIRE(conditional.value().control_flow.target == 0x1008U);
    REQUIRE(conditional.value().control_flow.condition == ConditionCode::Eq);
}

TEST_CASE("AArch64 decoder preserves indirect targets, return registers and bit branches")
{
    const auto decoder = AArch64Decoder::create();
    REQUIRE(decoder);

    // br x9
    const auto branch = decoder.value()->decode(0x1000U, 0xd61f0120U);
    REQUIRE(branch);
    REQUIRE(branch.value().id == InstructionId::Br);
    REQUIRE(branch.value().control_flow.kind == ControlFlowKind::IndirectBranch);
    REQUIRE(branch.value().control_flow.register_target);
    REQUIRE(branch.value().control_flow.register_target->index == 9U);
    REQUIRE_FALSE(branch.value().control_flow.has_fallthrough);

    // blr x8
    const auto call = decoder.value()->decode(0x1004U, 0xd63f0100U);
    REQUIRE(call);
    REQUIRE(call.value().id == InstructionId::Blr);
    REQUIRE(call.value().control_flow.kind == ControlFlowKind::IndirectCall);
    REQUIRE(call.value().control_flow.register_target->index == 8U);
    REQUIRE(call.value().control_flow.has_fallthrough);

    // ret (default x30)
    const auto ret = decoder.value()->decode(0x1008U, 0xd65f03c0U);
    REQUIRE(ret);
    REQUIRE(ret.value().id == InstructionId::Ret);
    REQUIRE(ret.value().control_flow.kind == ControlFlowKind::Return);
    REQUIRE(ret.value().control_flow.return_register);
    REQUIRE(ret.value().control_flow.return_register->index == 30U);

    // tbz x0, #3, +8 and tbnz x0, #3, +8
    const auto tbz = decoder.value()->decode(0x1000U, 0x36180040U);
    REQUIRE(tbz);
    REQUIRE(tbz.value().id == InstructionId::Tbz);
    REQUIRE(tbz.value().control_flow.target == 0x1008U);
    const auto tbnz = decoder.value()->decode(0x1000U, 0x37180040U);
    REQUIRE(tbnz);
    REQUIRE(tbnz.value().id == InstructionId::Tbnz);
    REQUIRE(tbnz.value().control_flow.target == 0x1008U);
}

TEST_CASE("AArch64 decoder exposes PC-relative ADR, ADRP and literal loads")
{
    const auto decoder = AArch64Decoder::create();
    REQUIRE(decoder);

    // adr x0, #4
    const auto adr = decoder.value()->decode(0x1000U, 0x10000020U);
    REQUIRE(adr);
    REQUIRE(adr.value().id == InstructionId::Adr);
    REQUIRE(adr.value().pc_relative_value == 0x1004U);

    // adrp x0, #0 from a non-page-aligned PC
    const auto adrp = decoder.value()->decode(0x1234U, 0x90000000U);
    REQUIRE(adrp);
    REQUIRE(adrp.value().id == InstructionId::Adrp);
    REQUIRE(adrp.value().pc_relative_value == 0x1000U);

    // ldr x0, +8 (literal)
    const auto literal = decoder.value()->decode(0x1000U, 0x58000040U);
    REQUIRE(literal);
    REQUIRE(literal.value().id == InstructionId::LdrLiteral);
    REQUIRE(literal.value().pc_relative_value == 0x1008U);
}

TEST_CASE("AArch64 decoder handles memory, SIMD, atomics and system instructions")
{
    const auto decoder = AArch64Decoder::create();
    REQUIRE(decoder);

    // ldr x0, [x1, #8]
    const auto load = decoder.value()->decode(0x1000U, 0xf9400420U);
    REQUIRE(load);
    REQUIRE(load.value().id == InstructionId::Ldr);
    REQUIRE(load.value().operands[1].kind == OperandKind::Memory);
    REQUIRE(load.value().operands[1].memory.base.index == 1U);
    REQUIRE(load.value().operands[1].memory.displacement == 8);

    // fmul s0, s1, s2
    const auto fp = decoder.value()->decode(0x1000U, 0x1e220820U);
    REQUIRE(fp);
    REQUIRE(fp.value().backend_decoded);
    REQUIRE(fp.value().id == InstructionId::FpSimd);

    // ldxr x0, [x1]
    const auto atomic = decoder.value()->decode(0x1000U, 0xc85f7c20U);
    REQUIRE(atomic);
    REQUIRE(atomic.value().id == InstructionId::Ldxr);

    // svc #1 and brk #0 terminate normal traversal explicitly.
    const auto svc = decoder.value()->decode(0x1000U, 0xd4000021U);
    REQUIRE(svc);
    REQUIRE(svc.value().id == InstructionId::Svc);
    REQUIRE(svc.value().control_flow.kind == ControlFlowKind::Exception);
    REQUIRE_FALSE(svc.value().control_flow.has_fallthrough);
    const auto brk = decoder.value()->decode(0x1000U, 0xd4200000U);
    REQUIRE(brk);
    REQUIRE(brk.value().id == InstructionId::Brk);
    REQUIRE(brk.value().control_flow.kind == ControlFlowKind::Trap);

    // udf #0 is an architecturally undefined instruction and must not look like fallthrough.
    const auto udf = decoder.value()->decode(0x1000U, 0x00000000U);
    REQUIRE(udf);
    REQUIRE(udf.value().control_flow.kind == ControlFlowKind::Trap);
    REQUIRE_FALSE(udf.value().control_flow.has_fallthrough);
}

TEST_CASE("Instruction fetch is aligned, little-endian and executable-only")
{
    const auto memory = code_memory(0x1000U, {0xd503201fU});
    REQUIRE(switchrecomp::aarch64::fetch_instruction(memory, 0x1000U).value() == 0xd503201fU);
    REQUIRE(switchrecomp::aarch64::fetch_instruction(memory, 0x1001U).error().code ==
            ErrorCode::MisalignedInstructionAddress);
    REQUIRE(switchrecomp::aarch64::fetch_instruction(memory, 0x2000U).error().code ==
            ErrorCode::UnmappedMemory);

    GuestMemory non_executable;
    const auto bytes = words({0xd503201fU});
    REQUIRE(non_executable.map(0x3000U, bytes, GuestMemoryPermissions::Read, ".rodata",
                               GuestRegionKind::Rodata));
    REQUIRE(switchrecomp::aarch64::fetch_instruction(non_executable, 0x3000U).error().code ==
            ErrorCode::NonExecutableAddress);

    GuestMemory truncated;
    const std::array<std::byte, 3> incomplete{std::byte{0x1f}, std::byte{0x20}, std::byte{0x03}};
    REQUIRE(truncated.map(0x4000U, incomplete,
                          GuestMemoryPermissions::Read | GuestMemoryPermissions::Execute,
                          ".text", GuestRegionKind::Text));
    REQUIRE(switchrecomp::aarch64::fetch_instruction(truncated, 0x4000U).error().code ==
            ErrorCode::UnmappedMemory);
}

TEST_CASE("Decoder rejects misaligned and invalid opcodes explicitly")
{
    const auto decoder = AArch64Decoder::create();
    REQUIRE(decoder);
    REQUIRE(decoder.value()->decode(0x1002U, 0xd503201fU).error().code ==
            ErrorCode::MisalignedInstructionAddress);
    const auto invalid = decoder.value()->decode(0x1000U, 0xffffffffU);
    REQUIRE_FALSE(invalid);
    REQUIRE(invalid.error().code == ErrorCode::DecodeFailed);
}
