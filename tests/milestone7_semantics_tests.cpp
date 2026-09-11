#include "switchrecomp/analysis/coverage.hpp"
#include "switchrecomp/analysis/cfg_analyzer.hpp"
#include "switchrecomp/interpreter/interpreter.hpp"
#include "switchrecomp/lifter/lifter.hpp"
#include "switchrecomp/runtime/cpu_state.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
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
    memory::GuestAddress code = 0x100000U;
    std::vector<std::byte> bytes;
    ir::Function function;
};

[[nodiscard]] Result<Fixture> make_fixture(std::initializer_list<std::uint32_t> values,
                                           memory::GuestAddress address = 0x100000U)
{
    Fixture fixture;
    fixture.code = address;
    fixture.bytes = words(values);
    const auto mapped = fixture.memory.map(
        address, std::span<const std::byte>(fixture.bytes),
        memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Execute,
        "fixture.text", memory::GuestRegionKind::Text);
    if (!mapped)
    {
        return Result<Fixture>::failure(mapped.error());
    }
    analysis::AnalysisOptions options;
    options.allowed_code_range = analysis::GuestAddressRange{
        address, static_cast<memory::GuestSize>(fixture.bytes.size())};
    const auto cfg = analysis::analyze_control_flow(fixture.memory, address, options);
    if (!cfg)
    {
        return Result<Fixture>::failure(cfg.error());
    }
    const auto function = lifter::lift_function(cfg.value());
    if (!function)
    {
        return Result<Fixture>::failure(function.error());
    }
    fixture.function = function.value();
    return Result<Fixture>::success(std::move(fixture));
}

[[nodiscard]] Result<runtime::ExecutionResult> run(Fixture& fixture, runtime::CpuState& cpu)
{
    runtime::RuntimeContext runtime{&fixture.memory};
    return interpreter::execute(fixture.function, cpu, runtime);
}

} // namespace

TEST_CASE("condition evaluation covers all NZCV combinations")
{
    using runtime::evaluate_condition;
    using ir::ConditionCode;

    for (unsigned int bits = 0U; bits < 16U; ++bits)
    {
        const bool n = (bits & 0x8U) != 0U;
        const bool z = (bits & 0x4U) != 0U;
        const bool c = (bits & 0x2U) != 0U;
        const bool v = (bits & 0x1U) != 0U;
        REQUIRE(evaluate_condition(ConditionCode::Eq, n, z, c, v) == z);
        REQUIRE(evaluate_condition(ConditionCode::Ne, n, z, c, v) == !z);
        REQUIRE(evaluate_condition(ConditionCode::Cs, n, z, c, v) == c);
        REQUIRE(evaluate_condition(ConditionCode::Cc, n, z, c, v) == !c);
        REQUIRE(evaluate_condition(ConditionCode::Mi, n, z, c, v) == n);
        REQUIRE(evaluate_condition(ConditionCode::Pl, n, z, c, v) == !n);
        REQUIRE(evaluate_condition(ConditionCode::Vs, n, z, c, v) == v);
        REQUIRE(evaluate_condition(ConditionCode::Vc, n, z, c, v) == !v);
        REQUIRE(evaluate_condition(ConditionCode::Hi, n, z, c, v) == (c && !z));
        REQUIRE(evaluate_condition(ConditionCode::Ls, n, z, c, v) == (!c || z));
        REQUIRE(evaluate_condition(ConditionCode::Ge, n, z, c, v) == (n == v));
        REQUIRE(evaluate_condition(ConditionCode::Lt, n, z, c, v) == (n != v));
        REQUIRE(evaluate_condition(ConditionCode::Gt, n, z, c, v) == (!z && n == v));
        REQUIRE(evaluate_condition(ConditionCode::Le, n, z, c, v) == (z || n != v));
        REQUIRE(evaluate_condition(ConditionCode::Al, n, z, c, v));
        REQUIRE_FALSE(evaluate_condition(ConditionCode::Nv, n, z, c, v));
    }
}

