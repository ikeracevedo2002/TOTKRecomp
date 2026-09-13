#include "switchrecomp/aarch64/decoder.hpp"
#include "switchrecomp/analysis/coverage.hpp"
#include "switchrecomp/analysis/cfg_analyzer.hpp"
#include "switchrecomp/interpreter/interpreter.hpp"
#include "switchrecomp/lifter/lifter.hpp"
#include "switchrecomp/runtime/context.hpp"
#include "switchrecomp/runtime/cpu_state.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

namespace
{

using namespace switchrecomp;
constexpr memory::GuestAddress code_address = 0x43000U;
constexpr std::uint32_t ret = 0xd65f03c0U;

[[nodiscard]] std::vector<std::byte> code(std::span<const std::uint32_t> words)
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

[[nodiscard]] std::vector<std::byte> code(std::initializer_list<std::uint32_t> words)
{
    return code(std::span<const std::uint32_t>(words.begin(), words.size()));
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
                                   "m43.advsimd.text", memory::GuestRegionKind::Text);
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

[[nodiscard]] runtime::Vector128 lanes8(std::initializer_list<std::uint8_t> values)
{
    runtime::Vector128 result{};
    std::uint8_t lane = 0U;
    for (const auto value : values) runtime::write_lane_bits(result, 8U, lane++, value);
    return result;
}

} // namespace

TEST_CASE("M43 AdvSIMD decoder and coverage agree on the new collapsed families")
{
    constexpr std::array words{
        0x4e22cc20U, // fmla v0.4s, v1.4s, v2.4s
        0x4ee5cc83U, // fmls v3.2d, v4.2d, v5.2d
        0x4ea0e8e6U, // fcmlt v6.4s, v7.4s, #0.0
        0x6ee0d928U, // fcmle v8.2d, v9.2d, #0.0
        0x2eacc16aU, // umull v10.2d, v11.2s, v12.2s
        0x6e6fc1cdU, // umull2 v13.4s, v14.8h, v15.8h
        0x4e120230U, // tbl v16.16b, {v17.16b}, v18.16b
        0x0e163293U, // tbx v19.8b, {v20.16b, v21.16b}, v22.8b
    };
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);
    const std::array expected{
        aarch64::SimdOperation::Fmla, aarch64::SimdOperation::Fmls,
        aarch64::SimdOperation::Fcmlt, aarch64::SimdOperation::Fcmle,
        aarch64::SimdOperation::Umull, aarch64::SimdOperation::Umull2,
        aarch64::SimdOperation::Tbl, aarch64::SimdOperation::Tbx,
    };
    for (std::size_t index = 0U; index < words.size(); ++index)
    {
        const auto decoded = decoder.value()->decode(code_address + index * 4U, words[index]);
        REQUIRE(decoded);
        INFO("index=" << index << " decoded=" << aarch64::simd_operation_name(decoded.value().simd_operation)
                       << " expected=" << aarch64::simd_operation_name(expected[index]));
        REQUIRE(decoded.value().simd_operation == expected[index]);
        REQUIRE(decoded.value().normalized);
        REQUIRE(lifter::is_instruction_liftable(decoded.value()));
    }

    const auto bytes = code(words);
    memory::GuestMemory memory;
    REQUIRE(memory.map(code_address, std::span<const std::byte>(bytes),
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "m43.coverage", memory::GuestRegionKind::Text));
    const auto report = analysis::scan_coverage(
        memory, code_address, static_cast<memory::GuestSize>(bytes.size()), "m43.coverage");
    REQUIRE(report);
    REQUIRE(report.value().decoded == words.size());
    REQUIRE(report.value().liftable == words.size());
    REQUIRE(report.value().unsupported == 0U);
}

