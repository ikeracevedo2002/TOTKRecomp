#include "switchrecomp/aarch64/decoder.hpp"
#include "switchrecomp/analysis/coverage.hpp"
#include "switchrecomp/analysis/cfg_analyzer.hpp"
#include "switchrecomp/interpreter/interpreter.hpp"
#include "switchrecomp/lifter/lifter.hpp"
#include "switchrecomp/runtime/context.hpp"
#include "switchrecomp/runtime/cpu_state.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <initializer_list>
#include <span>
#include <utility>
#include <vector>

namespace
{

using namespace switchrecomp;
constexpr memory::GuestAddress code_address = 0x8000U;
constexpr memory::GuestAddress data_address = 0x10000U;
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
                                   "m41.text", memory::GuestRegionKind::Text);
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

[[nodiscard]] Result<LiftedProgram> lift_program_with_data(
    std::initializer_list<std::uint32_t> words, std::span<const std::byte> data)
{
    auto program = lift_program(words);
    if (!program) return program;
    const auto mapped = program.value().memory.map(
        data_address, data, memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Write,
        "m41.data", memory::GuestRegionKind::Data);
    if (!mapped) return Result<LiftedProgram>::failure(mapped.error());
    return program;
}

[[nodiscard]] Result<runtime::ExecutionResult> execute_program(
    LiftedProgram& program, runtime::CpuState& cpu)
{
    runtime::RuntimeContext context{&program.memory};
    return interpreter::execute(program.function, cpu, context);
}

} // namespace

TEST_CASE("M41 decoder and coverage normalize structure-load forms")
{
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);
    const auto ld1_lane = decoder.value()->decode(code_address, 0x0d408020U);
    const auto ld1_lane_h = decoder.value()->decode(code_address, 0x0d405820U);
    const auto ld1 = decoder.value()->decode(code_address, 0x4c407820U);
    const auto ld1r = decoder.value()->decode(code_address, 0x4d40c820U);
    const auto ld1r_post = decoder.value()->decode(code_address, 0x4ddfc820U);
    const auto ld2 = decoder.value()->decode(code_address, 0x4c408820U);
    const auto ld4 = decoder.value()->decode(code_address, 0x4c400820U);
    const auto wrapped = decoder.value()->decode(code_address, 0x4c40a83fU);
    REQUIRE(ld1_lane);
    REQUIRE(ld1_lane_h);
    REQUIRE(ld1);
    REQUIRE(ld1r);
    REQUIRE(ld1r_post);
    REQUIRE(ld2);
    REQUIRE(ld4);
    REQUIRE(ld1_lane.value().simd_operation == aarch64::SimdOperation::Ld1);
    REQUIRE(ld1_lane.value().operands[0].arrangement == aarch64::VectorArrangement::S2);
    REQUIRE(ld1_lane.value().operands[0].vector_index == 0);
    REQUIRE(ld1_lane_h.value().operands[0].arrangement == aarch64::VectorArrangement::H4);
    REQUIRE(ld1_lane_h.value().operands[0].vector_index == 3);
    REQUIRE(ld1.value().simd_operation == aarch64::SimdOperation::Ld1);
    REQUIRE(ld1.value().operands.size() == 2U);
    REQUIRE(ld1.value().operands[0].arrangement == aarch64::VectorArrangement::S4);
    REQUIRE(ld1r.value().simd_operation == aarch64::SimdOperation::Ld1r);
    REQUIRE(ld1r_post.value().operands[1].memory.displacement == 4);
    REQUIRE(ld2.value().simd_operation == aarch64::SimdOperation::Ld2);
    REQUIRE(ld2.value().operands.size() == 3U);
    REQUIRE(ld4.value().simd_operation == aarch64::SimdOperation::Ld4);
    REQUIRE(ld4.value().operands.size() == 5U);
    REQUIRE(wrapped.value().operands[0].reg.index == 31U);
    REQUIRE(wrapped.value().operands[1].reg.index == 0U);
    REQUIRE(lifter::is_instruction_liftable(ld1_lane.value()));
    REQUIRE(lifter::is_instruction_liftable(ld1.value()));
    REQUIRE(lifter::is_instruction_liftable(ld1r.value()));
    REQUIRE(lifter::is_instruction_liftable(ld2.value()));
    REQUIRE(lifter::is_instruction_liftable(ld4.value()));

    const auto bytes = code({0x0d408020U, 0x4c407820U, 0x4d40c820U, 0x4c408820U,
                             0x4c400820U, ret});
    memory::GuestMemory memory;
    REQUIRE(memory.map(code_address, std::span<const std::byte>(bytes),
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "m41.coverage", memory::GuestRegionKind::Text));
    const auto report = analysis::scan_coverage(
        memory, code_address, static_cast<memory::GuestSize>(bytes.size()), "m41");
    REQUIRE(report);
    REQUIRE(report.value().decoded == 6U);
    REQUIRE(report.value().liftable == 6U);
    REQUIRE(report.value().unsupported == 0U);
    REQUIRE(report.value().decode_failures == 0U);
}

