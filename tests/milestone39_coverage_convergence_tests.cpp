#include "switchrecomp/aarch64/decoder.hpp"
#include "switchrecomp/analysis/coverage.hpp"
#include "switchrecomp/analysis/cfg_analyzer.hpp"
#include "switchrecomp/interpreter/interpreter.hpp"
#include "switchrecomp/ir/verifier.hpp"
#include "switchrecomp/lifter/lifter.hpp"
#include "switchrecomp/runtime/context.hpp"
#include "switchrecomp/runtime/cpu_state.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
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
    const auto bytes = code(words);
    memory::GuestMemory memory;
    const auto mapped = memory.map(code_address, std::span<const std::byte>(bytes),
                                   memory::GuestMemoryPermissions::Read |
                                       memory::GuestMemoryPermissions::Execute,
                                   "m39.text", memory::GuestRegionKind::Text);
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

TEST_CASE("M39 MOVI and MVNI modified immediates normalize every measured form")
{
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);
    const auto movi_d = decoder.value()->decode(code_address, 0x2f00e420U);
    const auto mvni_h = decoder.value()->decode(code_address, 0x6f04a40fU);
    const auto mvni_msl = decoder.value()->decode(code_address, 0x6f03c7f0U);
    REQUIRE(movi_d);
    REQUIRE(mvni_h);
    REQUIRE(mvni_msl);
    REQUIRE(movi_d.value().simd_operation == aarch64::SimdOperation::Movi);
    REQUIRE(movi_d.value().operands[0].arrangement == aarch64::VectorArrangement::D1);
    REQUIRE(movi_d.value().operands[1].immediate == static_cast<std::int64_t>(0x0101010101010101ULL));
    REQUIRE(mvni_h.value().simd_operation == aarch64::SimdOperation::Mvni);
    REQUIRE(mvni_h.value().operands[0].arrangement == aarch64::VectorArrangement::H8);
    REQUIRE(mvni_h.value().operands[1].immediate == static_cast<std::int64_t>(0x7fff7fff7fff7fffULL));
    REQUIRE(mvni_msl.value().simd_operation == aarch64::SimdOperation::Mvni);
    REQUIRE(mvni_msl.value().operands[1].immediate == static_cast<std::int64_t>(0xffff8000ffff8000ULL));
    REQUIRE(lifter::is_instruction_liftable(movi_d.value()));
    REQUIRE(lifter::is_instruction_liftable(mvni_h.value()));
    REQUIRE(lifter::is_instruction_liftable(mvni_msl.value()));
}

TEST_CASE("M39 MOVI D1 and MVNI execute exact replicated bits")
{
    auto movi = lift_program({0x2f00e420U, ret});
    REQUIRE(movi);
    runtime::CpuState movi_cpu;
    movi_cpu.vreg[0] = runtime::Vector128{0U, std::numeric_limits<std::uint64_t>::max()};
    REQUIRE(execute_program(movi.value(), movi_cpu));
    REQUIRE(movi_cpu.vreg[0] == (runtime::Vector128{0x0101010101010101ULL, 0U}));

    auto mvni = lift_program({0x6f03c7f0U, ret});
    REQUIRE(mvni);
    runtime::CpuState mvni_cpu;
    REQUIRE(execute_program(mvni.value(), mvni_cpu));
    REQUIRE(mvni_cpu.vreg[16] == (runtime::Vector128{0xffff8000ffff8000ULL,
                                                     0xffff8000ffff8000ULL}));
}

TEST_CASE("M39 pairwise FADD lifts and preserves pairwise source ordering")
{
    auto two = lift_program({0x2e22d420U, ret}); // faddp v0.2s, v1.2s, v2.2s
    REQUIRE(two);
    runtime::CpuState two_cpu;
    two_cpu.vreg[1] = runtime::Vector128{0x402000003fc00000ULL, 0U}; // 1.5, 2.5
    two_cpu.vreg[2] = runtime::Vector128{0x40e0000040a00000ULL, 0U}; // 5.0, 7.0
    REQUIRE(execute_program(two.value(), two_cpu));
    REQUIRE(two_cpu.vreg[0] == (runtime::Vector128{0x4140000040800000ULL, 0U})); // 4, 12

    auto four = lift_program({0x6e25d483U, ret}); // faddp v3.4s, v4.4s, v5.4s
    REQUIRE(four);
    runtime::CpuState four_cpu;
    four_cpu.vreg[4] = runtime::Vector128{0x4080000040000000ULL, 0x4100000040c00000ULL};
    four_cpu.vreg[5] = runtime::Vector128{0x4140000041200000ULL, 0x4180000041600000ULL};
    REQUIRE(execute_program(four.value(), four_cpu));
    REQUIRE(four_cpu.vreg[3].lo == 0x4160000040c00000ULL);
    REQUIRE(four_cpu.vreg[3].hi == 0x41f0000041b00000ULL);
}