TEST_CASE("expanded integer operations preserve width and flags")
{
    auto adds = make_fixture({0xab010000U, 0xd65f03c0U}); // adds x0, x0, x1; ret
    REQUIRE(adds);
    runtime::CpuState adds_cpu;
    adds_cpu.x[0] = std::numeric_limits<std::uint64_t>::max();
    adds_cpu.x[1] = 1U;
    REQUIRE(run(adds.value(), adds_cpu));
    REQUIRE(adds_cpu.x[0] == 0U);
    REQUIRE(adds_cpu.n == 0U);
    REQUIRE(adds_cpu.z == 1U);
    REQUIRE(adds_cpu.c == 1U);
    REQUIRE(adds_cpu.v == 0U);

    auto subs = make_fixture({0xeb010000U, 0xd65f03c0U}); // subs x0, x0, x1; ret
    REQUIRE(subs);
    runtime::CpuState subs_cpu;
    subs_cpu.x[0] = 0U;
    subs_cpu.x[1] = 1U;
    REQUIRE(run(subs.value(), subs_cpu));
    REQUIRE(subs_cpu.x[0] == std::numeric_limits<std::uint64_t>::max());
    REQUIRE(subs_cpu.n == 1U);
    REQUIRE(subs_cpu.z == 0U);
    REQUIRE(subs_cpu.c == 0U);
    REQUIRE(subs_cpu.v == 0U);

    auto logical = make_fixture({0xea020020U, 0xd65f03c0U}); // ands x0, x1, x2; ret
    REQUIRE(logical);
    runtime::CpuState logical_cpu;
    logical_cpu.x[1] = 0x8000000000000000U;
    logical_cpu.x[2] = std::numeric_limits<std::uint64_t>::max();
    logical_cpu.c = 1U;
    logical_cpu.v = 1U;
    REQUIRE(run(logical.value(), logical_cpu));
    REQUIRE(logical_cpu.x[0] == 0x8000000000000000U);
    REQUIRE(logical_cpu.n == 1U);
    REQUIRE(logical_cpu.z == 0U);
    REQUIRE(logical_cpu.c == 0U);
    REQUIRE(logical_cpu.v == 0U);

    auto cmn = make_fixture({0xab11021fU, 0xd65f03c0U}); // cmn x16, x17; ret
    REQUIRE(cmn);
    runtime::CpuState cmn_cpu;
    cmn_cpu.x[16] = std::numeric_limits<std::uint64_t>::max();
    cmn_cpu.x[17] = 1U;
    REQUIRE(run(cmn.value(), cmn_cpu));
    REQUIRE(cmn_cpu.n == 0U);
    REQUIRE(cmn_cpu.z == 1U);
    REQUIRE(cmn_cpu.c == 1U);
    REQUIRE(cmn_cpu.v == 0U);

    auto negs = make_fixture({0xeb0f03eeU, 0xd65f03c0U}); // negs x14, x15; ret
    REQUIRE(negs);
    runtime::CpuState negs_cpu;
    negs_cpu.x[15] = 1U;
    REQUIRE(run(negs.value(), negs_cpu));
    REQUIRE(negs_cpu.x[14] == std::numeric_limits<std::uint64_t>::max());
    REQUIRE(negs_cpu.n == 1U);
    REQUIRE(negs_cpu.z == 0U);
    REQUIRE(negs_cpu.c == 0U);
    REQUIRE(negs_cpu.v == 0U);

    auto wide = make_fixture({0x92a24680U, 0xf2b579a0U, 0xd65f03c0U}); // movn; movk; ret
    REQUIRE(wide);
    runtime::CpuState wide_cpu;
    REQUIRE(run(wide.value(), wide_cpu));
    REQUIRE(wide_cpu.x[0] == 0xffffffffabcdffffU);

    auto multiply = make_fixture({0x9b137e51U, 0x9b165eb4U, 0x9b1aef38U,
                                  0x9b02fc20U, 0xd65f03c0U}); // mul; madd; msub; mneg; ret
    REQUIRE(multiply);
    runtime::CpuState multiply_cpu;
    multiply_cpu.x[18] = 3U;
    multiply_cpu.x[19] = 4U;
    multiply_cpu.x[21] = 2U;
    multiply_cpu.x[22] = 5U;
    multiply_cpu.x[23] = 7U;
    multiply_cpu.x[25] = 2U;
    multiply_cpu.x[26] = 5U;
    multiply_cpu.x[27] = 20U;
    multiply_cpu.x[1] = 7U;
    multiply_cpu.x[2] = 6U;
    REQUIRE(run(multiply.value(), multiply_cpu));
    REQUIRE(multiply_cpu.x[17] == 12U);
    REQUIRE(multiply_cpu.x[20] == 17U);
    REQUIRE(multiply_cpu.x[24] == 10U);
    REQUIRE(multiply_cpu.x[0] == std::uint64_t{0} - 42U);
}

