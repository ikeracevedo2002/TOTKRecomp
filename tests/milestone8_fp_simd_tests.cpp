#include "switchrecomp/aarch64/decoder.hpp"
#include "switchrecomp/analysis/cfg_analyzer.hpp"
#include "switchrecomp/interpreter/interpreter.hpp"
#include "switchrecomp/ir/builder.hpp"
#include "switchrecomp/ir/verifier.hpp"
#include "switchrecomp/lifter/lifter.hpp"
#include "switchrecomp/runtime/context.hpp"
#include "switchrecomp/runtime/cpu_state.hpp"
#include "switchrecomp/runtime/fp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace
{

using namespace switchrecomp;

[[nodiscard]] ir::SourceLocation source()
{
    return ir::SourceLocation{0x100000U, 0U, "milestone8"};
}

[[nodiscard]] ir::Function scalar_add_function()
{
    ir::Function function("m8_scalar_add", 0x100000U);
    const auto block = function.add_block(0x100000U, "entry");
    function.set_entry_block(block);
    ir::Builder builder(function);
    REQUIRE(builder.set_insert_block(block));
    const auto one = builder.constant(ir::f32_type(), std::bit_cast<std::uint32_t>(1.5F), source());
    const auto two = builder.constant(ir::f32_type(), std::bit_cast<std::uint32_t>(2.25F), source());
    REQUIRE(one);
    REQUIRE(two);
    ir::Instruction add;
    add.opcode = ir::Opcode::FpBinary;
    add.result_type = ir::f32_type();
    add.operands = {one.value(), two.value()};
    add.fp_binary = ir::FpBinaryOperation::Add;
    add.source = source();
    const auto sum = builder.emit(std::move(add));
    REQUIRE(sum);
    ir::Instruction cast;
    cast.opcode = ir::Opcode::BitCast;
    cast.result_type = ir::i32_type();
    cast.operands = {sum.value()};
    cast.source = source();
    const auto bits = builder.emit(std::move(cast));
    REQUIRE(bits);
    ir::Instruction write;
    write.opcode = ir::Opcode::WriteRegister;
    write.result_type = ir::void_type();
    write.operands = {bits.value()};
    write.reg = ir::GuestRegister{ir::RegisterWidth::W32, 0U, false, false};
    write.source = source();
    REQUIRE(builder.emit_void(std::move(write)));
    ir::Terminator terminator;
    terminator.kind = ir::TerminatorKind::Return;
    terminator.source = source();
    REQUIRE(builder.set_terminator(std::move(terminator)));
    return function;
}

} // namespace

TEST_CASE("Milestone 8 models the shared V register file and scalar aliases")
{
    runtime::CpuState cpu;
    runtime::write_scalar_vector(cpu, 3U, 32U, 0x3f800000U);
    REQUIRE(cpu.vreg[3].lo == 0x3f800000U);
    REQUIRE(cpu.vreg[3].hi == 0U);
    REQUIRE(runtime::read_vector_lane(cpu, 3U, 32U, 0U) == 0x3f800000U);
    runtime::write_vector_lane(cpu, 3U, 32U, 1U, 0x40000000U);
    REQUIRE(runtime::read_vector_lane(cpu, 3U, 32U, 1U) == 0x40000000U);
}

TEST_CASE("Milestone 8 scalar reference semantics preserve IEEE edge cases")
{
    runtime::CpuState cpu;
    const auto one = std::bit_cast<std::uint32_t>(1.0F);
    const auto two = std::bit_cast<std::uint32_t>(2.0F);
    REQUIRE(runtime::fp_binary(cpu, runtime::FpBinaryOperation::Add, 32U, one, two) ==
            std::bit_cast<std::uint32_t>(3.0F));
    REQUIRE(runtime::fp_binary(cpu, runtime::FpBinaryOperation::Div, 32U, one, 0U) ==
            0x7f800000U);
    REQUIRE((cpu.fpsr & runtime::fpsr_divide_by_zero) != 0U);
    REQUIRE(runtime::fp_binary(cpu, runtime::FpBinaryOperation::Add, 32U, 0x7f800000U,
                              0xff800000U) == runtime::default_nan_bits(32U));
    REQUIRE((cpu.fpsr & runtime::fpsr_invalid_operation) != 0U);
    REQUIRE(runtime::fp_round(cpu, 32U, std::bit_cast<std::uint32_t>(1.5F),
                              runtime::FpRoundingMode::NearestEven) ==
            std::bit_cast<std::uint32_t>(2.0F));
    REQUIRE(runtime::fp_round(cpu, 32U, std::bit_cast<std::uint32_t>(1.5F),
                              runtime::FpRoundingMode::TowardZero) ==
            std::bit_cast<std::uint32_t>(1.0F));
    cpu.fpcr = runtime::fpcr_flush_to_zero;
    cpu.fpsr = 0U;
    REQUIRE(runtime::fp_binary(cpu, runtime::FpBinaryOperation::Add, 32U, 0x00000001U, 0U) == 0U);
    REQUIRE((cpu.fpsr & runtime::fpsr_underflow) != 0U);
}

