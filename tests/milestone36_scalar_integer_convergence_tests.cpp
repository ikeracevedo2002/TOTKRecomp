#include "switchrecomp/aarch64/decoder.hpp"
#include "switchrecomp/analysis/indirect_target.hpp"
#ifdef TOTKRECOMP_HAS_LLVM
#include "switchrecomp/codegen/llvm_backend.hpp"
#endif
#include "switchrecomp/analysis/cfg_analyzer.hpp"
#include "switchrecomp/interpreter/interpreter.hpp"
#include "switchrecomp/ir/builder.hpp"
#include "switchrecomp/ir/verifier.hpp"
#include "switchrecomp/lifter/lifter.hpp"
#include "switchrecomp/runtime/context.hpp"
#include "switchrecomp/runtime/cpu_state.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace
{

using namespace switchrecomp;

constexpr memory::GuestAddress code_address = 0x7a000U;
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
                                   "m36.scalar_integer.text", memory::GuestRegionKind::Text);
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

[[nodiscard]] std::uint64_t low_mask(unsigned int width, unsigned int count) noexcept
{
    const auto full = width == 32U ? std::uint64_t{0xffffffffU}
                                   : std::numeric_limits<std::uint64_t>::max();
    if (count >= width) return full;
    if (count == 0U) return 0U;
    return (std::uint64_t{1U} << count) - 1U;
}

[[nodiscard]] std::uint64_t rotate_right(std::uint64_t value, unsigned int width,
                                         unsigned int amount) noexcept
{
    const auto full = low_mask(width, width);
    value &= full;
    amount %= width;
    return amount == 0U ? value : ((value >> amount) | (value << (width - amount))) & full;
}

[[nodiscard]] std::uint64_t bitfield_expected(std::uint32_t opcode, std::uint64_t source,
                                              std::uint64_t destination,
                                              aarch64::InstructionId id) noexcept
{
    const auto width = ((opcode >> 31U) & 1U) != 0U ? 64U : 32U;
    const auto immr = static_cast<unsigned int>((opcode >> 16U) & 0x3fU);
    const auto imms = static_cast<unsigned int>((opcode >> 10U) & 0x3fU);
    const auto full = low_mask(width, width);
    const auto write_mask = rotate_right(low_mask(width, imms + 1U), width, immr);
    const auto count = ((imms - immr) & (width - 1U)) + 1U;
    const auto test_mask = low_mask(width, count);
    const auto inserted = rotate_right(source, width, immr) & write_mask;
    if (id == aarch64::InstructionId::Ubfm)
    {
        return inserted & test_mask;
    }
    if (id == aarch64::InstructionId::Sbfm)
    {
        const auto top = ((source >> imms) & 1U) != 0U ? full : 0U;
        return (top & ~test_mask) | (inserted & test_mask);
    }
    const auto combined = (destination & ~write_mask) | inserted;
    return (combined & test_mask) | (destination & ~test_mask);
}

} // namespace

TEST_CASE("M36 refinement batches publish deterministically and count avoided rebuilds")
{
    const auto make_observation = [](memory::GuestAddress target) {
        analysis::ObservedIndirectTarget observed;
        observed.source_module = "m36";
        observed.source_function = 0x1000U;
        observed.source_pc = 0x1004U;
        observed.control_flow = analysis::IndirectControlFlowKind::Call;
        observed.target_register = "x8";
        observed.target = target;
        observed.target_module = "m36";
        return observed;
    };
    const auto make_assessment = [&](memory::GuestAddress target) {
        analysis::IndirectTargetAssessment assessment;
        assessment.observed = make_observation(target);
        assessment.validation.target_module = "m36";
        assessment.validation.structurally_eligible = true;
        assessment.decision.kind = analysis::IndirectTargetDecisionKind::TrustedNewEntry;
        assessment.decision.eligible_for_promotion = true;
        return assessment;
    };
    const auto first = make_assessment(0x2000U);
    const auto second = make_assessment(0x3000U);
    analysis::IndirectTargetRefinementWorklist worklist;
    REQUIRE(worklist.begin_round());
    REQUIRE(worklist.observe(first).newly_unique_candidate);
    REQUIRE(worklist.observe(second).newly_unique_candidate);
    const auto first_id = analysis::indirect_target_candidate_identity(first.observed);
    const auto second_id = analysis::indirect_target_candidate_identity(second.observed);
    REQUIRE(worklist.begin_candidate_assessment(first_id));
    REQUIRE(worklist.begin_candidate_assessment(second_id));
    const analysis::IndirectTargetRefinementAnalysisWork work{
        "m36", 2U, 0U, 4U, 8U, 2U, 2U, 32U, 1U, 0U, 1U};
    const std::array<analysis::IndirectTargetCandidateIdentity, 2U> identities{
        first_id, second_id};
    REQUIRE(worklist.can_commit_batch_refinement(identities, work, 1U, 0U));
    worklist.record_batch_promotion(identities, 1U, 0U, work, 4U, 4U, 2U, 1U, 1U, 0U);
    worklist.end_round();
    const auto summary = worklist.summary();
    REQUIRE(summary.successful_promotions == 2U);
    REQUIRE(summary.map_rebuilds == 1U);
    REQUIRE(summary.refinement_batches == 1U);
    REQUIRE(summary.batch_candidates == 2U);
    REQUIRE(summary.singleton_batches == 0U);
    REQUIRE(summary.rebuilds_avoided == 1U);
    REQUIRE(summary.max_batch_width == 2U);
    REQUIRE(summary.finalized_functions_reused == 4U);
    REQUIRE(summary.cfgs_reused == 4U);
    REQUIRE(summary.functions_rebuilt == 2U);
    REQUIRE(summary.map_generation == 1U);
    REQUIRE(summary.pending_candidate_count == 0U);
}

