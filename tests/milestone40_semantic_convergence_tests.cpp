#include "switchrecomp/aarch64/decoder.hpp"
#include "switchrecomp/analysis/coverage.hpp"
#include "switchrecomp/analysis/cfg_analyzer.hpp"
#include "switchrecomp/interpreter/interpreter.hpp"
#include "switchrecomp/ir/verifier.hpp"
#include "switchrecomp/lifter/lifter.hpp"
#include "switchrecomp/runtime/context.hpp"
#include "switchrecomp/runtime/cpu_state.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

namespace
{

using namespace switchrecomp;
constexpr memory::GuestAddress code_address = 0x8000U;
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
                                   "m40.text", memory::GuestRegionKind::Text);
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

TEST_CASE("M40 long multiply-add forms preserve signedness and X accumulation")
{
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);
    const auto umaddl = decoder.value()->decode(code_address, 0x9ba20c20U);
    const auto umsubl = decoder.value()->decode(code_address, 0x9ba69ca4U);
    const auto smaddl = decoder.value()->decode(code_address, 0x9b2a2d28U);
    const auto smsubl = decoder.value()->decode(code_address, 0x9b2ebdacU);
    REQUIRE(umaddl);
    REQUIRE(umsubl);
    REQUIRE(smaddl);
    REQUIRE(smsubl);
    REQUIRE(umaddl.value().id == aarch64::InstructionId::Umaddl);
    REQUIRE(umsubl.value().id == aarch64::InstructionId::Umsubl);
    REQUIRE(smaddl.value().id == aarch64::InstructionId::Smaddl);
    REQUIRE(smsubl.value().id == aarch64::InstructionId::Smsubl);
    REQUIRE(lifter::is_instruction_liftable(umaddl.value()));
    REQUIRE(lifter::is_instruction_liftable(umsubl.value()));
    REQUIRE(lifter::is_instruction_liftable(smaddl.value()));
    REQUIRE(lifter::is_instruction_liftable(smsubl.value()));

    auto program = lift_program({0x9ba20c20U, ret});
    REQUIRE(program);
    runtime::CpuState cpu;
    cpu.x[1] = 0xffffffffU;
    cpu.x[2] = 2U;
    cpu.x[3] = 1U;
    REQUIRE(execute_program(program.value(), cpu));
    REQUIRE(cpu.x[0] == 0x1ffffffffULL);

    auto signed_program = lift_program({0x9b2a2d28U, ret});
    REQUIRE(signed_program);
    runtime::CpuState signed_cpu;
    signed_cpu.x[9] = 0xfffffffeU;
    signed_cpu.x[10] = 3U;
    signed_cpu.x[11] = 20U;
    REQUIRE(execute_program(signed_program.value(), signed_cpu));
    REQUIRE(signed_cpu.x[8] == 14U);

    auto unsigned_sub = lift_program({0x9ba69ca4U, ret});
    REQUIRE(unsigned_sub);
    runtime::CpuState unsigned_sub_cpu;
    unsigned_sub_cpu.x[5] = 3U;
    unsigned_sub_cpu.x[6] = 5U;
    unsigned_sub_cpu.x[7] = 10U;
    REQUIRE(execute_program(unsigned_sub.value(), unsigned_sub_cpu));
    REQUIRE(unsigned_sub_cpu.x[4] == 0xffffffffffffffffULL - 4U);

    auto signed_sub = lift_program({0x9b2ebdacU, ret});
    REQUIRE(signed_sub);
    runtime::CpuState signed_sub_cpu;
    signed_sub_cpu.x[13] = 0xfffffffeU;
    signed_sub_cpu.x[14] = 3U;
    signed_sub_cpu.x[15] = 20U;
    REQUIRE(execute_program(signed_sub.value(), signed_sub_cpu));
    REQUIRE(signed_sub_cpu.x[12] == 26U);
}

TEST_CASE("M40 CRC32 forms use reflected CRC32 and CRC32C polynomials")
{
    auto crc_b = lift_program({0x1ad24230U, ret});
    auto crc_h = lift_program({0x1ad54693U, ret});
    auto crc_w = lift_program({0x1ad84af6U, ret});
    auto crc_x = lift_program({0x9adb4f59U, ret});
    auto crcc_b = lift_program({0x1ade53bcU, ret});
    auto crcc_h = lift_program({0x1ac25420U, ret});
    auto crcc_w = lift_program({0x1ac55883U, ret});
    auto crcc_x = lift_program({0x9ac85ce6U, ret});
    REQUIRE(crc_b);
    REQUIRE(crc_h);
    REQUIRE(crc_w);
    REQUIRE(crc_x);
    REQUIRE(crcc_b);
    REQUIRE(crcc_h);
    REQUIRE(crcc_w);
    REQUIRE(crcc_x);

    runtime::CpuState b;
    b.x[17] = 0x12345678U;
    b.x[18] = 0xabU;
    REQUIRE(execute_program(crc_b.value(), b));
    REQUIRE(b.x[16] == 0x1fc8b738U);

    runtime::CpuState h;
    h.x[20] = 0U;
    h.x[21] = 0x1234U;
    REQUIRE(execute_program(crc_h.value(), h));
    REQUIRE(h.x[19] == 0x489382bfU);

    runtime::CpuState w;
    w.x[23] = 0xffffffffU;
    w.x[24] = 0x04030201U;
    REQUIRE(execute_program(crc_w.value(), w));
    REQUIRE(w.x[22] == 0x49c30432U);

    runtime::CpuState x;
    x.x[26] = 0x89abcdefU;
    x.x[27] = 0x0123456789abcdefULL;
    REQUIRE(execute_program(crc_x.value(), x));
    REQUIRE(x.x[25] == 0x190ec766U);

    runtime::CpuState cc_b;
    cc_b.x[29] = 0U;
    cc_b.x[30] = 0xabU;
    REQUIRE(execute_program(crcc_b.value(), cc_b));
    REQUIRE(cc_b.x[28] == 0x3bc21e9dU);

    runtime::CpuState cc_h;
    cc_h.x[1] = 0U;
    cc_h.x[2] = 0x1234U;
    REQUIRE(execute_program(crcc_h.value(), cc_h));
    REQUIRE(cc_h.x[0] == 0xffa1c4c7U);

    runtime::CpuState cc_w;
    cc_w.x[4] = 0U;
    cc_w.x[5] = 0x04030201U;
    REQUIRE(execute_program(crcc_w.value(), cc_w));
    REQUIRE(cc_w.x[3] == 0x6157c733U);

    runtime::CpuState cc_x;
    cc_x.x[7] = 0U;
    cc_x.x[8] = 0x0123456789abcdefULL;
    REQUIRE(execute_program(crcc_x.value(), cc_x));
    REQUIRE(cc_x.x[6] == 0xe9986aa9U);
}