TEST_CASE("M41 LD1 and LD1R preserve arrangement lanes and replicate every width")
{
    struct Case
    {
        std::uint32_t instruction;
        std::vector<std::byte> data;
        std::uint64_t expected_low;
        std::uint64_t expected_high;
    };
    const std::vector<Case> loads{
        {0x0c407020U,
         {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}, std::byte{6},
          std::byte{7}, std::byte{8}},
         0x0807060504030201ULL, 0xa5a5a5a5a5a5a5a5ULL},
        {0x4c407020U,
         {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}, std::byte{6},
          std::byte{7}, std::byte{8}, std::byte{9}, std::byte{10}, std::byte{11}, std::byte{12},
          std::byte{13}, std::byte{14}, std::byte{15}, std::byte{16}},
         0x0807060504030201ULL, 0x100f0e0d0c0b0a09ULL},
        {0x0c407820U,
         {std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}, std::byte{0x55},
          std::byte{0x66}, std::byte{0x77}, std::byte{static_cast<unsigned char>(0x88)}},
         0x8877665544332211ULL, 0xa5a5a5a5a5a5a5a5ULL},
        {0x4c407820,
         {std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}, std::byte{0x55},
          std::byte{0x66}, std::byte{0x77}, std::byte{static_cast<unsigned char>(0x88)},
          std::byte{static_cast<unsigned char>(0x99)}, std::byte{static_cast<unsigned char>(0xaa)},
          std::byte{static_cast<unsigned char>(0xbb)}, std::byte{static_cast<unsigned char>(0xcc)},
          std::byte{static_cast<unsigned char>(0xdd)}, std::byte{static_cast<unsigned char>(0xee)},
          std::byte{static_cast<unsigned char>(0xff)}, std::byte{0x10}},
         0x8877665544332211ULL, 0x10ffeeddccbbaa99ULL},
        {0x0c407c20U,
         {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}, std::byte{6},
          std::byte{7}, std::byte{8}},
         0x0807060504030201ULL, 0xa5a5a5a5a5a5a5a5ULL},
        {0x4c407c20U,
         {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}, std::byte{6},
          std::byte{7}, std::byte{8}, std::byte{9}, std::byte{10}, std::byte{11}, std::byte{12},
          std::byte{13}, std::byte{14}, std::byte{15}, std::byte{16}},
         0x0807060504030201ULL, 0x100f0e0d0c0b0a09ULL},
    };
    for (const auto& test : loads)
    {
        auto program = lift_program_with_data({test.instruction, ret}, test.data);
        REQUIRE(program);
        runtime::CpuState cpu;
        cpu.x[1] = data_address;
        cpu.vreg[0] = runtime::Vector128{0xaaaaaaaaaaaaaaaaULL, 0xa5a5a5a5a5a5a5a5ULL};
        REQUIRE(execute_program(program.value(), cpu));
        REQUIRE(cpu.vreg[0].lo == test.expected_low);
        REQUIRE(cpu.vreg[0].hi == test.expected_high);
    }

    struct ReplicateCase
    {
        std::uint32_t instruction;
        std::uint8_t element_bytes;
        std::uint64_t expected_low;
        std::uint64_t expected_high;
    };
    const std::vector<ReplicateCase> replicates{
        {0x0d40c020U, 1U, 0x5a5a5a5a5a5a5a5aULL, 0xa5a5a5a5a5a5a5a5ULL},
        {0x4d40c020U, 1U, 0x5a5a5a5a5a5a5a5aULL, 0x5a5a5a5a5a5a5a5aULL},
        {0x0d40c420U, 2U, 0x5a5a5a5a5a5a5a5aULL, 0xa5a5a5a5a5a5a5a5ULL},
        {0x4d40c420U, 2U, 0x5a5a5a5a5a5a5a5aULL, 0x5a5a5a5a5a5a5a5aULL},
        {0x0d40c820U, 4U, 0x5a5a5a5a5a5a5a5aULL, 0xa5a5a5a5a5a5a5a5ULL},
        {0x4d40c820U, 4U, 0x5a5a5a5a5a5a5a5aULL, 0x5a5a5a5a5a5a5a5aULL},
        {0x0d40cc20U, 8U, 0x5a5a5a5a5a5a5a5aULL, 0xa5a5a5a5a5a5a5a5ULL},
        {0x4d40cc20U, 8U, 0x5a5a5a5a5a5a5a5aULL, 0x5a5a5a5a5a5a5a5aULL},
    };
    for (const auto& test : replicates)
    {
        const std::vector<std::byte> data(test.element_bytes, std::byte{0x5a});
        auto program = lift_program_with_data({test.instruction, ret}, data);
        REQUIRE(program);
        runtime::CpuState cpu;
        cpu.x[1] = data_address;
        cpu.vreg[0] = runtime::Vector128{0xaaaaaaaaaaaaaaaaULL, 0xa5a5a5a5a5a5a5a5ULL};
        REQUIRE(execute_program(program.value(), cpu));
        REQUIRE(cpu.vreg[0].lo == test.expected_low);
        REQUIRE(cpu.vreg[0].hi == test.expected_high);
    }
}

