#include "switchrecomp/aarch64/decoder.hpp"
#include "switchrecomp/analysis/cfg_analyzer.hpp"
#ifdef TOTKRECOMP_HAS_LLVM
#include "switchrecomp/codegen/llvm_backend.hpp"
#endif
#include "switchrecomp/interpreter/interpreter.hpp"
#include "switchrecomp/ir/builder.hpp"
#include "switchrecomp/ir/verifier.hpp"
#include "switchrecomp/lifter/lifter.hpp"
#include "switchrecomp/runtime/context.hpp"
#include "switchrecomp/runtime/cpu_state.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <utility>
#include <vector>

namespace
{

using namespace switchrecomp;

constexpr memory::GuestAddress code_address = 0x7000U;
constexpr std::uint32_t ret = 0xd65f03c0U;

[[nodiscard]] std::vector<std::byte> code(std::initializer_list<std::uint32_t> words)
{
    std::vector<std::byte> bytes;
    bytes.reserve(words.size() * 4U);
    for (const auto word : words)
    {
        bytes.push_back(static_cast<std::byte>(word & 0xffU));
        bytes.push_back(static_cast<std::byte>((word >> 8U) & 0xffU));
        bytes.push_back(static_cast<std::byte>((word >> 16U) & 0xffU));
        bytes.push_back(static_cast<std::byte>((word >> 24U) & 0xffU));
    }
    return bytes;
}

struct LiftedProgram
{
    memory::GuestMemory memory;
    ir::Function function;
};

[[nodiscard]] Result<LiftedProgram> lift_program(std::initializer_list<std::uint32_t> words)
{
    auto bytes = code(words);
    memory::GuestMemory memory;
    const auto mapped = memory.map(code_address, std::span<const std::byte>(bytes),
                                   memory::GuestMemoryPermissions::Read |
                                       memory::GuestMemoryPermissions::Execute,
                                   "m35.fp_simd.text", memory::GuestRegionKind::Text);
    if (!mapped) return Result<LiftedProgram>::failure(mapped.error());

    analysis::AnalysisOptions options;
    options.allowed_code_range = analysis::GuestAddressRange{
        code_address, static_cast<memory::GuestSize>(bytes.size())};
    const auto cfg = analysis::analyze_control_flow(memory, code_address, options);
    if (!cfg) return Result<LiftedProgram>::failure(cfg.error());
    const auto function = lifter::lift_function(cfg.value());
    if (!function) return Result<LiftedProgram>::failure(function.error());
    return Result<LiftedProgram>::success(
        LiftedProgram{std::move(memory), std::move(function).value()});
}

[[nodiscard]] Result<runtime::ExecutionResult> execute_program(
    LiftedProgram& program, runtime::CpuState& cpu)
{
    runtime::RuntimeContext context{&program.memory};
    return interpreter::execute(program.function, cpu, context);
}

} // namespace

TEST_CASE("M35 measured vector FMOV normalizes typed operands and fallthrough")
{
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);
    const auto decoded = decoder.value()->decode(code_address, 0x0f03f5e1U);
    REQUIRE(decoded);
    REQUIRE(decoded.value().id == aarch64::InstructionId::FpSimd);
    REQUIRE(decoded.value().simd_operation == aarch64::SimdOperation::Fmov);
    REQUIRE(decoded.value().normalized);
    REQUIRE(decoded.value().operands.size() == 2U);
    REQUIRE(decoded.value().operands[0].kind == aarch64::OperandKind::Register);
    REQUIRE(decoded.value().operands[0].reg.kind == aarch64::RegisterKind::Vector);
    REQUIRE(decoded.value().operands[0].reg.index == 1U);
    REQUIRE(decoded.value().operands[0].arrangement == aarch64::VectorArrangement::S2);
    REQUIRE(decoded.value().operands[1].kind == aarch64::OperandKind::FloatingImmediate);
    REQUIRE(decoded.value().operands[1].has_floating_immediate);
    REQUIRE(decoded.value().operands[1].floating_immediate == 0.96875);
    REQUIRE(decoded.value().control_flow.kind == aarch64::ControlFlowKind::Fallthrough);
    REQUIRE(decoded.value().control_flow.has_fallthrough);
    REQUIRE_FALSE(decoded.value().operands[1].reg.valid());
}