TEST_CASE("M40 PRFM is an architectural hint and REV forms reverse only selected bytes")
{
    auto prfm = lift_program({0xf9802120U, ret});
    auto rev_w = lift_program({0x5ac0096aU, ret});
    auto rev_x = lift_program({0xdac00dacU, ret});
    auto rev16_w = lift_program({0x5ac005eeU, ret});
    auto rev16_x = lift_program({0xdac00630U, ret});
    REQUIRE(prfm);
    REQUIRE(rev_w);
    REQUIRE(rev_x);
    REQUIRE(rev16_w);
    REQUIRE(rev16_x);

    runtime::CpuState hint_cpu;
    hint_cpu.x[9] = 0x1234U;
    REQUIRE(execute_program(prfm.value(), hint_cpu));
    REQUIRE(hint_cpu.x[9] == 0x1234U);

    runtime::CpuState rev_cpu;
    rev_cpu.x[11] = 0x11223344U;
    REQUIRE(execute_program(rev_w.value(), rev_cpu));
    REQUIRE(rev_cpu.x[10] == 0x44332211U);
    rev_cpu.x[13] = 0x0102030405060708ULL;
    REQUIRE(execute_program(rev_x.value(), rev_cpu));
    REQUIRE(rev_cpu.x[12] == 0x0807060504030201ULL);
    rev_cpu.x[15] = 0x11223344U;
    REQUIRE(execute_program(rev16_w.value(), rev_cpu));
    REQUIRE(rev_cpu.x[14] == 0x22114433U);
    rev_cpu.x[17] = 0x0102030405060708ULL;
    REQUIRE(execute_program(rev16_x.value(), rev_cpu));
    REQUIRE(rev_cpu.x[16] == 0x0201040306050807ULL);
}

TEST_CASE("M40 coverage recognizes every selected normalized family")
{
    const auto bytes = code({0x9ba20c20U, 0x9ba69ca4U, 0x9b2a2d28U, 0x9b2ebdacU,
                             0x1ad24230U, 0x1ade53bcU, 0xf9802120U, 0x5ac0096aU,
                             0x3a140272U, ret});
    memory::GuestMemory memory;
    REQUIRE(memory.map(code_address, std::span<const std::byte>(bytes),
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "m40.coverage", memory::GuestRegionKind::Text));
    const auto size = static_cast<memory::GuestSize>(bytes.size());
    const auto serial = analysis::scan_coverage(memory, code_address, size, "m40");
    const auto parallel = analysis::scan_coverage(memory, code_address, size, "m40",
                                                  analysis::CoverageOptions{4'000'000U, 4U});
    REQUIRE(serial);
    REQUIRE(parallel);
    REQUIRE(serial.value().decoded == 10U);
    REQUIRE(serial.value().liftable == 10U);
    REQUIRE(serial.value().unsupported == 0U);
    REQUIRE(serial.value().decode_failures == 0U);
    REQUIRE(analysis::render_coverage_json(serial.value()) ==
            analysis::render_coverage_json(parallel.value()));
}

TEST_CASE("M40 ADCS, SBCS, and NGCS update NZCV from architectural carry-in")
{
    auto adcs = lift_program({0x3a140272U, ret});
    auto sbcs = lift_program({0x7a190317U, ret});
    auto ngcs = lift_program({0xfa1603f5U, ret});
    REQUIRE(adcs);
    REQUIRE(sbcs);
    REQUIRE(ngcs);

    runtime::CpuState add_cpu;
    add_cpu.x[19] = 0xffffffffU;
    add_cpu.x[20] = 0U;
    add_cpu.c = 1U;
    REQUIRE(execute_program(adcs.value(), add_cpu));
    REQUIRE(add_cpu.x[18] == 0U);
    REQUIRE(add_cpu.z == 1U);
    REQUIRE(add_cpu.c == 1U);
    REQUIRE(add_cpu.v == 0U);

    runtime::CpuState sub_cpu;
    sub_cpu.x[24] = 0U;
    sub_cpu.x[25] = 1U;
    sub_cpu.c = 1U;
    REQUIRE(execute_program(sbcs.value(), sub_cpu));
    REQUIRE(sub_cpu.x[23] == 0xffffffffU);
    REQUIRE(sub_cpu.c == 0U);

    runtime::CpuState neg_cpu;
    neg_cpu.x[22] = 1U;
    neg_cpu.c = 1U;
    REQUIRE(execute_program(ngcs.value(), neg_cpu));
    REQUIRE(neg_cpu.x[21] == 0xffffffffffffffffULL);
}