TEST_CASE("conditional selects and indirect returns use architectural state")
{
    auto cset = make_fixture({0x9a9f17e0U, 0xd65f03c0U}); // cset x0, eq; ret
    REQUIRE(cset);
    runtime::CpuState equal;
    equal.z = 1U;
    REQUIRE(run(cset.value(), equal));
    REQUIRE(equal.x[0] == 1U);
    runtime::CpuState different;
    REQUIRE(run(cset.value(), different));
    REQUIRE(different.x[0] == 0U);

    auto csel = make_fixture({0x9a820020U, 0xd65f03c0U}); // csel x0, x1, x2, eq; ret
    REQUIRE(csel);
    runtime::CpuState selected;
    selected.x[1] = 11U;
    selected.x[2] = 22U;
    selected.z = 1U;
    selected.x[30] = 0x12345000U;
    REQUIRE(run(csel.value(), selected));
    REQUIRE(selected.x[0] == 11U);
    REQUIRE(selected.pc == 0x12345000U);

    auto aliases = make_fixture({0x5a9f03e1U, 0x9a835462U, 0xda8540a4U,
                                 0xda87b4e6U, 0xd65f03c0U}); // csetm; cinc; cinv; cneg; ret
    REQUIRE(aliases);
    runtime::CpuState alias_cpu;
    alias_cpu.x[3] = 9U;
    alias_cpu.x[5] = 0x1234U;
    alias_cpu.x[7] = 5U;
    alias_cpu.n = 0U;
    alias_cpu.v = 0U;
    alias_cpu.z = 0U;
    REQUIRE(run(aliases.value(), alias_cpu));
    REQUIRE(alias_cpu.x[1] == 0xffffffffU);
    REQUIRE(alias_cpu.x[2] == 9U);
    REQUIRE(alias_cpu.x[4] == ~std::uint64_t{0x1234U});
    REQUIRE(alias_cpu.x[6] == std::uint64_t{0} - 5U);

    auto branch = make_fixture({0xd61f0120U}); // br x9
    REQUIRE(branch);
    runtime::CpuState branch_cpu;
    branch_cpu.x[9] = 0x71000000U;
    REQUIRE(run(branch.value(), branch_cpu));
    REQUIRE(branch_cpu.pc == 0x71000000U);

    auto call = make_fixture({0xd63f0100U, 0xd65f03c0U}); // blr x8; ret
    REQUIRE(call);
    runtime::CpuState call_cpu;
    call_cpu.x[8] = 0x72000000U;
    REQUIRE(run(call.value(), call_cpu));
    REQUIRE(call_cpu.x[30] == call.value().code + 4U);
    REQUIRE(call_cpu.pc == 0x72000000U);

    auto direct_call = make_fixture({0x94000002U, 0xd65f03c0U, 0xd65f03c0U}); // bl +8; ret; ret
    REQUIRE(direct_call);
    runtime::CpuState direct_call_cpu;
    REQUIRE(run(direct_call.value(), direct_call_cpu));
    REQUIRE(direct_call_cpu.x[30] == direct_call.value().code + 4U);
    REQUIRE(direct_call_cpu.pc == direct_call.value().code + 8U);
}

TEST_CASE("bitfield aliases and rotates preserve the selected width")
{
    auto ubfx = make_fixture({0xd3483c41U, 0xd65f03c0U}); // ubfx x1, x2, #8, #8; ret
    REQUIRE(ubfx);
    runtime::CpuState ubfx_cpu;
    ubfx_cpu.x[2] = 0x123400U;
    REQUIRE(run(ubfx.value(), ubfx_cpu));
    REQUIRE(ubfx_cpu.x[1] == 0x34U);

    auto sxtb = make_fixture({0x93401c83U, 0xd65f03c0U}); // sxtb x3, w4; ret
    REQUIRE(sxtb);
    runtime::CpuState sxtb_cpu;
    sxtb_cpu.x[4] = 0x80U;
    REQUIRE(run(sxtb.value(), sxtb_cpu));
    REQUIRE(sxtb_cpu.x[3] == 0xffffffffffffff80U);

    auto ror = make_fixture({0x93d51eb4U, 0xd65f03c0U}); // ror x20, x21, #7; ret
    REQUIRE(ror);
    runtime::CpuState ror_cpu;
    ror_cpu.x[21] = 1U;
    REQUIRE(run(ror.value(), ror_cpu));
    REQUIRE(ror_cpu.x[20] == (std::uint64_t{1} << 57U));
}

