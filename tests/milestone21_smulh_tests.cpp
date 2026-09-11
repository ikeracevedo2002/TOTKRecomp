#include "switchrecomp/aarch64/decoder.hpp"
#include "switchrecomp/analysis/cfg_analyzer.hpp"
#include "switchrecomp/common/portable_arithmetic.hpp"
#include "switchrecomp/interpreter/interpreter.hpp"
#include "switchrecomp/ir/builder.hpp"
#include "switchrecomp/ir/printer.hpp"
#include "switchrecomp/ir/verifier.hpp"
#include "switchrecomp/lifter/lifter.hpp"

#ifdef TOTKRECOMP_HAS_LLVM
#include "switchrecomp/codegen/llvm_backend.hpp"
#endif

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace switchrecomp;

struct Vector
{
    const char* name;
    std::uint64_t left;
    std::uint64_t right;
    std::uint64_t expected;
};

// Fixed vectors were generated independently from the production helper by
// evaluating the signed mathematical product with arbitrary precision. The
// expected values are retained here so the tests do not share an arithmetic
// implementation with the code under test.
constexpr std::array<Vector, 20> kVectors{{
    {"zero_zero", 0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL},
    {"zero_positive", 0x0000000000000000ULL, 0x123456789abcdef0ULL, 0x0000000000000000ULL},
    {"zero_negative", 0x0000000000000000ULL, 0xffffffffffffffffULL, 0x0000000000000000ULL},
    {"one_one", 0x0000000000000001ULL, 0x0000000000000001ULL, 0x0000000000000000ULL},
    {"one_negative_one", 0x0000000000000001ULL, 0xffffffffffffffffULL, 0xffffffffffffffffULL},
    {"negative_one_negative_one", 0xffffffffffffffffULL, 0xffffffffffffffffULL, 0x0000000000000000ULL},
    {"max_max", 0x7fffffffffffffffULL, 0x7fffffffffffffffULL, 0x3fffffffffffffffULL},
    {"min_min", 0x8000000000000000ULL, 0x8000000000000000ULL, 0x4000000000000000ULL},
    {"min_one", 0x8000000000000000ULL, 0x0000000000000001ULL, 0xffffffffffffffffULL},
    {"min_negative_one", 0x8000000000000000ULL, 0xffffffffffffffffULL, 0x0000000000000000ULL},
    {"max_negative_one", 0x7fffffffffffffffULL, 0xffffffffffffffffULL, 0xffffffffffffffffULL},
    {"positive_positive_high", 0x0000000100000000ULL, 0x0000000100000000ULL, 0x0000000000000001ULL},
    {"positive_negative", 0x123456789abcdef0ULL, 0xedcba98765432110ULL, 0xfeb49923cc095323ULL},
    {"negative_positive", 0xedcba98765432110ULL, 0x123456789abcdef0ULL, 0xfeb49923cc095323ULL},
    {"negative_negative", 0xedcba98765432110ULL, 0xfedcba9876543211ULL, 0x0014b66dc33f6acdULL},
    {"only_sign_bits", 0x8000000000000000ULL, 0x8000000000000000ULL, 0x4000000000000000ULL},
    {"populated_high_low", 0x923456789abcdef0ULL, 0x8123456789abcdefULL, 0x3668e87db10b145eULL},
    {"mixed_high_low", 0x7fedcba987654321ULL, 0x8123456789abcdefULL, 0xc09aa828936005fcULL},
    {"all_bits_negative", 0xffffffffffffffffULL, 0xffffffffffffffffULL, 0x0000000000000000ULL},
    {"negative_pow2", 0xffffffffffff0000ULL, 0x0000000100000000ULL, 0xffffffffffffffffULL},
}};

[[nodiscard]] std::vector<std::byte> words(std::initializer_list<std::uint32_t> values)
{
    std::vector<std::byte> bytes;
    bytes.reserve(values.size() * 4U);
    for (const auto word : values)
    {
        bytes.push_back(static_cast<std::byte>(word & 0xffU));
        bytes.push_back(static_cast<std::byte>((word >> 8U) & 0xffU));
        bytes.push_back(static_cast<std::byte>((word >> 16U) & 0xffU));
        bytes.push_back(static_cast<std::byte>((word >> 24U) & 0xffU));
    }
    return bytes;
}

