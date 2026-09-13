#include "switchrecomp/aarch64/decoder.hpp"
#include "switchrecomp/analysis/coverage.hpp"
#include "switchrecomp/analysis/cfg_analyzer.hpp"
#include "switchrecomp/interpreter/interpreter.hpp"
#include "switchrecomp/lifter/lifter.hpp"
#include "switchrecomp/runtime/context.hpp"
#include "switchrecomp/runtime/cpu_state.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <span>
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

[[nodiscard]] Result<LiftedProgram> lift_program_with_data(
    std::initializer_list<std::uint32_t> words, std::size_t data_size = 64U)
{
    const auto bytes = code(words);
    memory::GuestMemory memory;
    const auto mapped = memory.map(code_address, std::span<const std::byte>(bytes),
                                   memory::GuestMemoryPermissions::Read |
                                       memory::GuestMemoryPermissions::Execute,
                                   "m42.text", memory::GuestRegionKind::Text);
    if (!mapped) return Result<LiftedProgram>::failure(mapped.error());
    const auto data = memory.map(data_address, data_size,
                                 memory::GuestMemoryPermissions::Read |
                                     memory::GuestMemoryPermissions::Write,
                                 "m42.data", memory::GuestRegionKind::Data);
    if (!data) return Result<LiftedProgram>::failure(data.error());
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

[[nodiscard]] std::vector<std::byte> read_data(const memory::GuestMemory& memory,
                                               memory::GuestAddress address,
                                               std::size_t size)
{
    std::vector<std::byte> bytes(size);
    REQUIRE(memory.read(address, std::span<std::byte>(bytes)));
    return bytes;
}

[[nodiscard]] Result<runtime::ExecutionResult> execute(LiftedProgram& program, runtime::CpuState& cpu)
{
    runtime::RuntimeContext context{&program.memory};
    return interpreter::execute(program.function, cpu, context);
}

} // namespace

TEST_CASE("M42 decoder, lifter, and coverage agree on ST1 through ST4 forms")
{
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);
    constexpr std::uint32_t forms[]{
        0x4c00ac00U, // ST1 {V0.2D, V1.2D}
        0x0c008800U, // ST2 {V0.2S, V1.2S}
        0x0c004800U, // ST3 {V0.2S, V1.2S, V2.2S}
        0x0c000800U, // ST4 {V0.2S, V1.2S, V2.2S, V3.2S}
        0x0c9f8800U, // ST2 immediate post-index
        0x0c818800U, // ST2 register post-index
        0x0d9f9000U, // ST1 lane immediate post-index
        0x0d819000U, // ST1 lane register post-index
        0x0c00881fU, // ST2 register-list wrap V31 -> V0
        0x0c008be0U, // ST2 with SP as the base
    };
    const auto bytes = code({forms[0], forms[1], forms[2], forms[3], forms[4],
                             forms[5], forms[6], forms[7], forms[8], forms[9], ret});
    memory::GuestMemory memory;
    REQUIRE(memory.map(code_address, std::span<const std::byte>(bytes),
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "m42.coverage", memory::GuestRegionKind::Text));
    const auto report = analysis::scan_coverage(
        memory, code_address, static_cast<memory::GuestSize>(bytes.size()), "m42");
    REQUIRE(report);
    REQUIRE(report.value().decoded == 11U);
    REQUIRE(report.value().liftable == 11U);
    REQUIRE(report.value().unsupported == 0U);
    REQUIRE(report.value().decode_failures == 0U);

    for (std::size_t index = 0U; index < std::size(forms); ++index)
    {
        const auto decoded = decoder.value()->decode(code_address, forms[index]);
        REQUIRE(decoded);
        REQUIRE(decoded.value().id == aarch64::InstructionId::FpSimd);
        REQUIRE(decoded.value().normalized);
        REQUIRE(decoded.value().simd_operation ==
                (index == 0U || index == 6U || index == 7U ? aarch64::SimdOperation::St1
                 : index == 1U || index == 4U || index == 5U || index == 8U || index == 9U
                     ? aarch64::SimdOperation::St2
                     : index == 2U ? aarch64::SimdOperation::St3 : aarch64::SimdOperation::St4));
        REQUIRE(lifter::is_instruction_liftable(decoded.value()));
    }
}