TEST_CASE("scalar memory widths, sign extension and register offsets are explicit")
{
    auto fixture = make_fixture({0xf862d824U, 0xb8627828U, 0x39400020U, 0x79400022U,
                                 0x39800023U, 0x79800024U, 0xb9800025U, 0xd65f03c0U});
    REQUIRE(fixture);
    constexpr memory::GuestAddress data = 0x200000U;
    REQUIRE(fixture.value().memory.map(data, 16U,
                                       memory::GuestMemoryPermissions::Read |
                                           memory::GuestMemoryPermissions::Write,
                                       "fixture.data", memory::GuestRegionKind::Data));
    const std::vector<std::byte> initial{
        std::byte{0x80}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
        std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12},
        std::byte{0},    std::byte{0},    std::byte{0},    std::byte{0},
        std::byte{0},    std::byte{0},    std::byte{0},    std::byte{0}};
    REQUIRE(fixture.value().memory.write(data, initial));

    runtime::CpuState cpu;
    cpu.x[1] = data;
    cpu.x[2] = 1U;
    REQUIRE(run(fixture.value(), cpu));
    REQUIRE(cpu.x[0] == 0x80U);
    REQUIRE(cpu.x[2] == 0xff80U);
    REQUIRE(cpu.x[3] == 0xffffffffffffff80U);
    REQUIRE(cpu.x[4] == 0xffffffffffffff80U);
    REQUIRE(cpu.x[5] == 0xffffffffffffff80U);
    REQUIRE(cpu.x[8] == 0x12345678U);

    auto store_byte = make_fixture({0x39000020U, 0xd65f03c0U}); // strb w0, [x1]; ret
    REQUIRE(store_byte);
    constexpr memory::GuestAddress byte_data = 0x210000U;
    REQUIRE(store_byte.value().memory.map(byte_data, 4U,
                                          memory::GuestMemoryPermissions::Read |
                                              memory::GuestMemoryPermissions::Write,
                                          "fixture.byte", memory::GuestRegionKind::Data));
    runtime::CpuState byte_cpu;
    byte_cpu.x[0] = 0xabU;
    byte_cpu.x[1] = byte_data;
    REQUIRE(run(store_byte.value(), byte_cpu));
    std::uint64_t byte_value = 0U;
    runtime::RuntimeContext byte_runtime{&store_byte.value().memory};
    REQUIRE(runtime::switchrecomp_runtime_guest_load(&byte_runtime, byte_data, 1U, &byte_value) == 0U);
    REQUIRE(byte_value == 0xabU);

    auto store_half = make_fixture({0x79000022U, 0xd65f03c0U}); // strh w2, [x1]; ret
    REQUIRE(store_half);
    constexpr memory::GuestAddress half_data = 0x220000U;
    REQUIRE(store_half.value().memory.map(half_data, 4U,
                                          memory::GuestMemoryPermissions::Read |
                                              memory::GuestMemoryPermissions::Write,
                                          "fixture.half", memory::GuestRegionKind::Data));
    runtime::CpuState half_cpu;
    half_cpu.x[1] = half_data;
    half_cpu.x[2] = 0xcdefU;
    REQUIRE(run(store_half.value(), half_cpu));
    std::uint64_t half_value = 0U;
    runtime::RuntimeContext half_runtime{&store_half.value().memory};
    REQUIRE(runtime::switchrecomp_runtime_guest_load(&half_runtime, half_data, 2U, &half_value) == 0U);
    REQUIRE(half_value == 0xcdefU);
}

TEST_CASE("pair addressing performs pre and post writeback")
{
    auto fixture = make_fixture({0xa9be7bfdU, 0xa8c27bfdU, 0xd65f03c0U});
    REQUIRE(fixture);
    constexpr memory::GuestAddress stack = 0x300100U;
    REQUIRE(fixture.value().memory.map(stack - 0x40U, 0x80U,
                                       memory::GuestMemoryPermissions::Read |
                                           memory::GuestMemoryPermissions::Write,
                                       "fixture.stack", memory::GuestRegionKind::Data));
    runtime::CpuState cpu;
    cpu.sp = stack;
    cpu.x[29] = 0x1111U;
    cpu.x[30] = 0x2222U;
    REQUIRE(run(fixture.value(), cpu));
    REQUIRE(cpu.x[29] == 0x1111U);
    REQUIRE(cpu.x[30] == 0x2222U);
    REQUIRE(cpu.sp == stack);
}