TEST_CASE("M36 measured word normalizes as register-controlled LSL")
{
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);
    const auto decoded = decoder.value()->decode(code_address, 0x1ad32108U);
    REQUIRE(decoded);
    REQUIRE(decoded.value().id == aarch64::InstructionId::Lsl);
    REQUIRE(decoded.value().normalized);
    REQUIRE(decoded.value().disassembly == "lsl w8, w8, w19");
    REQUIRE(decoded.value().operands.size() == 3U);
    REQUIRE(decoded.value().operands[0].reg.width == aarch64::RegisterWidth::W32);
    REQUIRE(decoded.value().operands[1].reg.width == aarch64::RegisterWidth::W32);
    REQUIRE(decoded.value().operands[2].reg.width == aarch64::RegisterWidth::W32);
    REQUIRE(decoded.value().operands[2].kind == aarch64::OperandKind::Register);
    REQUIRE(lifter::is_instruction_liftable(decoded.value()));
}

TEST_CASE("M36 variable shifts mask register amounts and preserve unrelated state")
{
    struct ShiftCase
    {
        std::uint32_t base;
        ir::Opcode operation;
        std::uint32_t expected;
    };
    constexpr ShiftCase cases[]{
        {0x1ac02000U, ir::Opcode::ShiftLeft, 0x80000001U},
        {0x1ac02400U, ir::Opcode::LogicalShiftRight, 0x80000001U},
        {0x1ac02800U, ir::Opcode::ArithmeticShiftRight, 0x80000001U},
        {0x1ac02c00U, ir::Opcode::RotateRight, 0x80000001U},
    };
    for (const auto& test : cases)
    {
        auto program = lift_program({test.base | (2U << 16U) | (1U << 5U), ret});
        REQUIRE(program);
        REQUIRE(ir::verify(program.value().function));
        runtime::CpuState cpu;
        cpu.x[0] = 0xaaaaaaaa00000000ULL;
        cpu.x[1] = 0x80000001U;
        cpu.x[2] = 32U;
        cpu.fpcr = 0x12345678U;
        cpu.fpsr = 0x0000001fU;
        cpu.n = 1U;
        cpu.z = 0U;
        cpu.c = 1U;
        cpu.v = 1U;
        const auto before = cpu;
        REQUIRE(execute_program(program.value(), cpu));
        REQUIRE(cpu.x[0] == (test.expected == 0xffffffffU ? 0xffffffffULL : test.expected));
        REQUIRE(cpu.x[1] == before.x[1]);
        REQUIRE(cpu.x[2] == before.x[2]);
        REQUIRE(cpu.fpcr == before.fpcr);
        REQUIRE(cpu.fpsr == before.fpsr);
        REQUIRE(cpu.n == before.n);
        REQUIRE(cpu.z == before.z);
        REQUIRE(cpu.c == before.c);
        REQUIRE(cpu.v == before.v);
    }

    auto arithmetic = lift_program({0x1ac22820U, ret});
    REQUIRE(arithmetic);
    runtime::CpuState arithmetic_cpu;
    arithmetic_cpu.x[1] = 0x80000001U;
    arithmetic_cpu.x[2] = 31U;
    REQUIRE(execute_program(arithmetic.value(), arithmetic_cpu));
    REQUIRE(arithmetic_cpu.x[0] == 0xffffffffU);
}

TEST_CASE("M36 variable X shifts use six-bit architectural masking")
{
    constexpr std::uint32_t rorv_x0_x1_x2 = 0x9ac22c20U;
    auto program = lift_program({rorv_x0_x1_x2, ret});
    REQUIRE(program);
    runtime::CpuState cpu;
    cpu.x[1] = 0x8000000000000001ULL;
    cpu.x[2] = 64U;
    REQUIRE(execute_program(program.value(), cpu));
    REQUIRE(cpu.x[0] == 0x8000000000000001ULL);
}

