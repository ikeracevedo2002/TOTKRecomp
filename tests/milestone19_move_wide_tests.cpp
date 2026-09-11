#include "switchrecomp/aarch64/decoder.hpp"
#include "switchrecomp/analysis/cfg_analyzer.hpp"
#include "switchrecomp/codegen/llvm_backend.hpp"
#include "switchrecomp/interpreter/interpreter.hpp"
#include "switchrecomp/ir/builder.hpp"
#include "switchrecomp/ir/verifier.hpp"
#include "switchrecomp/lifter/lifter.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <array>
#include <initializer_list>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace
{

using namespace switchrecomp;

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
    analysis::ControlFlowGraph cfg;
    ir::Function function;
};

[[nodiscard]] Result<Fixture> make_fixture(std::initializer_list<std::uint32_t> values,
                                           memory::GuestAddress address = 0x100000U)
{
    Fixture fixture;
    const auto bytes = words(values);
    const auto mapped = fixture.memory.map(
        address, std::span<const std::byte>(bytes),
        memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Execute,
        "m19.text", memory::GuestRegionKind::Text);
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

[[nodiscard]] Result<analysis::ControlFlowGraph> make_cfg(
    std::initializer_list<std::uint32_t> values, memory::GuestMemory& memory,
    memory::GuestAddress address = 0x100000U)
{
    const auto bytes = words(values);
    const auto mapped = memory.map(
        address, std::span<const std::byte>(bytes),
        memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Execute,
        "m19.cfg.text", memory::GuestRegionKind::Text);
    if (!mapped) return Result<analysis::ControlFlowGraph>::failure(mapped.error());
    analysis::AnalysisOptions options;
    options.allowed_code_range = analysis::GuestAddressRange{
        address, static_cast<memory::GuestSize>(bytes.size())};
    return analysis::analyze_control_flow(memory, address, options);
}

[[nodiscard]] Result<runtime::ExecutionResult> run(Fixture& fixture, runtime::CpuState& cpu)
{
    runtime::RuntimeContext context{&fixture.memory};
    return interpreter::execute(fixture.function, cpu, context);
}

} // namespace

TEST_CASE("M19 decoder normalizes move-wide immediates and legal shifts")
{
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);

    struct Expected
    {
        std::uint32_t opcode;
        aarch64::InstructionId id;
        aarch64::RegisterWidth width;
        std::uint8_t destination;
        std::int64_t immediate;
        std::uint8_t shift;
    };
    // These are independently assembled A64 encodings; the test does not
    // derive expected fields with project decoder helpers.
    const std::array<Expected, 7> cases{{
        {0x52800000U, aarch64::InstructionId::Movz, aarch64::RegisterWidth::W32, 0U, 0, 0U},
        {0x52800040U, aarch64::InstructionId::Movz, aarch64::RegisterWidth::W32, 0U, 2, 0U},
        {0x52bfffe3U, aarch64::InstructionId::Movz, aarch64::RegisterWidth::W32, 3U, 0xffff, 16U},
        {0xd2824681U, aarch64::InstructionId::Movz, aarch64::RegisterWidth::X64, 1U, 0x1234, 0U},
        {0xd2a24681U, aarch64::InstructionId::Movz, aarch64::RegisterWidth::X64, 1U, 0x1234, 16U},
        {0xd2c24681U, aarch64::InstructionId::Movz, aarch64::RegisterWidth::X64, 1U, 0x1234, 32U},
        {0xd2e24681U, aarch64::InstructionId::Movz, aarch64::RegisterWidth::X64, 1U, 0x1234, 48U},
    }};

    for (const auto& expected : cases)
    {
        const auto decoded = decoder.value()->decode(0x1000U, expected.opcode);
        REQUIRE(decoded);
        REQUIRE(decoded.value().opcode == expected.opcode);
        REQUIRE(decoded.value().id == expected.id);
        REQUIRE(decoded.value().operands.size() == 2U);
        REQUIRE(decoded.value().operands[0].kind == aarch64::OperandKind::Register);
        REQUIRE(decoded.value().operands[0].reg.kind == aarch64::RegisterKind::General);
        REQUIRE(decoded.value().operands[0].reg.width == expected.width);
        REQUIRE(decoded.value().operands[0].reg.index == expected.destination);
        REQUIRE(decoded.value().operands[1].kind == aarch64::OperandKind::Immediate);
        REQUIRE(decoded.value().operands[1].immediate == expected.immediate);
        REQUIRE(decoded.value().operands[1].shift == expected.shift);
        REQUIRE(decoded.value().operands[1].shift_kind ==
                (expected.shift == 0U ? aarch64::ShiftKind::None : aarch64::ShiftKind::Lsl));
    }
}