TEST_CASE("TBZ and TBNZ use the encoded bit and CFG paths")
{
    auto fixture = make_fixture({0x36180060U, 0xd2800000U, 0xd65f03c0U,
                                 0xd2800021U, 0xd65f03c0U}); // tbz x0,#3,+12
    REQUIRE(fixture);
    runtime::CpuState zero;
    REQUIRE(run(fixture.value(), zero));
    REQUIRE(zero.x[1] == 1U);

    runtime::CpuState set;
    set.x[0] = 8U;
    REQUIRE(run(fixture.value(), set));
    REQUIRE(set.x[1] == 0U);

    auto tbnz = make_fixture({0x37180060U, 0xd2800000U, 0xd65f03c0U,
                              0xd2800021U, 0xd65f03c0U}); // tbnz x0,#3,+12
    REQUIRE(tbnz);
    runtime::CpuState tbnz_set;
    tbnz_set.x[0] = 8U;
    REQUIRE(run(tbnz.value(), tbnz_set));
    REQUIRE(tbnz_set.x[1] == 1U);

    runtime::CpuState tbnz_zero;
    REQUIRE(run(tbnz.value(), tbnz_zero));
    REQUIRE(tbnz_zero.x[1] == 0U);
}

TEST_CASE("canonical function prologue, conditional body and epilogue execute")
{
    auto fixture = make_fixture({0xa9be7bfdU, 0x910003fdU, 0xf100001fU, 0x54000060U,
                                 0x91000400U, 0x14000002U, 0xd2800540U, 0xa8c27bfdU,
                                 0xd65f03c0U});
    REQUIRE(fixture);
    constexpr memory::GuestAddress stack = 0x500100U;
    REQUIRE(fixture.value().memory.map(stack - 0x40U, 0x80U,
                                       memory::GuestMemoryPermissions::Read |
                                           memory::GuestMemoryPermissions::Write,
                                       "fixture.stack", memory::GuestRegionKind::Data));

    runtime::CpuState nonzero;
    nonzero.sp = stack;
    nonzero.x[0] = 4U;
    nonzero.x[29] = 0xaaaaU;
    nonzero.x[30] = 0x71000000U;
    REQUIRE(run(fixture.value(), nonzero));
    REQUIRE(nonzero.x[0] == 5U);
    REQUIRE(nonzero.x[29] == 0xaaaaU);
    REQUIRE(nonzero.x[30] == 0x71000000U);
    REQUIRE(nonzero.sp == stack);
    REQUIRE(nonzero.pc == 0x71000000U);

    runtime::CpuState zero;
    zero.sp = stack;
    zero.x[29] = 0xbbbbU;
    zero.x[30] = 0x72000000U;
    REQUIRE(run(fixture.value(), zero));
    REQUIRE(zero.x[0] == 42U);
    REQUIRE(zero.x[29] == 0xbbbbU);
    REQUIRE(zero.x[30] == 0x72000000U);
    REQUIRE(zero.sp == stack);
    REQUIRE(zero.pc == 0x72000000U);
}

TEST_CASE("coverage reports are deterministic and include normalized FP families")
{
    constexpr memory::GuestAddress base = 0x400000U;
    const auto bytes = words({0xd503201fU, 0x8b010000U, 0x1e220820U, 0xd65f03c0U});
    memory::GuestMemory memory;
    REQUIRE(memory.map(base, std::span<const std::byte>(bytes),
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "synthetic.text", memory::GuestRegionKind::Text));
    const auto first = analysis::scan_coverage(memory, base, bytes.size(), "synthetic.bin");
    const auto second = analysis::scan_coverage(memory, base, bytes.size(), "synthetic.bin");
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(first.value().decoded == 4U);
    REQUIRE(first.value().liftable == 4U);
    REQUIRE(first.value().unsupported == 0U);
    REQUIRE(first.value().unsupported_frequency.empty());
    REQUIRE(analysis::render_coverage(first.value()) == analysis::render_coverage(second.value()));
    REQUIRE(analysis::render_coverage_json(first.value()) ==
            analysis::render_coverage_json(second.value()));
    REQUIRE(analysis::render_coverage_json(first.value()).find("\"schema_version\":1") !=
            std::string::npos);
    REQUIRE(analysis::render_coverage_json(first.value()).find("\"instruction_frequency\"") !=
            std::string::npos);
}