struct Fixture
{
    memory::GuestMemory memory;
    memory::GuestAddress address = 0x200000U;
    analysis::ControlFlowGraph cfg;
    ir::Function function;
};

[[nodiscard]] Result<Fixture> make_fixture(std::uint32_t smulh_opcode,
                                            memory::GuestAddress address = 0x200000U)
{
    Fixture fixture;
    fixture.address = address;
    const auto bytes = words({smulh_opcode, 0xd65f03c0U}); // SMULH; RET
    const auto mapped = fixture.memory.map(
        address, std::span<const std::byte>(bytes),
        memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Execute,
        "m21.smulh.text", memory::GuestRegionKind::Text);
    if (!mapped) return Result<Fixture>::failure(mapped.error());

    analysis::AnalysisOptions options;
    options.allowed_code_range = analysis::GuestAddressRange{
        address, static_cast<memory::GuestSize>(bytes.size())};
    const auto cfg = analysis::analyze_control_flow(fixture.memory, address, options);
    if (!cfg) return Result<Fixture>::failure(cfg.error());
    fixture.cfg = cfg.value();
    const auto function = lifter::lift_function(fixture.cfg);
    if (!function) return Result<Fixture>::failure(function.error());
    fixture.function = function.value();
    return Result<Fixture>::success(std::move(fixture));
}

[[nodiscard]] Result<ir::Function> make_ir_function()
{
    ir::Function function("m21_smulh_ir", 0x1000U);
    const auto block = function.add_block(0x1000U, "entry");
    function.set_entry_block(block);
    ir::Builder builder(function);
    const auto inserted = builder.set_insert_block(block);
    if (!inserted) return Result<ir::Function>::failure(inserted.error());

    ir::Instruction read_left;
    read_left.opcode = ir::Opcode::ReadRegister;
    read_left.result_type = ir::i64_type();
    read_left.reg = ir::GuestRegister{ir::RegisterWidth::X64, 1U, false, false};
    const auto left = builder.emit(read_left);
    if (!left) return Result<ir::Function>::failure(left.error());

    ir::Instruction read_right;
    read_right.opcode = ir::Opcode::ReadRegister;
    read_right.result_type = ir::i64_type();
    read_right.reg = ir::GuestRegister{ir::RegisterWidth::X64, 2U, false, false};
    const auto right = builder.emit(read_right);
    if (!right) return Result<ir::Function>::failure(right.error());

    const auto high = builder.mul_high_signed(left.value(), right.value(), ir::i64_type());
    if (!high) return Result<ir::Function>::failure(high.error());

    ir::Instruction write;
    write.opcode = ir::Opcode::WriteRegister;
    write.result_type = ir::void_type();
    write.operands = {high.value()};
    write.reg = ir::GuestRegister{ir::RegisterWidth::X64, 0U, false, false};
    const auto written = builder.emit_void(write);
    if (!written) return Result<ir::Function>::failure(written.error());

    ir::Terminator ret;
    ret.kind = ir::TerminatorKind::Return;
    const auto terminated = builder.set_terminator(ret);
    if (!terminated) return Result<ir::Function>::failure(terminated.error());
    return Result<ir::Function>::success(std::move(function));
}

[[nodiscard]] std::uint32_t smulh_opcode(std::uint8_t destination, std::uint8_t lhs,
                                         std::uint8_t rhs) noexcept
{
    return 0x9b407c00U | (static_cast<std::uint32_t>(rhs) << 16U) |
           (static_cast<std::uint32_t>(lhs) << 5U) | destination;
}

} // namespace

TEST_CASE("M21 portable signed high multiply matches independent edge vectors")
{
    for (const auto& vector : kVectors)
    {
        INFO(vector.name);
        REQUIRE(common::multiply_high_signed_64(vector.left, vector.right) == vector.expected);
        REQUIRE(common::multiply_high_signed_64(vector.right, vector.left) == vector.expected);
    }
}