TEST_CASE("M43 scalar and vector widening multiply preserve signedness and upper lanes")
{
    auto scalar = lift_program({0x9ba07c00U, ret}); // umull x0, w0, w0
    REQUIRE(scalar);
    runtime::CpuState scalar_cpu;
    scalar_cpu.x[0] = 0xffffffffU;
    REQUIRE(execute_program(scalar.value(), scalar_cpu));
    REQUIRE(scalar_cpu.x[0] == 0xfffffffe00000001ULL);

    auto vector = lift_program({0x2eacc16aU, ret});
    REQUIRE(vector);
    runtime::CpuState vector_cpu;
    vector_cpu.vreg[11] = runtime::Vector128{0x00000002ffffffffULL, 0U};
    vector_cpu.vreg[12] = runtime::Vector128{0x0000000300000002ULL, 0U};
    REQUIRE(execute_program(vector.value(), vector_cpu));
    REQUIRE(vector_cpu.vreg[10] == (runtime::Vector128{0x00000001fffffffeULL,
                                                        0x0000000000000006ULL}));

    auto upper = lift_program({0x6e6fc1cdU, ret});
    REQUIRE(upper);
    runtime::CpuState upper_cpu;
    upper_cpu.vreg[14] = runtime::Vector128{0x0000000400000003ULL,
                                             0x0000000600000005ULL};
    upper_cpu.vreg[15] = runtime::Vector128{0x0000000800000007ULL,
                                             0x0000000a00000009ULL};
    REQUIRE(execute_program(upper.value(), upper_cpu));
    REQUIRE(upper_cpu.vreg[13] == (runtime::Vector128{0x000000000000002dULL,
                                                       0x000000000000003cULL}));

    auto signed_subtract = lift_program({0x4eb1a20fU, ret}); // smlsl2 v15.2d, v16.4s, v17.4s
    REQUIRE(signed_subtract);
    runtime::CpuState signed_cpu;
    signed_cpu.vreg[15] = runtime::Vector128{100U, 200U};
    signed_cpu.vreg[16] = runtime::Vector128{
        0U, (std::uint64_t{0xfffffffdU} << 32U) | 0xfffffffeU};
    signed_cpu.vreg[17] = runtime::Vector128{
        0U, (std::uint64_t{0xfffffffbU} << 32U) | 0xfffffffcu};
    REQUIRE(execute_program(signed_subtract.value(), signed_cpu));
    REQUIRE(runtime::read_lane_bits(signed_cpu.vreg[15], 64U, 0U) == 92U);
    REQUIRE(runtime::read_lane_bits(signed_cpu.vreg[15], 64U, 1U) == 185U);
}

TEST_CASE("M43 fused FP and zero compares use the reference runtime")
{
    auto fused = lift_program({0x4e22cc20U, ret});
    REQUIRE(fused);
    runtime::CpuState fused_cpu;
    fused_cpu.vreg[1] = runtime::Vector128{
        (std::uint64_t{0x40000000U} << 32U) | 0x3f800000U,
        (std::uint64_t{0x40800000U} << 32U) | 0x40400000U};
    fused_cpu.vreg[2] = runtime::Vector128{
        (std::uint64_t{0x41a00000U} << 32U) | 0x41200000U,
        (std::uint64_t{0x42200000U} << 32U) | 0x41f00000U};
    fused_cpu.vreg[0] = runtime::Vector128{0U, 0U};
    REQUIRE(execute_program(fused.value(), fused_cpu));
    REQUIRE(std::bit_cast<float>(static_cast<std::uint32_t>(runtime::read_lane_bits(
                fused_cpu.vreg[0], 32U, 0U))) == 10.0F);
    REQUIRE(std::bit_cast<float>(static_cast<std::uint32_t>(runtime::read_lane_bits(
                fused_cpu.vreg[0], 32U, 1U))) == 40.0F);
    REQUIRE(std::bit_cast<float>(static_cast<std::uint32_t>(runtime::read_lane_bits(
                fused_cpu.vreg[0], 32U, 2U))) == 90.0F);
    REQUIRE(std::bit_cast<float>(static_cast<std::uint32_t>(runtime::read_lane_bits(
                fused_cpu.vreg[0], 32U, 3U))) == 160.0F);

    auto subtract = lift_program({0x4fc55883U, ret}); // fmls v3.2d, v4.2d, v5.d[1]
    REQUIRE(subtract);
    runtime::CpuState subtract_cpu;
    subtract_cpu.vreg[3] = runtime::Vector128{std::bit_cast<std::uint64_t>(100.0),
                                               std::bit_cast<std::uint64_t>(200.0)};
    subtract_cpu.vreg[4] = runtime::Vector128{std::bit_cast<std::uint64_t>(3.0),
                                               std::bit_cast<std::uint64_t>(4.0)};
    subtract_cpu.vreg[5] = runtime::Vector128{std::bit_cast<std::uint64_t>(2.0),
                                               std::bit_cast<std::uint64_t>(5.0)};
    REQUIRE(execute_program(subtract.value(), subtract_cpu));
    REQUIRE(std::bit_cast<double>(runtime::read_lane_bits(subtract_cpu.vreg[3], 64U, 0U)) == 85.0);
    REQUIRE(std::bit_cast<double>(runtime::read_lane_bits(subtract_cpu.vreg[3], 64U, 1U)) == 180.0);

    auto compare = lift_program({0x4ea0e8e6U, ret});
    REQUIRE(compare);
    runtime::CpuState compare_cpu;
    compare_cpu.vreg[7] = runtime::Vector128{
        (std::uint64_t{0xbf800000} << 32U) | 0x3f800000U,
        (std::uint64_t{0x40000000U} << 32U) | 0xc0000000U};
    REQUIRE(execute_program(compare.value(), compare_cpu));
    REQUIRE(runtime::read_lane_bits(compare_cpu.vreg[6], 32U, 0U) == 0U);
    REQUIRE(runtime::read_lane_bits(compare_cpu.vreg[6], 32U, 1U) == UINT32_MAX);
    REQUIRE(runtime::read_lane_bits(compare_cpu.vreg[6], 32U, 2U) == UINT32_MAX);
    REQUIRE(runtime::read_lane_bits(compare_cpu.vreg[6], 32U, 3U) == 0U);
}