TEST_CASE("M35 measured vector FMOV lifts and verifies")
{
    auto program = lift_program({0x0f03f5e1U, ret});
    REQUIRE(program);
    REQUIRE(ir::verify(program.value().function));
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);
    const auto decoded = decoder.value()->decode(code_address, 0x0f03f5e1U);
    REQUIRE(decoded);
    REQUIRE(lifter::is_instruction_liftable(decoded.value()));
}

TEST_CASE("M35 FMOV .2S broadcasts exact bits and clears upper V bits")
{
    auto program = lift_program({0x0f03f5e1U, ret});
    REQUIRE(program);
    runtime::CpuState cpu;
    cpu.vreg[1] = runtime::Vector128{0xaaaaaaaa3f000000ULL, 0xbbbbbbbbbbbbbbbbULL};
    cpu.fpcr = 0x12345678U;
    cpu.fpsr = 0x0000001fU;
    cpu.n = 1U;
    cpu.z = 0U;
    cpu.c = 1U;
    cpu.v = 1U;
    const auto before = cpu;
    REQUIRE(execute_program(program.value(), cpu));
    REQUIRE(cpu.vreg[1] == (runtime::Vector128{0x3f7800003f780000ULL, 0U}));
    REQUIRE(cpu.fpcr == before.fpcr);
    REQUIRE(cpu.fpsr == before.fpsr);
    REQUIRE(cpu.n == before.n);
    REQUIRE(cpu.z == before.z);
    REQUIRE(cpu.c == before.c);
    REQUIRE(cpu.v == before.v);
}

TEST_CASE("M35 FMOV .4S broadcasts all exact f32 lanes")
{
    auto program = lift_program({0x4f03f401U, ret}); // fmov v1.4s, #0.5
    REQUIRE(program);
    runtime::CpuState cpu;
    cpu.vreg[1] = runtime::Vector128{UINT64_MAX, UINT64_MAX};
    REQUIRE(execute_program(program.value(), cpu));
    REQUIRE(cpu.vreg[1] == (runtime::Vector128{0x3f0000003f000000ULL,
                                               0x3f0000003f000000ULL}));
}

TEST_CASE("M35 FMOV .2D broadcasts both exact f64 lanes")
{
    auto program = lift_program({0x6f04f402U, ret}); // fmov v2.2d, #-2.0
    REQUIRE(program);
    runtime::CpuState cpu;
    cpu.vreg[2] = runtime::Vector128{0U, UINT64_MAX};
    REQUIRE(execute_program(program.value(), cpu));
    REQUIRE(cpu.vreg[2] == (runtime::Vector128{0xc000000000000000ULL,
                                               0xc000000000000000ULL}));
}

TEST_CASE("M35 FMOV vector immediates preserve negative and varied exponent/fraction bits")
{
    auto negative = lift_program({0x0f07f600U, ret}); // fmov v0.2s, #-1.0
    REQUIRE(negative);
    runtime::CpuState negative_cpu;
    REQUIRE(execute_program(negative.value(), negative_cpu));
    REQUIRE(negative_cpu.vreg[0] == (runtime::Vector128{0xbf800000bf800000ULL, 0U}));

    auto varied = lift_program({0x4f03f401U, ret}); // fmov v1.4s, #0.5
    REQUIRE(varied);
    runtime::CpuState varied_cpu;
    REQUIRE(execute_program(varied.value(), varied_cpu));
    REQUIRE(runtime::read_lane_bits(varied_cpu.vreg[1], 32U, 0U) == 0x3f000000U);
    REQUIRE(runtime::read_lane_bits(varied_cpu.vreg[1], 32U, 3U) == 0x3f000000U);
}