TEST_CASE("M42 ST1 stores sequential vectors and every lane element width")
{
    auto program = lift_program_with_data({0x4c00ac00U, ret}, 32U);
    REQUIRE(program);
    runtime::CpuState cpu;
    cpu.x[0] = data_address;
    cpu.vreg[0] = runtime::Vector128{0x100f0e0d0c0b0a09ULL, 0x1817161514131211ULL};
    cpu.vreg[1] = runtime::Vector128{0x201f1e1d1c1b1a19ULL, 0x2827262524232221ULL};
    REQUIRE(execute(program.value(), cpu));
    REQUIRE(read_data(program.value().memory, data_address, 32U) ==
            std::vector<std::byte>{
                std::byte{0x09}, std::byte{0x0a}, std::byte{0x0b}, std::byte{0x0c},
                std::byte{0x0d}, std::byte{0x0e}, std::byte{0x0f}, std::byte{0x10},
                std::byte{0x11}, std::byte{0x12}, std::byte{0x13}, std::byte{0x14},
                std::byte{0x15}, std::byte{0x16}, std::byte{0x17}, std::byte{0x18},
                std::byte{0x19}, std::byte{0x1a}, std::byte{0x1b}, std::byte{0x1c},
                std::byte{0x1d}, std::byte{0x1e}, std::byte{0x1f}, std::byte{0x20},
                std::byte{0x21}, std::byte{0x22}, std::byte{0x23}, std::byte{0x24},
                std::byte{0x25}, std::byte{0x26}, std::byte{0x27}, std::byte{0x28}});
    REQUIRE(cpu.x[0] == data_address);

    struct LaneCase
    {
        std::uint32_t instruction;
        std::size_t size;
        std::size_t offset;
        std::size_t width;
    };
    constexpr LaneCase lanes[]{
        {0x0d000900U, 8U, 2U, 1U},  // B8 lane 2
        {0x0d005121U, 8U, 4U, 2U},  // H4 lane 2
        {0x4d008142U, 16U, 8U, 4U}, // S4 lane 2
        {0x4d008563U, 16U, 8U, 8U}, // D2 lane 1
    };
    for (const auto& test : lanes)
    {
        auto lane_program = lift_program_with_data({test.instruction, ret}, 16U);
        REQUIRE(lane_program);
        runtime::CpuState lane_cpu;
        for (auto& reg : lane_cpu.x) reg = data_address;
        for (auto& vector : lane_cpu.vreg)
            vector = runtime::Vector128{0x8877665544332211ULL,
                                        0x00ffeeddccbbaa99ULL};
        REQUIRE(execute(lane_program.value(), lane_cpu));
        const std::array<std::byte, 16> vector_bytes{
            std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44},
            std::byte{0x55}, std::byte{0x66}, std::byte{0x77}, std::byte{0x88},
            std::byte{0x99}, std::byte{0xaa}, std::byte{0xbb}, std::byte{0xcc},
            std::byte{0xdd}, std::byte{0xee}, std::byte{0xff}, std::byte{0x00}};
        const auto stored = read_data(lane_program.value().memory, data_address, 16U);
        for (std::size_t byte = 0U; byte < test.width; ++byte)
            REQUIRE(stored[byte] == vector_bytes[test.offset + byte]);
    }
}