TEST_CASE("M19 MOVZ executes all architectural W and X forms")
{
    struct Expected
    {
        std::uint32_t opcode;
        std::uint8_t destination;
        std::uint64_t value;
    };
    const std::array<Expected, 8> cases{{
        {0x52800000U, 0U, 0x0000000000000000ULL},
        {0x52800020U, 0U, 0x0000000000000001ULL},
        {0x529fffe0U, 0U, 0x000000000000ffffULL},
        {0x52bfffe3U, 3U, 0x00000000ffff0000ULL},
        {0xd2824681U, 1U, 0x0000000000001234ULL},
        {0xd2a24681U, 1U, 0x0000000012340000ULL},
        {0xd2c24681U, 1U, 0x0000123400000000ULL},
        {0xd2e24681U, 1U, 0x1234000000000000ULL},
    }};

    for (const auto& expected : cases)
    {
        auto fixture = make_fixture({expected.opcode, 0xd65f03c0U}); // ret
        REQUIRE(fixture);
        runtime::CpuState cpu;
        cpu.x[expected.destination] = std::numeric_limits<std::uint64_t>::max();
        REQUIRE(run(fixture.value(), cpu));
        REQUIRE(cpu.x[expected.destination] == expected.value);
        REQUIRE(cpu.pc == 0x100004U);
    }
}

TEST_CASE("M19 MOVN and MOVK share validated move-wide semantics")
{
    auto movn_w = make_fixture({0x12a24680U, 0xd65f03c0U}); // movn w0, #0x1234, lsl #16
    REQUIRE(movn_w);
    runtime::CpuState movn_w_cpu;
    movn_w_cpu.x[0] = std::numeric_limits<std::uint64_t>::max();
    REQUIRE(run(movn_w.value(), movn_w_cpu));
    REQUIRE(movn_w_cpu.x[0] == 0x00000000edcbffffULL);

    auto movn_x = make_fixture({0x92800000U, 0xd65f03c0U}); // movn x0, #0
    REQUIRE(movn_x);
    runtime::CpuState movn_x_cpu;
    REQUIRE(run(movn_x.value(), movn_x_cpu));
    REQUIRE(movn_x_cpu.x[0] == std::numeric_limits<std::uint64_t>::max());

    const std::array<std::uint32_t, 4> movk{{0xf29579a0U, 0xf2b579a0U,
                                               0xf2d579a0U, 0xf2f579a0U}};
    const std::array<std::uint64_t, 4> expected{{0x000000000000abcdULL,
                                                  0x00000000abcd0001ULL,
                                                  0x0000abcd00000001ULL,
                                                  0xabcd000000000001ULL}};
    for (std::size_t index = 0U; index < movk.size(); ++index)
    {
        auto fixture = make_fixture({0xd2800020U, movk[index], 0xd65f03c0U});
        REQUIRE(fixture);
        runtime::CpuState cpu;
        cpu.x[0] = std::numeric_limits<std::uint64_t>::max();
        REQUIRE(run(fixture.value(), cpu));
        REQUIRE(cpu.x[0] == expected[index]);
    }

    auto movk_w = make_fixture({0x729fffe0U, 0xd65f03c0U}); // movk w0, #0xffff
    REQUIRE(movk_w);
    runtime::CpuState movk_w_cpu;
    movk_w_cpu.x[0] = std::numeric_limits<std::uint64_t>::max();
    REQUIRE(run(movk_w.value(), movk_w_cpu));
    REQUIRE(movk_w_cpu.x[0] == 0x00000000ffffffffULL);

    auto movk_w_shifted = make_fixture({0x72bfffe0U, 0xd65f03c0U}); // movk w0, #0xffff, lsl #16
    REQUIRE(movk_w_shifted);
    runtime::CpuState movk_w_shifted_cpu;
    movk_w_shifted_cpu.x[0] = 0xfeedface00000001ULL;
    REQUIRE(run(movk_w_shifted.value(), movk_w_shifted_cpu));
    REQUIRE(movk_w_shifted_cpu.x[0] == 0x00000000ffff0001ULL);
}