TEST_CASE("M21 signed high multiply is a typed Semantic IR operation")
{
    REQUIRE(ir::opcode_name(ir::Opcode::MulHighSigned) == "mul_high_signed");
    const auto function = make_ir_function();
    REQUIRE(function);
    REQUIRE(ir::verify(function.value()));
    REQUIRE(ir::print(function.value()).find("mul_high_signed") != std::string::npos);

    runtime::CpuState cpu;
    cpu.x[1] = 0x923456789abcdef0ULL;
    cpu.x[2] = 0x8123456789abcdefULL;
    cpu.n = 1U;
    cpu.z = 0U;
    cpu.c = 1U;
    cpu.v = 1U;
    runtime::RuntimeContext context;
    const auto execution = interpreter::execute(function.value(), cpu, context);
    REQUIRE(execution);
    REQUIRE(cpu.x[0] == 0x3668e87db10b145eULL);
    REQUIRE(cpu.n == 1U);
    REQUIRE(cpu.z == 0U);
    REQUIRE(cpu.c == 1U);
    REQUIRE(cpu.v == 1U);

    ir::Function invalid("m21_invalid_smulh", 0x1000U);
    const auto block = invalid.add_block(0x1000U, "entry");
    invalid.set_entry_block(block);
    ir::Builder builder(invalid);
    REQUIRE(builder.set_insert_block(block));
    const auto left = builder.constant(ir::i32_type(), 1U);
    const auto right = builder.constant(ir::i32_type(), 2U);
    REQUIRE(left);
    REQUIRE(right);
    const auto wrong_width = builder.mul_high_signed(left.value(), right.value(), ir::i32_type());
    REQUIRE(wrong_width);
    ir::Terminator ret;
    ret.kind = ir::TerminatorKind::Return;
    REQUIRE(builder.set_terminator(ret));
    const auto verified = ir::verify(invalid);
    REQUIRE_FALSE(verified);
    REQUIRE(verified.error().code == ErrorCode::IrVerificationFailed);
    REQUIRE(verified.error().message.find("mul_high_signed") != std::string::npos);

    ir::Function invalid_result("m21_invalid_smulh_result", 0x1100U);
    const auto result_block = invalid_result.add_block(0x1100U, "entry");
    invalid_result.set_entry_block(result_block);
    ir::Builder result_builder(invalid_result);
    REQUIRE(result_builder.set_insert_block(result_block));
    const auto i64_left = result_builder.constant(ir::i64_type(), 1U);
    const auto i64_right = result_builder.constant(ir::i64_type(), 2U);
    REQUIRE(i64_left);
    REQUIRE(i64_right);
    ir::Instruction invalid_result_instruction;
    invalid_result_instruction.opcode = ir::Opcode::MulHighSigned;
    invalid_result_instruction.result_type = ir::i32_type();
    invalid_result_instruction.operands = {i64_left.value(), i64_right.value()};
    REQUIRE(result_builder.emit(invalid_result_instruction));
    REQUIRE(result_builder.set_terminator(ir::Terminator{}));
    const auto invalid_result_verified = ir::verify(invalid_result);
    REQUIRE_FALSE(invalid_result_verified);
    REQUIRE(invalid_result_verified.error().code == ErrorCode::IrVerificationFailed);
    REQUIRE(invalid_result_verified.error().message.find("mul_high_signed") != std::string::npos);
}