TEST_CASE("M35 scalar FMOV immediates remain scalar after vector FMOV support")
{
    auto single = lift_program({0x1e2e1000U, ret}); // fmov s0, #1.0
    REQUIRE(single);
    runtime::CpuState single_cpu;
    single_cpu.vreg[0] = runtime::Vector128{0xaaaaaaaa00000000ULL, 0xbbbbbbbbbbbbbbbbULL};
    single_cpu.fpcr = 0x12345678U;
    single_cpu.fpsr = 0x0000001fU;
    single_cpu.n = 1U;
    single_cpu.z = 0U;
    single_cpu.c = 1U;
    single_cpu.v = 1U;
    const auto single_before = single_cpu;
    REQUIRE(execute_program(single.value(), single_cpu));
    REQUIRE(single_cpu.vreg[0] == (runtime::Vector128{0x000000003f800000ULL, 0U}));
    REQUIRE(single_cpu.fpcr == single_before.fpcr);
    REQUIRE(single_cpu.fpsr == single_before.fpsr);
    REQUIRE(single_cpu.n == single_before.n);
    REQUIRE(single_cpu.z == single_before.z);
    REQUIRE(single_cpu.c == single_before.c);
    REQUIRE(single_cpu.v == single_before.v);

    auto double_precision = lift_program({0x1e6e1001U, ret}); // fmov d1, #1.0
    REQUIRE(double_precision);
    runtime::CpuState double_cpu;
    double_cpu.vreg[1] = runtime::Vector128{0xaaaaaaaaaaaaaaaaULL, 0xbbbbbbbbbbbbbbbbULL};
    REQUIRE(execute_program(double_precision.value(), double_cpu));
    REQUIRE(double_cpu.vreg[1] == (runtime::Vector128{0x3ff0000000000000ULL, 0U}));
}

TEST_CASE("M35 reserved vector FMOV encoding is not normalized as a legal form")
{
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);
    REQUIRE_FALSE(decoder.value()->decode(code_address, 0x2f03f5e1U));

}

TEST_CASE("M35 verifier rejects a malformed floating VectorBroadcast")
{
    ir::Function function("m35_malformed_vector_broadcast", code_address);
    const auto block = function.add_block(code_address, "entry");
    function.set_entry_block(block);
    ir::Builder builder(function);
    REQUIRE(builder.set_insert_block(block));
    const auto scalar = builder.constant(ir::f32_type(), 0x3f780000U,
                                         ir::SourceLocation{code_address, 0U, "m35"});
    REQUIRE(scalar);
    ir::Instruction broadcast;
    broadcast.opcode = ir::Opcode::VectorBroadcast;
    broadcast.result_type = ir::v128_type();
    broadcast.operands = {scalar.value()};
    broadcast.arrangement = ir::VectorArrangement::S2;
    broadcast.source = ir::SourceLocation{code_address, 0U, "m35"};
    REQUIRE(builder.emit(std::move(broadcast)));
    ir::Terminator terminator;
    terminator.kind = ir::TerminatorKind::Return;
    terminator.source = ir::SourceLocation{code_address, 4U, "m35"};
    REQUIRE(builder.set_terminator(std::move(terminator)));
    REQUIRE_FALSE(ir::verify(function));
}

TEST_CASE("M35 measured MOVI vector immediate normalizes typed data")
{
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);
    const auto decoded = decoder.value()->decode(code_address, 0x6f00e400U);
    REQUIRE(decoded);
    REQUIRE(decoded.value().id == aarch64::InstructionId::FpSimd);
    REQUIRE(decoded.value().simd_operation == aarch64::SimdOperation::Movi);
    REQUIRE(decoded.value().normalized);
    REQUIRE(decoded.value().operands.size() == 2U);
    REQUIRE(decoded.value().operands[0].reg.kind == aarch64::RegisterKind::Vector);
    REQUIRE(decoded.value().operands[0].arrangement == aarch64::VectorArrangement::D2);
    REQUIRE(decoded.value().operands[1].kind == aarch64::OperandKind::Immediate);
    REQUIRE(decoded.value().operands[1].immediate == 0);
    REQUIRE(decoded.value().control_flow.kind == aarch64::ControlFlowKind::Fallthrough);
}