TEST_CASE("M19 diagnostic lifting isolates an unsupported CFG block")
{
    memory::GuestMemory memory;
    const auto cfg = make_cfg({0xb4000060U, 0x7a400900U, 0xd65f03c0U,
                               0x52800040U, 0xd65f03c0U}, memory);
    REQUIRE(cfg);
    lifter::LiftOptions options;
    options.stop_at_unsupported_instruction = true;
    const auto function = lifter::lift_function(cfg.value(), options);
    REQUIRE(function);

    runtime::CpuState cpu;
    cpu.x[0] = 0U; // cbz takes the MOVZ block at +0xC.
    runtime::RuntimeContext context{&memory};
    const auto result = interpreter::execute(function.value(), cpu, context);
    REQUIRE(result);
    REQUIRE(result.value().status == runtime::ExecutionStatus::Returned);
    REQUIRE(cpu.x[0] == 2U);
}

TEST_CASE("M19 move-wide malformed normalized forms fail closed")
{
    const auto mutate_and_lift = [](auto&& mutate) {
        memory::GuestMemory memory;
        auto cfg = make_cfg({0x52800040U, 0xd65f03c0U}, memory);
        REQUIRE(cfg);
        auto& instruction = cfg.value().blocks.begin()->second.instructions.front();
        mutate(instruction);
        return lifter::lift_function(cfg.value());
    };

    SECTION("W shift outside the legal set")
    {
        const auto lifted = mutate_and_lift([](auto& instruction) {
            instruction.operands[1].shift_kind = aarch64::ShiftKind::Lsl;
            instruction.operands[1].shift = 32U;
        });
        REQUIRE_FALSE(lifted);
        REQUIRE(lifted.error().code == ErrorCode::UnsupportedInstruction);
    }
    SECTION("immediate outside the architectural field")
    {
        const auto lifted = mutate_and_lift([](auto& instruction) {
            instruction.operands[1].immediate = 0x10000;
        });
        REQUIRE_FALSE(lifted);
        REQUIRE(lifted.error().code == ErrorCode::UnsupportedInstruction);
    }
    SECTION("non-general destination register")
    {
        const auto lifted = mutate_and_lift([](auto& instruction) {
            instruction.operands[0].reg.kind = aarch64::RegisterKind::Vector;
            instruction.operands[0].reg.width = aarch64::RegisterWidth::Q128;
        });
        REQUIRE_FALSE(lifted);
        REQUIRE(lifted.error().code == ErrorCode::UnsupportedInstruction);
    }
    SECTION("unsupported immediate shift kind")
    {
        const auto lifted = mutate_and_lift([](auto& instruction) {
            instruction.operands[1].shift_kind = aarch64::ShiftKind::Lsr;
        });
        REQUIRE_FALSE(lifted);
        REQUIRE(lifted.error().code == ErrorCode::UnsupportedInstruction);
    }
}