TEST_CASE("M39 BIF BIT and BSL implement architectural bit selection")
{
    constexpr auto destination = runtime::Vector128{0xaaaaaaaaaaaaaaaaULL, 0x5555555555555555ULL};
    constexpr auto mask = runtime::Vector128{0xffff0000ffff0000ULL, 0x00ffff00ff00ffffULL};
    constexpr auto value = runtime::Vector128{0x123456789abcdef0ULL, 0xfedcba9876543210ULL};

    auto bif = lift_program({0x6ee81ce6U, ret}); // bif v6.16b, v7.16b, v8.16b
    REQUIRE(bif);
    runtime::CpuState bif_cpu;
    bif_cpu.vreg[6] = destination;
    bif_cpu.vreg[7] = mask;
    bif_cpu.vreg[8] = value;
    REQUIRE(execute_program(bif.value(), bif_cpu));
    REQUIRE(bif_cpu.vreg[6] == (runtime::Vector128{(destination.lo & mask.lo) | (value.lo & ~mask.lo),
                                                     (destination.hi & mask.hi) | (value.hi & ~mask.hi)}));

    auto bit = lift_program({0x2eab1d49U, ret}); // bit v9.8b, v10.8b, v11.8b
    REQUIRE(bit);
    runtime::CpuState bit_cpu;
    bit_cpu.vreg[9] = destination;
    bit_cpu.vreg[10] = mask;
    bit_cpu.vreg[11] = value;
    REQUIRE(execute_program(bit.value(), bit_cpu));
    REQUIRE(bit_cpu.vreg[9] == (runtime::Vector128{(destination.lo & ~mask.lo) | (value.lo & mask.lo), 0U}));

    auto bsl = lift_program({0x6e6e1dacU, ret}); // bsl v12.16b, v13.16b, v14.16b
    REQUIRE(bsl);
    runtime::CpuState bsl_cpu;
    bsl_cpu.vreg[12] = destination;
    bsl_cpu.vreg[13] = mask;
    bsl_cpu.vreg[14] = value;
    REQUIRE(execute_program(bsl.value(), bsl_cpu));
    REQUIRE(bsl_cpu.vreg[12] == (runtime::Vector128{(destination.lo & mask.lo) | (value.lo & ~destination.lo),
                                                      (destination.hi & mask.hi) | (value.hi & ~destination.hi)}));
}

TEST_CASE("M39 UDF decodes, lifts to a trap, and remains distinct from unsupported BRK")
{
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);
    const auto udf = decoder.value()->decode(code_address, 0x00000000U);
    const auto brk = decoder.value()->decode(code_address, 0xd4200000U);
    REQUIRE(udf);
    REQUIRE(brk);
    REQUIRE(udf.value().id == aarch64::InstructionId::Udf);
    REQUIRE(udf.value().control_flow.kind == aarch64::ControlFlowKind::Trap);
    REQUIRE(lifter::is_instruction_liftable(udf.value()));
    REQUIRE_FALSE(lifter::is_instruction_liftable(brk.value()));

    auto program = lift_program({0U, ret});
    REQUIRE(program);
    REQUIRE(ir::verify(program.value().function));
    runtime::CpuState cpu;
    runtime::RuntimeContext context{&program.value().memory};
    interpreter::InterpreterFrame frame;
    const auto execution = interpreter::execute_until_boundary(
        program.value().function, cpu, context, frame);
    REQUIRE(execution);
    REQUIRE(execution.value().status == runtime::ExecutionStatus::Trapped);
    REQUIRE(execution.value().boundary.kind == runtime::ExecutionBoundaryKind::Trap);
}

TEST_CASE("M39 coverage scanner recognizes only normalized complete families")
{
    constexpr std::array words{
        0x2f00e420U, 0x6f03c7f0U, 0x2e22d420U, 0x6ee81ce6U, 0U, ret, 0xd4200000U};
    const auto bytes = code({0x2f00e420U, 0x6f03c7f0U, 0x2e22d420U, 0x6ee81ce6U, 0U, ret,
                             0xd4200000U});
    memory::GuestMemory memory;
    REQUIRE(memory.map(code_address, std::span<const std::byte>(bytes),
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "m39.coverage", memory::GuestRegionKind::Text));
    const auto report = analysis::scan_coverage(
        memory, code_address, static_cast<memory::GuestSize>(bytes.size()), "m39");
    const auto parallel_report = analysis::scan_coverage(
        memory, code_address, static_cast<memory::GuestSize>(bytes.size()), "m39",
        analysis::CoverageOptions{4'000'000U, 4U});
    REQUIRE(report);
    REQUIRE(parallel_report);
    REQUIRE(analysis::render_coverage_json(report.value()) ==
            analysis::render_coverage_json(parallel_report.value()));
    REQUIRE(report.value().decoded == words.size());
    REQUIRE(report.value().decode_failures == 0U);
    REQUIRE(report.value().unsupported == 1U);
    std::size_t direct_liftable = 0U;
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);
    for (std::size_t index = 0U; index < words.size(); ++index)
    {
        const auto decoded = decoder.value()->decode(
            code_address + static_cast<memory::GuestAddress>(index * 4U), words[index]);
        REQUIRE(decoded);
        direct_liftable += lifter::is_instruction_liftable(decoded.value()) ? 1U : 0U;
    }
    REQUIRE(report.value().liftable == direct_liftable);
}