TEST_CASE("M21 SMULH normalization and lifting preserve X operands and fallthrough")
{
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);

    const std::array<std::uint32_t, 2> opcodes{
        0x9b4c7d4aU, // smulh x10, x10, x12: the real M20 frontier
        smulh_opcode(3U, 3U, 3U), // smulh x3, x3, x3
    };
    for (const auto opcode : opcodes)
    {
        const auto decoded = decoder.value()->decode(0x1000U, opcode);
        REQUIRE(decoded);
        REQUIRE(decoded.value().id == aarch64::InstructionId::Smulh);
        REQUIRE(decoded.value().normalized);
        REQUIRE(decoded.value().control_flow.kind == aarch64::ControlFlowKind::Fallthrough);
        REQUIRE(decoded.value().operands.size() == 3U);
        for (const auto& operand : decoded.value().operands)
        {
            REQUIRE(operand.kind == aarch64::OperandKind::Register);
            REQUIRE(operand.reg.kind == aarch64::RegisterKind::General);
            REQUIRE(operand.reg.width == aarch64::RegisterWidth::X64);
        }
    }

    auto fixture = make_fixture(0x9b4c7d4aU);
    REQUIRE(fixture);
    const auto& decoded = fixture.value().cfg.blocks.begin()->second.instructions.front();
    REQUIRE(decoded.id == aarch64::InstructionId::Smulh);
    REQUIRE(decoded.operands[0].reg.index == 10U);
    REQUIRE(decoded.operands[1].reg.index == 10U);
    REQUIRE(decoded.operands[2].reg.index == 12U);

    const ir::Instruction* high = nullptr;
    const ir::Instruction* write = nullptr;
    for (const auto& block : fixture.value().function.blocks())
    {
        for (const auto& instruction : block.instructions)
        {
            if (instruction.opcode == ir::Opcode::MulHighSigned) high = &instruction;
            if (instruction.opcode == ir::Opcode::WriteRegister) write = &instruction;
        }
        REQUIRE(block.terminator.kind == ir::TerminatorKind::Return);
    }
    REQUIRE(high != nullptr);
    REQUIRE(high->result_type == ir::i64_type());
    REQUIRE(high->operands.size() == 2U);
    REQUIRE(write != nullptr);
    REQUIRE(write->reg.index == 10U);
    REQUIRE(write->reg.width == ir::RegisterWidth::X64);

    runtime::CpuState cpu;
    cpu.x[10] = 0x923456789abcdef0ULL;
    cpu.x[12] = 0x8123456789abcdefULL;
    cpu.n = 1U;
    cpu.z = 1U;
    cpu.c = 0U;
    cpu.v = 1U;
    runtime::RuntimeContext context{&fixture.value().memory};
    const auto execution = interpreter::execute(fixture.value().function, cpu, context);
    REQUIRE(execution);
    REQUIRE(cpu.x[10] == 0x3668e87db10b145eULL);
    REQUIRE(cpu.pc == fixture.value().address + 4U);
    REQUIRE(cpu.n == 1U);
    REQUIRE(cpu.z == 1U);
    REQUIRE(cpu.c == 0U);
    REQUIRE(cpu.v == 1U);
}

TEST_CASE("M21 SMULH destination aliases preserve both source reads")
{
    const std::array<std::uint32_t, 3> opcodes{
        smulh_opcode(1U, 1U, 2U), // smulh x1, x1, x2
        smulh_opcode(2U, 1U, 2U), // smulh x2, x1, x2
        smulh_opcode(3U, 3U, 3U), // smulh x3, x3, x3
    };
    const std::array<std::uint8_t, 3> destinations{1U, 2U, 3U};
    for (std::size_t index = 0U; index < opcodes.size(); ++index)
    {
        auto fixture = make_fixture(opcodes[index], 0x210000U + index * 0x1000U);
        REQUIRE(fixture);
        runtime::CpuState cpu;
        cpu.x[1] = 0x123456789abcdef0ULL;
        cpu.x[2] = 0xedcba98765432110ULL;
        cpu.x[3] = 0xffffffffffffffffULL;
        runtime::RuntimeContext context{&fixture.value().memory};
        REQUIRE(interpreter::execute(fixture.value().function, cpu, context));
        const auto expected = index == 2U ? 0x0000000000000000ULL : 0xfeb49923cc095323ULL;
        REQUIRE(cpu.x[destinations[index]] == expected);
    }
}

#ifdef TOTKRECOMP_HAS_LLVM
TEST_CASE("M21 LLVM SMULH agrees with the portable interpreter for all sign combinations")
{
    const auto function = make_ir_function();
    REQUIRE(function);
    const auto backend = codegen::LlvmBackend::create();
    REQUIRE(backend);

    for (const auto& vector : kVectors)
    {
        runtime::CpuState interpreter_cpu;
        interpreter_cpu.x[1] = vector.left;
        interpreter_cpu.x[2] = vector.right;
        runtime::RuntimeContext interpreter_context;
        const auto interpreted = interpreter::execute(
            function.value(), interpreter_cpu, interpreter_context);
        REQUIRE(interpreted);

        runtime::CpuState llvm_cpu;
        llvm_cpu.x[1] = vector.left;
        llvm_cpu.x[2] = vector.right;
        runtime::RuntimeContext llvm_context;
        const auto lowered = backend.value()->execute(function.value(), llvm_cpu, llvm_context);
        INFO(vector.name);
        REQUIRE(lowered);
        REQUIRE(llvm_cpu.x[0] == vector.expected);
        REQUIRE(llvm_cpu.x[0] == interpreter_cpu.x[0]);
        REQUIRE(llvm_cpu.n == interpreter_cpu.n);
        REQUIRE(llvm_cpu.z == interpreter_cpu.z);
        REQUIRE(llvm_cpu.c == interpreter_cpu.c);
        REQUIRE(llvm_cpu.v == interpreter_cpu.v);
    }
}
#endif