TEST_CASE("M35 MOVI vector immediate broadcasts exact 64-bit elements")
{
    auto zero = lift_program({0x6f00e400U, ret}); // movi v0.2d, #0
    REQUIRE(zero);
    runtime::CpuState zero_cpu;
    zero_cpu.vreg[0] = runtime::Vector128{UINT64_MAX, UINT64_MAX};
    zero_cpu.fpcr = 0x12345678U;
    zero_cpu.fpsr = 0x0000001fU;
    zero_cpu.n = 1U;
    zero_cpu.z = 0U;
    zero_cpu.c = 1U;
    zero_cpu.v = 1U;
    const auto before = zero_cpu;
    REQUIRE(execute_program(zero.value(), zero_cpu));
    REQUIRE(zero_cpu.vreg[0] == (runtime::Vector128{0U, 0U}));
    REQUIRE(zero_cpu.fpcr == before.fpcr);
    REQUIRE(zero_cpu.fpsr == before.fpsr);
    REQUIRE(zero_cpu.n == before.n);
    REQUIRE(zero_cpu.z == before.z);
    REQUIRE(zero_cpu.c == before.c);
    REQUIRE(zero_cpu.v == before.v);

    auto repeated = lift_program({0x6f00e420U, ret}); // movi v0.2d, #0101010101010101
    REQUIRE(repeated);
    runtime::CpuState repeated_cpu;
    REQUIRE(execute_program(repeated.value(), repeated_cpu));
    REQUIRE(repeated_cpu.vreg[0] == (runtime::Vector128{0x0101010101010101ULL,
                                                         0x0101010101010101ULL}));
}

TEST_CASE("M35 MOVI vector immediate lifts and verifies")
{
    auto program = lift_program({0x6f00e400U, ret});
    REQUIRE(program);
    REQUIRE(ir::verify(program.value().function));
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);
    const auto decoded = decoder.value()->decode(code_address, 0x6f00e400U);
    REQUIRE(decoded);
    REQUIRE(lifter::is_instruction_liftable(decoded.value()));
}

TEST_CASE("M35 MOVI MSL immediates use the architectural 32-bit expansion")
{
    constexpr std::uint32_t msl8 = 0x4f00c640U; // movi v0.4s, #0x12, msl #8
    constexpr std::uint32_t msl16 = 0x4f00d640U; // movi v0.4s, #0x12, msl #16
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);
    const auto first = decoder.value()->decode(code_address, msl8);
    const auto second = decoder.value()->decode(code_address, msl16);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(first.value().operands[1].immediate == static_cast<std::int64_t>(0x000012ff000012ffULL));
    REQUIRE(second.value().operands[1].immediate == static_cast<std::int64_t>(0x0012ffff0012ffffULL));

    auto program = lift_program({msl8, ret});
    REQUIRE(program);
    runtime::CpuState cpu;
    REQUIRE(execute_program(program.value(), cpu));
    REQUIRE(cpu.vreg[0] == (runtime::Vector128{0x000012ff000012ffULL,
                                               0x000012ff000012ffULL}));
}

TEST_CASE("M35 measured ST1 lane normalizes architectural arrangement")
{
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);
    const auto decoded = decoder.value()->decode(code_address, 0x4d008100U);
    REQUIRE(decoded);
    REQUIRE(decoded.value().id == aarch64::InstructionId::FpSimd);
    REQUIRE(decoded.value().simd_operation == aarch64::SimdOperation::St1);
    REQUIRE(decoded.value().normalized);
    REQUIRE(decoded.value().operands.size() == 2U);
    REQUIRE(decoded.value().operands[0].reg.kind == aarch64::RegisterKind::Vector);
    REQUIRE(decoded.value().operands[0].reg.index == 0U);
    REQUIRE(decoded.value().operands[0].arrangement == aarch64::VectorArrangement::S4);
    REQUIRE(decoded.value().operands[0].vector_index == 2);
    REQUIRE(decoded.value().operands[1].kind == aarch64::OperandKind::Memory);
    REQUIRE(decoded.value().operands[1].memory.base.kind == aarch64::RegisterKind::General);
    REQUIRE(decoded.value().operands[1].memory.base.index == 8U);
    REQUIRE(decoded.value().control_flow.kind == aarch64::ControlFlowKind::Fallthrough);
}

TEST_CASE("M35 ST1 single-lane store lifts with typed checked memory")
{
    auto program = lift_program({0x4d008100U, ret});
    REQUIRE(program);
    REQUIRE(ir::verify(program.value().function));
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);
    const auto decoded = decoder.value()->decode(code_address, 0x4d008100U);
    REQUIRE(decoded);
    REQUIRE(lifter::is_instruction_liftable(decoded.value()));
}