TEST_CASE("M43 TBL and TBX honor table bounds and destination preservation")
{
    auto tbl = lift_program({0x4e120230U, ret});
    REQUIRE(tbl);
    runtime::CpuState tbl_cpu;
    tbl_cpu.vreg[17] = lanes8({0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U});
    tbl_cpu.vreg[18] = lanes8({0U, 15U, 16U, 2U, 9U, 14U, 1U, 8U, 3U, 4U, 5U, 6U, 7U, 10U, 11U, 12U});
    tbl_cpu.vreg[16] = runtime::Vector128{UINT64_MAX, UINT64_MAX};
    REQUIRE(execute_program(tbl.value(), tbl_cpu));
    REQUIRE(runtime::read_lane_bits(tbl_cpu.vreg[16], 8U, 0U) == 0U);
    REQUIRE(runtime::read_lane_bits(tbl_cpu.vreg[16], 8U, 1U) == 15U);
    REQUIRE(runtime::read_lane_bits(tbl_cpu.vreg[16], 8U, 2U) == 0U);
    REQUIRE(runtime::read_lane_bits(tbl_cpu.vreg[16], 8U, 3U) == 2U);

    auto tbx = lift_program({0x0e163293U, ret});
    REQUIRE(tbx);
    runtime::CpuState tbx_cpu;
    tbx_cpu.vreg[20] = lanes8({0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U});
    tbx_cpu.vreg[21] = lanes8({0xa0U, 0xa1U, 0xa2U, 0xa3U, 0xa4U, 0xa5U, 0xa6U, 0xa7U,
                               0xa8U, 0xa9U, 0xaaU, 0xabU, 0xacU, 0xadU, 0xaeU, 0xafU});
    tbx_cpu.vreg[22] = lanes8({0U, 16U, 31U, 32U, 1U, 17U, 33U, 2U});
    tbx_cpu.vreg[19] = runtime::Vector128{0x8877665544332211ULL, 0xdeadbeefcafebabeULL};
    REQUIRE(execute_program(tbx.value(), tbx_cpu));
    REQUIRE(runtime::read_lane_bits(tbx_cpu.vreg[19], 8U, 0U) == 0U);
    REQUIRE(runtime::read_lane_bits(tbx_cpu.vreg[19], 8U, 1U) == 0xa0U);
    REQUIRE(runtime::read_lane_bits(tbx_cpu.vreg[19], 8U, 2U) == 0xafU);
    REQUIRE(runtime::read_lane_bits(tbx_cpu.vreg[19], 8U, 3U) == 0x44U);
    REQUIRE(runtime::read_lane_bits(tbx_cpu.vreg[19], 8U, 4U) == 1U);
    REQUIRE(runtime::read_lane_bits(tbx_cpu.vreg[19], 8U, 5U) == 0xa1U);
    REQUIRE(runtime::read_lane_bits(tbx_cpu.vreg[19], 8U, 6U) == 0x77U);
    REQUIRE(runtime::read_lane_bits(tbx_cpu.vreg[19], 8U, 7U) == 2U);
}