TEST_CASE("M19 verifier rejects malformed register-width IR")
{
    const auto verify_write = [](ir::Type value_type, ir::GuestRegister reg) {
        ir::Function function("malformed_move_wide", 0x1000U);
        const auto block = function.add_block(0x1000U, "block_0");
        function.set_entry_block(block);
        ir::Builder builder(function);
        REQUIRE(builder.set_insert_block(block));
        const auto value = builder.constant(value_type, 2U);
        REQUIRE(value);
        ir::Instruction write;
        write.opcode = ir::Opcode::WriteRegister;
        write.result_type = ir::void_type();
        write.operands = {value.value()};
        write.reg = reg;
        REQUIRE(builder.emit_void(write));
        ir::Terminator ret;
        ret.kind = ir::TerminatorKind::Return;
        REQUIRE(builder.set_terminator(ret));
        return ir::verify(function);
    };

    REQUIRE_FALSE(verify_write(ir::i64_type(),
                               ir::GuestRegister{ir::RegisterWidth::W32, 0U, false, false}));
    REQUIRE_FALSE(verify_write(ir::i32_type(),
                               ir::GuestRegister{ir::RegisterWidth::X64, 0U, false, false}));
    REQUIRE_FALSE(verify_write(ir::i32_type(),
                               ir::GuestRegister{ir::RegisterWidth::W32, 31U, false, false}));
    REQUIRE_FALSE(verify_write(ir::i32_type(),
                               ir::GuestRegister{static_cast<ir::RegisterWidth>(99), 0U, false, false}));

    ir::Function malformed_constant("malformed_move_wide_constant", 0x1000U);
    const auto block = malformed_constant.add_block(0x1000U, "block_0");
    malformed_constant.set_entry_block(block);
    ir::Builder builder(malformed_constant);
    REQUIRE(builder.set_insert_block(block));
    ir::Instruction constant;
    constant.opcode = ir::Opcode::Constant;
    constant.result_type = ir::i64_type();
    constant.constant = 2U;
    constant.constant_high = 1U;
    REQUIRE(builder.emit(constant));
    ir::Terminator ret;
    ret.kind = ir::TerminatorKind::Return;
    REQUIRE(builder.set_terminator(ret));
    REQUIRE_FALSE(ir::verify(malformed_constant));
}

#ifdef TOTKRECOMP_HAS_LLVM
TEST_CASE("M19 LLVM move-wide execution matches the interpreter")
{
    const std::array<std::uint32_t, 7> move_wide{{
        0x52800040U, 0x52bfffe3U, 0xd2824681U, 0xd2a24681U,
        0xd2c24681U, 0xd2e24681U, 0x12a24680U}};
    const auto backend = codegen::LlvmBackend::create();
    REQUIRE(backend);
    for (const auto opcode : move_wide)
    {
        auto fixture = make_fixture({opcode, 0xd65f03c0U});
        REQUIRE(fixture);
        runtime::CpuState interpreter_cpu;
        interpreter_cpu.x[0] = std::numeric_limits<std::uint64_t>::max();
        interpreter_cpu.x[1] = std::numeric_limits<std::uint64_t>::max();
        runtime::RuntimeContext interpreter_context{&fixture.value().memory};
        const auto interpreted = interpreter::execute(fixture.value().function,
                                                      interpreter_cpu, interpreter_context);
        REQUIRE(interpreted);

        runtime::CpuState llvm_cpu;
        llvm_cpu.x[0] = std::numeric_limits<std::uint64_t>::max();
        llvm_cpu.x[1] = std::numeric_limits<std::uint64_t>::max();
        runtime::RuntimeContext llvm_context{&fixture.value().memory};
        const auto native = backend.value()->execute(fixture.value().function, llvm_cpu,
                                                     llvm_context);
        REQUIRE(native);
        REQUIRE(llvm_cpu.x == interpreter_cpu.x);
        REQUIRE(llvm_cpu.sp == interpreter_cpu.sp);
        REQUIRE(llvm_cpu.pc == interpreter_cpu.pc);
        REQUIRE(llvm_cpu.n == interpreter_cpu.n);
        REQUIRE(llvm_cpu.z == interpreter_cpu.z);
        REQUIRE(llvm_cpu.c == interpreter_cpu.c);
        REQUIRE(llvm_cpu.v == interpreter_cpu.v);
        REQUIRE(native.value().status == interpreted.value().status);
    }
}
#endif