TEST_CASE("M35 ST1 stores the selected S lane and preserves architectural state")
{
    constexpr memory::GuestAddress data_address = 0x9000U;
    auto program = lift_program({0x4d008100U, ret});
    REQUIRE(program);
    std::array<std::byte, 16> initial{};
    REQUIRE(program.value().memory.map(
        data_address, std::span<const std::byte>(initial),
        memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Write,
        "m35.st1.data", memory::GuestRegionKind::Data));

    runtime::CpuState cpu;
    cpu.vreg[0] = runtime::Vector128{0x5566778811223344ULL, 0x99aabbccddeeff00ULL};
    cpu.x[8] = data_address;
    cpu.x[0] = 0x123456789abcdef0ULL;
    cpu.fpcr = 0x12345678U;
    cpu.fpsr = 0x0000001fU;
    cpu.n = 1U;
    cpu.z = 0U;
    cpu.c = 1U;
    cpu.v = 1U;
    const auto before = cpu;
    REQUIRE(execute_program(program.value(), cpu));

    std::uint64_t stored = 0U;
    runtime::RuntimeContext context{&program.value().memory};
    REQUIRE(runtime::switchrecomp_runtime_guest_load(&context, data_address, 8U, &stored) == 0U);
    REQUIRE(stored == 0x00000000ddeeff00ULL);
    REQUIRE(cpu.vreg == before.vreg);
    REQUIRE(cpu.x[0] == before.x[0]);
    REQUIRE(cpu.x[8] == before.x[8]);
    REQUIRE(cpu.fpcr == before.fpcr);
    REQUIRE(cpu.fpsr == before.fpsr);
    REQUIRE(cpu.n == before.n);
    REQUIRE(cpu.z == before.z);
    REQUIRE(cpu.c == before.c);
    REQUIRE(cpu.v == before.v);
}

TEST_CASE("M35 ST1 lane normalization is generic across B/H/S/D arrangements")
{
    struct Case
    {
        std::uint32_t opcode;
        aarch64::VectorArrangement arrangement;
        std::int8_t lane;
    };
    constexpr Case cases[]{
        {0x0d000900U, aarch64::VectorArrangement::B8, 2},
        {0x0d005121U, aarch64::VectorArrangement::H4, 2},
        {0x4d008142U, aarch64::VectorArrangement::S4, 2},
        {0x4d008563U, aarch64::VectorArrangement::D2, 1},
    };
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);
    for (const auto& test : cases)
    {
        const auto decoded = decoder.value()->decode(code_address, test.opcode);
        REQUIRE(decoded);
        REQUIRE(decoded.value().simd_operation == aarch64::SimdOperation::St1);
        REQUIRE(decoded.value().normalized);
        REQUIRE(decoded.value().operands[0].arrangement == test.arrangement);
        REQUIRE(decoded.value().operands[0].vector_index == test.lane);
        REQUIRE(lifter::is_instruction_liftable(decoded.value()));
    }
}

TEST_CASE("M35 ST1 multi-register form is not claimed as a single-lane operation")
{
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);
    const auto decoded = decoder.value()->decode(code_address, 0x4c007984U); // st1 {v4.4s}, [x12]
    REQUIRE(decoded);
    REQUIRE(decoded.value().simd_operation == aarch64::SimdOperation::St1);
    REQUIRE_FALSE(lifter::is_instruction_liftable(decoded.value()));
}