TEST_CASE("M41 LD2 and LD4 deinterleave, wrap register lists, and write back")
{
    const std::vector<std::byte> interleaved{
        std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13},
        std::byte{0x20}, std::byte{0x21}, std::byte{0x22}, std::byte{0x23},
        std::byte{0x30}, std::byte{0x31}, std::byte{0x32}, std::byte{0x33},
        std::byte{0x40}, std::byte{0x41}, std::byte{0x42}, std::byte{0x43},
        std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17},
        std::byte{0x24}, std::byte{0x25}, std::byte{0x26}, std::byte{0x27},
        std::byte{0x34}, std::byte{0x35}, std::byte{0x36}, std::byte{0x37},
        std::byte{0x44}, std::byte{0x45}, std::byte{0x46}, std::byte{0x47},
    };
    auto ld2 = lift_program_with_data({0x0c408820U, ret},
                                      std::span<const std::byte>(interleaved).first(16U));
    REQUIRE(ld2);
    runtime::CpuState ld2_cpu;
    ld2_cpu.x[1] = data_address;
    REQUIRE(execute_program(ld2.value(), ld2_cpu));
    REQUIRE(ld2_cpu.vreg[0] == (runtime::Vector128{0x3332313013121110ULL, 0x0000000000000000ULL}));
    REQUIRE(ld2_cpu.vreg[1] == (runtime::Vector128{0x4342414023222120ULL, 0x0000000000000000ULL}));

    auto ld4 = lift_program_with_data({0x0c400820U, ret}, interleaved);
    REQUIRE(ld4);
    runtime::CpuState ld4_cpu;
    ld4_cpu.x[1] = data_address;
    REQUIRE(execute_program(ld4.value(), ld4_cpu));
    REQUIRE(ld4_cpu.vreg[0] == (runtime::Vector128{0x1716151413121110ULL, 0x0000000000000000ULL}));
    REQUIRE(ld4_cpu.vreg[1] == (runtime::Vector128{0x2726252423222120ULL, 0x0000000000000000ULL}));
    REQUIRE(ld4_cpu.vreg[2] == (runtime::Vector128{0x3736353433323130ULL, 0x0000000000000000ULL}));
    REQUIRE(ld4_cpu.vreg[3] == (runtime::Vector128{0x4746454443424140ULL, 0x0000000000000000ULL}));

    auto wrapped = lift_program_with_data({0x0c40883fU, ret},
                                          std::span<const std::byte>(interleaved).first(16U));
    REQUIRE(wrapped);
    runtime::CpuState wrapped_cpu;
    wrapped_cpu.x[1] = data_address;
    REQUIRE(execute_program(wrapped.value(), wrapped_cpu));
    REQUIRE(wrapped_cpu.vreg[31] == ld2_cpu.vreg[0]);
    REQUIRE(wrapped_cpu.vreg[0] == ld2_cpu.vreg[1]);

    auto immediate_post = lift_program_with_data({0x4cdf7820U, ret},
                                                  std::span<const std::byte>(interleaved).first(16U));
    REQUIRE(immediate_post);
    runtime::CpuState immediate_cpu;
    immediate_cpu.x[1] = data_address;
    REQUIRE(execute_program(immediate_post.value(), immediate_cpu));
    REQUIRE(immediate_cpu.x[1] == data_address + 16U);

    const std::vector<std::byte> replicate_data(4U, std::byte{0x5a});
    auto replicate_post = lift_program_with_data({0x4ddfc820U, ret}, replicate_data);
    REQUIRE(replicate_post);
    runtime::CpuState replicate_cpu;
    replicate_cpu.x[1] = data_address;
    REQUIRE(execute_program(replicate_post.value(), replicate_cpu));
    REQUIRE(replicate_cpu.x[1] == data_address + 4U);

    auto register_post = lift_program_with_data({0x4cc28820U, ret}, interleaved);
    REQUIRE(register_post);
    runtime::CpuState register_cpu;
    register_cpu.x[1] = data_address;
    register_cpu.x[2] = 32U;
    REQUIRE(execute_program(register_post.value(), register_cpu));
    REQUIRE(register_cpu.x[1] == data_address + 32U);

    const std::vector<std::byte> replicated_doubles(32U, std::byte{0x5a});
    const std::vector<std::uint32_t> replicated_double_forms{
        0x0d60cc20U, 0x0d40ec20U, 0x0d60ec20U};
    for (const auto instruction : replicated_double_forms)
    {
        auto program = lift_program_with_data({instruction, ret}, replicated_doubles);
        REQUIRE(program);
        runtime::CpuState cpu;
        cpu.x[1] = data_address;
        REQUIRE(execute_program(program.value(), cpu));
    }
}