TEST_CASE("M42 ST2, ST3, and ST4 interleave elements and wrap vector lists")
{
    constexpr runtime::Vector128 v0{0x1716151413121110ULL, 0x2726252423222120ULL};
    constexpr runtime::Vector128 v1{0x3736353433323130ULL, 0x4746454443424140ULL};
    constexpr runtime::Vector128 v2{0x5756555453525150ULL, 0x6766656463626160ULL};
    constexpr runtime::Vector128 v3{0x7776757473727170ULL, 0x8786858483828180ULL};

    struct Case
    {
        std::uint32_t instruction;
        std::size_t source_count;
        std::size_t bytes;
    };
    constexpr Case cases[]{
        {0x0c008800U, 2U, 16U},
        {0x0c004800U, 3U, 24U},
        {0x0c000800U, 4U, 32U},
        {0x4c008800U, 2U, 32U}, // 128-bit 4S arrangement
        {0x0c00881fU, 2U, 16U}, // V31 -> V0
    };
    for (const auto& test : cases)
    {
        auto program = lift_program_with_data({test.instruction, ret}, 64U);
        REQUIRE(program);
        runtime::CpuState cpu;
        cpu.x[0] = data_address;
        cpu.vreg[0] = v0;
        cpu.vreg[1] = v1;
        cpu.vreg[2] = v2;
        cpu.vreg[3] = v3;
        cpu.vreg[31] = v0;
        REQUIRE(execute(program.value(), cpu));
        const auto stored = read_data(program.value().memory, data_address, test.bytes);
        const std::array<std::byte, 32> expected{
            std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13},
            std::byte{0x30}, std::byte{0x31}, std::byte{0x32}, std::byte{0x33},
            std::byte{0x50}, std::byte{0x51}, std::byte{0x52}, std::byte{0x53},
            std::byte{0x70}, std::byte{0x71}, std::byte{0x72}, std::byte{0x73},
            std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17},
            std::byte{0x34}, std::byte{0x35}, std::byte{0x36}, std::byte{0x37},
            std::byte{0x54}, std::byte{0x55}, std::byte{0x56}, std::byte{0x57},
            std::byte{0x74}, std::byte{0x75}, std::byte{0x76}, std::byte{0x77}};
        if (test.instruction == 0x0c008800U)
            REQUIRE(stored == std::vector<std::byte>{
                std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13},
                std::byte{0x30}, std::byte{0x31}, std::byte{0x32}, std::byte{0x33},
                std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17},
                std::byte{0x34}, std::byte{0x35}, std::byte{0x36}, std::byte{0x37}});
        else if (test.instruction == 0x4c008800U)
            REQUIRE(stored == std::vector<std::byte>{
                std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13},
                std::byte{0x30}, std::byte{0x31}, std::byte{0x32}, std::byte{0x33},
                std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17},
                std::byte{0x34}, std::byte{0x35}, std::byte{0x36}, std::byte{0x37},
                std::byte{0x20}, std::byte{0x21}, std::byte{0x22}, std::byte{0x23},
                std::byte{0x40}, std::byte{0x41}, std::byte{0x42}, std::byte{0x43},
                std::byte{0x24}, std::byte{0x25}, std::byte{0x26}, std::byte{0x27},
                std::byte{0x44}, std::byte{0x45}, std::byte{0x46}, std::byte{0x47}});
        else if (test.instruction == 0x0c004800U)
            REQUIRE(stored == std::vector<std::byte>{
                std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13},
                std::byte{0x30}, std::byte{0x31}, std::byte{0x32}, std::byte{0x33},
                std::byte{0x50}, std::byte{0x51}, std::byte{0x52}, std::byte{0x53},
                std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17},
                std::byte{0x34}, std::byte{0x35}, std::byte{0x36}, std::byte{0x37},
                std::byte{0x54}, std::byte{0x55}, std::byte{0x56}, std::byte{0x57}});
        else if (test.instruction == 0x0c000800U)
            REQUIRE(stored == std::vector<std::byte>{expected.begin(), expected.end()});
        else if (test.instruction == 0x0c00881fU)
            REQUIRE(stored == std::vector<std::byte>{
                std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13},
                std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13},
                std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17},
                std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17}});
        else
            REQUIRE(stored == std::vector<std::byte>{
                std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13},
                std::byte{0x30}, std::byte{0x31}, std::byte{0x32}, std::byte{0x33},
                std::byte{0x50}, std::byte{0x51}, std::byte{0x52}, std::byte{0x53},
                std::byte{0x70}, std::byte{0x71}, std::byte{0x72}, std::byte{0x73},
                std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17},
                std::byte{0x34}, std::byte{0x35}, std::byte{0x36}, std::byte{0x37},
                std::byte{0x54}, std::byte{0x55}, std::byte{0x56}, std::byte{0x57},
                std::byte{0x74}, std::byte{0x75}, std::byte{0x76}, std::byte{0x77}});
        (void)test.source_count;
    }
}

TEST_CASE("M42 structure-store writeback uses Xn, register offsets, and SP")
{
    struct Case
    {
        std::uint32_t instruction;
        std::uint64_t initial_base;
        std::uint64_t expected_base;
        bool stack_pointer;
    };
    constexpr Case cases[]{
        {0x0c008800U, data_address, data_address, false},
        {0x0c9f8800U, data_address, data_address + 16U, false},
        {0x0c818800U, data_address, data_address + 24U, false},
        {0x0c9f8be0U, data_address, data_address + 16U, true},
    };
    for (const auto& test : cases)
    {
        auto program = lift_program_with_data({test.instruction, ret}, 32U);
        REQUIRE(program);
        runtime::CpuState cpu;
        cpu.x[0] = test.initial_base;
        cpu.x[1] = 24U;
        cpu.x[2] = 24U;
        cpu.sp = test.initial_base;
        cpu.vreg[0] = runtime::Vector128{0x04030201U, 0x08070605U};
        cpu.vreg[1] = runtime::Vector128{0x14131211U, 0x18171615U};
        REQUIRE(execute(program.value(), cpu));
        REQUIRE((test.stack_pointer ? cpu.sp : cpu.x[0]) == test.expected_base);
        REQUIRE(cpu.vreg[0] == (runtime::Vector128{0x0000000004030201ULL, 0x0000000008070605ULL}));
        REQUIRE(cpu.vreg[1] == (runtime::Vector128{0x0000000014131211ULL, 0x0000000018171615ULL}));
    }
}