#ifdef TOTKRECOMP_HAS_LLVM
TEST_CASE("M35 vector FMOV interpreter and LLVM state agree")
{
    auto program = lift_program({0x0f03f5e1U, ret});
    REQUIRE(program);
    const auto backend = codegen::LlvmBackend::create();
    REQUIRE(backend);

    runtime::CpuState interpreter_cpu;
    interpreter_cpu.vreg[1] = runtime::Vector128{UINT64_MAX, UINT64_MAX};
    interpreter_cpu.fpcr = 0x12345678U;
    interpreter_cpu.fpsr = 0x0000001fU;
    interpreter_cpu.n = 1U;
    interpreter_cpu.z = 1U;
    interpreter_cpu.c = 0U;
    interpreter_cpu.v = 1U;
    runtime::RuntimeContext interpreter_context{&program.value().memory};
    const auto interpreted = interpreter::execute(
        program.value().function, interpreter_cpu, interpreter_context);
    REQUIRE(interpreted);

    runtime::CpuState llvm_cpu;
    llvm_cpu.vreg[1] = runtime::Vector128{UINT64_MAX, UINT64_MAX};
    llvm_cpu.fpcr = 0x12345678U;
    llvm_cpu.fpsr = 0x0000001fU;
    llvm_cpu.n = 1U;
    llvm_cpu.z = 1U;
    llvm_cpu.c = 0U;
    llvm_cpu.v = 1U;
    runtime::RuntimeContext llvm_context{&program.value().memory};
    const auto lowered = backend.value()->lower_to_llvm_ir(program.value().function);
    REQUIRE(lowered);
    const auto native = backend.value()->execute(
        program.value().function, llvm_cpu, llvm_context);
    REQUIRE(native);
    REQUIRE(llvm_cpu.vreg == interpreter_cpu.vreg);
    REQUIRE(llvm_cpu.fpcr == interpreter_cpu.fpcr);
    REQUIRE(llvm_cpu.fpsr == interpreter_cpu.fpsr);
    REQUIRE(llvm_cpu.n == interpreter_cpu.n);
    REQUIRE(llvm_cpu.z == interpreter_cpu.z);
    REQUIRE(llvm_cpu.c == interpreter_cpu.c);
    REQUIRE(llvm_cpu.v == interpreter_cpu.v);
}

TEST_CASE("M35 MOVI and ST1 interpreter and LLVM architectural state agree")
{
    constexpr memory::GuestAddress data_address = 0x9000U;
    const auto make_program = [&]() -> Result<LiftedProgram> {
        auto program = lift_program({0x6f00e420U, 0x4d008100U, ret});
        if (!program) return program;
        std::array<std::byte, 16> initial{};
        const auto mapped = program.value().memory.map(
            data_address, std::span<const std::byte>(initial),
            memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Write,
            "m35.llvm.st1.data", memory::GuestRegionKind::Data);
        if (!mapped) return Result<LiftedProgram>::failure(mapped.error());
        return program;
    };
    auto interpreter_program = make_program();
    auto llvm_program = make_program();
    REQUIRE(interpreter_program);
    REQUIRE(llvm_program);
    const auto backend = codegen::LlvmBackend::create();
    REQUIRE(backend);

    runtime::CpuState interpreter_cpu;
    interpreter_cpu.x[8] = data_address;
    runtime::RuntimeContext interpreter_context{&interpreter_program.value().memory};
    REQUIRE(interpreter::execute(interpreter_program.value().function, interpreter_cpu,
                                 interpreter_context));

    runtime::CpuState llvm_cpu;
    llvm_cpu.x[8] = data_address;
    runtime::RuntimeContext llvm_context{&llvm_program.value().memory};
    REQUIRE(backend.value()->lower_to_llvm_ir(llvm_program.value().function));
    REQUIRE(backend.value()->execute(llvm_program.value().function, llvm_cpu, llvm_context));

    REQUIRE(llvm_cpu.vreg == interpreter_cpu.vreg);
    REQUIRE(llvm_cpu.fpcr == interpreter_cpu.fpcr);
    REQUIRE(llvm_cpu.fpsr == interpreter_cpu.fpsr);
    REQUIRE(llvm_cpu.n == interpreter_cpu.n);
    REQUIRE(llvm_cpu.z == interpreter_cpu.z);
    REQUIRE(llvm_cpu.c == interpreter_cpu.c);
    REQUIRE(llvm_cpu.v == interpreter_cpu.v);
    std::uint64_t interpreter_memory = 0U;
    std::uint64_t llvm_memory = 0U;
    REQUIRE(runtime::switchrecomp_runtime_guest_load(&interpreter_context, data_address, 4U,
                                                     &interpreter_memory) == 0U);
    REQUIRE(runtime::switchrecomp_runtime_guest_load(&llvm_context, data_address, 4U,
                                                     &llvm_memory) == 0U);
    REQUIRE(llvm_memory == interpreter_memory);
}
#endif