TEST_CASE("M36 immediate aliases and EXTR share verified scalar semantics")
{
    struct Case
    {
        std::uint32_t opcode;
        aarch64::InstructionId id;
        std::uint64_t expected;
    };
    constexpr Case cases[]{
        {0x531d7020U, aarch64::InstructionId::Lsl, 0x00000008U},
        {0x53037c20U, aarch64::InstructionId::Lsr, 0x10000000U},
        {0x13037c20U, aarch64::InstructionId::Asr, 0xf0000000U},
        {0x13810c20U, aarch64::InstructionId::Ror, 0x30000000U},
        {0x13820c20U, aarch64::InstructionId::Extr, 0x20000000U},
    };
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);
    for (const auto& test : cases)
    {
        const auto decoded = decoder.value()->decode(code_address, test.opcode);
        REQUIRE(decoded);
        REQUIRE(decoded.value().id == test.id);
        REQUIRE(decoded.value().normalized);
        auto program = lift_program({test.opcode, ret});
        REQUIRE(program);
        runtime::CpuState cpu;
        cpu.x[0] = 0xaaaaaaaa00000000ULL;
        cpu.x[1] = 0x80000001U;
        cpu.x[2] = 0x00000002U;
        REQUIRE(execute_program(program.value(), cpu));
        REQUIRE(cpu.x[0] == test.expected);
    }
}

TEST_CASE("M36 UBFM, SBFM and BFM use wrapped architectural masks")
{
    constexpr std::uint32_t opcodes[]{
        0x53142020U, // ubfm w0, w1, #20, #8
        0x13142020U, // sbfm w0, w1, #20, #8
        0x33142020U, // bfm w0, w1, #20, #8
    };
    constexpr aarch64::InstructionId ids[]{
        aarch64::InstructionId::Ubfm,
        aarch64::InstructionId::Sbfm,
        aarch64::InstructionId::Bfm,
    };
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);
    for (std::size_t index = 0U; index < 3U; ++index)
    {
        const auto decoded = decoder.value()->decode(code_address, opcodes[index]);
        REQUIRE(decoded);
        REQUIRE(decoded.value().id == ids[index]);
        auto program = lift_program({opcodes[index], ret});
        REQUIRE(program);
        runtime::CpuState cpu;
        cpu.x[0] = 0xaaaaaaaaU;
        cpu.x[1] = 0x80000001U;
        REQUIRE(execute_program(program.value(), cpu));
        REQUIRE(cpu.x[0] == bitfield_expected(opcodes[index], cpu.x[1], 0xaaaaaaaaU, ids[index]));
    }
}

TEST_CASE("M36 malformed scalar shift IR is rejected by the verifier")
{
    ir::Function function("m36_malformed_shift", code_address);
    const auto block = function.add_block(code_address, "entry");
    function.set_entry_block(block);
    ir::Builder builder(function);
    REQUIRE(builder.set_insert_block(block));
    const auto value = builder.constant(ir::i32_type(), 1U,
                                        ir::SourceLocation{code_address, 0U, "m36"});
    const auto amount = builder.constant(ir::i64_type(), 1U,
                                         ir::SourceLocation{code_address, 0U, "m36"});
    REQUIRE(value);
    REQUIRE(amount);
    ir::Instruction shift;
    shift.opcode = ir::Opcode::ShiftLeft;
    shift.result_type = ir::i32_type();
    shift.operands = {value.value(), amount.value()};
    shift.source = ir::SourceLocation{code_address, 0U, "m36"};
    REQUIRE(builder.emit(std::move(shift)));
    ir::Terminator terminator;
    terminator.kind = ir::TerminatorKind::Return;
    terminator.source = ir::SourceLocation{code_address, 4U, "m36"};
    REQUIRE(builder.set_terminator(std::move(terminator)));
    REQUIRE_FALSE(ir::verify(function));
}

#ifdef TOTKRECOMP_HAS_LLVM
TEST_CASE("M36 variable LSL interpreter and LLVM execution agree")
{
    auto program = lift_program({0x1ac22020U, ret});
    REQUIRE(program);
    const auto backend = codegen::LlvmBackend::create();
    REQUIRE(backend);
    runtime::CpuState interpreted_cpu;
    interpreted_cpu.x[1] = 0x80000001U;
    interpreted_cpu.x[2] = 33U;
    runtime::RuntimeContext interpreted_context{&program.value().memory};
    REQUIRE(interpreter::execute(program.value().function, interpreted_cpu, interpreted_context));
    runtime::CpuState llvm_cpu;
    llvm_cpu.x[1] = 0x80000001U;
    llvm_cpu.x[2] = 33U;
    runtime::RuntimeContext llvm_context{&program.value().memory};
    REQUIRE(backend.value()->execute(program.value().function, llvm_cpu, llvm_context));
    REQUIRE(llvm_cpu.x == interpreted_cpu.x);
    REQUIRE(llvm_cpu.n == interpreted_cpu.n);
    REQUIRE(llvm_cpu.z == interpreted_cpu.z);
    REQUIRE(llvm_cpu.c == interpreted_cpu.c);
    REQUIRE(llvm_cpu.v == interpreted_cpu.v);
}
#endif