TEST_CASE("Milestone 8 vector operations use explicit lane order")
{
    runtime::Vector128 left{};
    runtime::Vector128 right{};
    for (std::uint8_t lane = 0U; lane < 4U; ++lane)
    {
        runtime::write_lane_bits(left, 32U, lane, lane + 1U);
        runtime::write_lane_bits(right, 32U, lane, lane + 11U);
    }
    runtime::CpuState cpu;
    const auto sum = runtime::vector_binary(cpu, 4U, 6U, left, right); // ADD .4S
    REQUIRE(runtime::read_lane_bits(sum, 32U, 0U) == 12U);
    REQUIRE(runtime::read_lane_bits(sum, 32U, 3U) == 18U);
    const auto zipped = runtime::vector_shuffle(1U, 6U, left, right, 0U); // ZIP1 .4S
    REQUIRE(runtime::read_lane_bits(zipped, 32U, 0U) == 1U);
    REQUIRE(runtime::read_lane_bits(zipped, 32U, 1U) == 11U);
    REQUIRE(runtime::read_lane_bits(zipped, 32U, 2U) == 2U);
    REQUIRE(runtime::read_lane_bits(zipped, 32U, 3U) == 12U);
}

TEST_CASE("Milestone 8 vector guest memory remains checked and little endian")
{
    memory::GuestMemory memory;
    std::array<std::byte, 16> bytes{};
    REQUIRE(memory.map(0x4000U, std::span<const std::byte>(bytes),
                       memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Write,
                       "vector.data", memory::GuestRegionKind::Data));
    runtime::RuntimeContext context{&memory};
    runtime::Vector128 input{0x1122334455667788ULL, 0x99aabbccddeeff00ULL};
    runtime::Vector128 output{};
    REQUIRE(runtime::switchrecomp_runtime_guest_store_vector(&context, 0x4000U, &input) == 0U);
    REQUIRE(runtime::switchrecomp_runtime_guest_load_vector(&context, 0x4000U, &output) == 0U);
    REQUIRE(output == input);
    REQUIRE(runtime::switchrecomp_runtime_guest_load_vector(&context, 0x5000U, &output) != 0U);
    REQUIRE(context.has_error);
}

TEST_CASE("Milestone 8 interpreter executes typed scalar FP IR")
{
    auto function = scalar_add_function();
    REQUIRE(ir::verify(function));
    runtime::CpuState cpu;
    runtime::RuntimeContext context;
    REQUIRE(interpreter::execute(function, cpu, context));
    REQUIRE(cpu.x[0] == std::bit_cast<std::uint32_t>(3.75F));
}

TEST_CASE("Milestone 8 decoder exposes project-owned SIMD normalization")
{
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);
    const auto decoded = decoder.value()->decode(0x100000U, 0x1e222820U); // fadd s0, s1, s2
    REQUIRE(decoded);
    REQUIRE(decoded.value().id == aarch64::InstructionId::FpSimd);
    REQUIRE(decoded.value().simd_operation == aarch64::SimdOperation::Fadd);
    REQUIRE(decoded.value().normalized);
}

TEST_CASE("Milestone 8 lifts and interprets a scalar FP instruction")
{
    constexpr memory::GuestAddress address = 0x6000U;
    const std::array<std::byte, 8> bytes{
        static_cast<std::byte>(0x20U), static_cast<std::byte>(0x28U),
        static_cast<std::byte>(0x22U), static_cast<std::byte>(0x1eU),
        static_cast<std::byte>(0xc0U), static_cast<std::byte>(0x03U),
        static_cast<std::byte>(0x5fU), static_cast<std::byte>(0xd6U)};
    memory::GuestMemory memory;
    REQUIRE(memory.map(address, std::span<const std::byte>(bytes),
                       memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Execute,
                       "fp.text", memory::GuestRegionKind::Text));
    analysis::AnalysisOptions options;
    options.allowed_code_range = analysis::GuestAddressRange{address, bytes.size()};
    const auto cfg = analysis::analyze_control_flow(memory, address, options);
    REQUIRE(cfg);
    const auto function = lifter::lift_function(cfg.value());
    REQUIRE(function);
    runtime::CpuState cpu;
    cpu.vreg[1].lo = std::bit_cast<std::uint32_t>(1.5F);
    cpu.vreg[2].lo = std::bit_cast<std::uint32_t>(2.25F);
    runtime::RuntimeContext context{&memory};
    REQUIRE(interpreter::execute(function.value(), cpu, context));
    REQUIRE(static_cast<std::uint32_t>(cpu.vreg[0].lo) == std::bit_cast<std::uint32_t>(3.75F));
}
