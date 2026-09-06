#include "switchrecomp/analysis/cfg_analyzer.hpp"
#include "switchrecomp/interpreter/interpreter.hpp"
#include "switchrecomp/ir/builder.hpp"
#include "switchrecomp/ir/printer.hpp"
#include "switchrecomp/lifter/lifter.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
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
    memory::GuestAddress code = 0x100000U;
    std::vector<std::byte> bytes;
    analysis::ControlFlowGraph cfg;
    ir::Function function;
};

[[nodiscard]] Result<Fixture> make_fixture(std::initializer_list<std::uint32_t> values,
                                           std::uint64_t address = 0x100000U)
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
    fixture.cfg = cfg.value();
    const auto function = lifter::lift_function(fixture.cfg);
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

TEST_CASE("lifter and interpreter execute scalar arithmetic")
{
    auto fixture = make_fixture({0x8b010000U, 0xd65f03c0U}); // add x0, x0, x1; ret
    REQUIRE(fixture);
    runtime::CpuState cpu;
    cpu.x[0] = 40U;
    cpu.x[1] = 2U;
    REQUIRE(run(fixture.value(), cpu));
    REQUIRE(cpu.x[0] == 42U);
    REQUIRE(cpu.pc == 0x100004U);
}

TEST_CASE("W writes clear the high half of a guest register")
{
    auto fixture = make_fixture({0x0b010000U, 0xd65f03c0U}); // add w0, w0, w1; ret
    REQUIRE(fixture);
    runtime::CpuState cpu;
    cpu.x[0] = UINT64_MAX;
    cpu.x[1] = 2U;
    REQUIRE(run(fixture.value(), cpu));
    REQUIRE(cpu.x[0] == 1U);
}

TEST_CASE("MOVZ and MOVK compose a wide immediate")
{
    auto fixture = make_fixture({0xd2824680U, 0xf2b579a0U, 0xd65f03c0U});
    REQUIRE(fixture);
    runtime::CpuState cpu;
    REQUIRE(run(fixture.value(), cpu));
    REQUIRE(cpu.x[0] == 0xabcd1234U);
}

TEST_CASE("ADDS and CMP produce architectural NZCV flags")
{
    auto adds = make_fixture({0xab010000U, 0xd65f03c0U}); // adds x0, x0, x1; ret
    REQUIRE(adds);
    runtime::CpuState cpu;
    cpu.x[0] = UINT64_MAX;
    cpu.x[1] = 1U;
    REQUIRE(run(adds.value(), cpu));
    REQUIRE(cpu.x[0] == 0U);
    REQUIRE(cpu.n == 0U);
    REQUIRE(cpu.z == 1U);
    REQUIRE(cpu.c == 1U);
    REQUIRE(cpu.v == 0U);

    auto cmp = make_fixture({0xeb01001fU, 0xd65f03c0U}); // cmp x0, x1; ret
    REQUIRE(cmp);
    runtime::CpuState compare_cpu;
    compare_cpu.x[0] = 5U;
    compare_cpu.x[1] = 5U;
    REQUIRE(run(cmp.value(), compare_cpu));
    REQUIRE(compare_cpu.z == 1U);
    REQUIRE(compare_cpu.c == 1U);
}

TEST_CASE("conditional branch selects the correct CFG block")
{
    auto fixture = make_fixture({0xeb01001fU, 0x54000060U, 0xd2800000U, 0xd65f03c0U,
                                 0xd2800020U, 0xd65f03c0U}); // cmp; b.eq +12; movz x0,#0; ret; movz x0,#1; ret
    REQUIRE(fixture);
    runtime::CpuState equal;
    equal.x[0] = 7U;
    equal.x[1] = 7U;
    REQUIRE(run(fixture.value(), equal));
    REQUIRE(equal.x[0] == 1U);

    runtime::CpuState different;
    different.x[0] = 7U;
    different.x[1] = 8U;
    REQUIRE(run(fixture.value(), different));
    REQUIRE(different.x[0] == 0U);
}

TEST_CASE("CBNZ selects the correct CFG block")
{
    auto fixture = make_fixture({0xb5000060U, 0xd2800000U, 0xd65f03c0U,
                                 0xd2800020U, 0xd65f03c0U});
    REQUIRE(fixture);
    runtime::CpuState zero;
    REQUIRE(run(fixture.value(), zero));
    REQUIRE(zero.x[0] == 0U);

    runtime::CpuState nonzero;
    nonzero.x[0] = 7U;
    REQUIRE(run(fixture.value(), nonzero));
    REQUIRE(nonzero.x[0] == 1U);
}

TEST_CASE("PC-relative ADR remains in the guest address domain")
{
    auto fixture = make_fixture({0x10000000U, 0xd65f03c0U}, 0x7100123000U); // adr x0, .; ret
    REQUIRE(fixture);
    runtime::CpuState cpu;
    REQUIRE(run(fixture.value(), cpu));
    REQUIRE(cpu.x[0] == 0x7100123000U);

    auto page_fixture = make_fixture({0x90000000U, 0xd65f03c0U}, 0x7100123456U - 0x56U);
    REQUIRE(page_fixture);
    runtime::CpuState page_cpu;
    REQUIRE(run(page_fixture.value(), page_cpu));
    REQUIRE(page_cpu.x[0] == 0x7100123000U);
}

TEST_CASE("LDR, ADD, and STR use the checked guest-memory boundary")
{
    auto fixture = make_fixture({0xf9400020U, 0x91000400U, 0xf9000020U, 0xd65f03c0U});
    REQUIRE(fixture);
    constexpr memory::GuestAddress data_address = 0x200000U;
    REQUIRE(fixture.value().memory.map(data_address, 16U,
                                       memory::GuestMemoryPermissions::Read |
                                           memory::GuestMemoryPermissions::Write,
                                       "fixture.data", memory::GuestRegionKind::Data));
    runtime::CpuState cpu;
    cpu.x[1] = data_address;
    runtime::RuntimeContext runtime{&fixture.value().memory};
    REQUIRE(runtime::switchrecomp_runtime_guest_store(&runtime, data_address, 8U, 41U) == 0U);
    REQUIRE(interpreter::execute(fixture.value().function, cpu, runtime));
    REQUIRE(cpu.x[0] == 42U);
    std::uint64_t stored = 0U;
    REQUIRE(runtime::switchrecomp_runtime_guest_load(&runtime, data_address, 8U, &stored) == 0U);
    REQUIRE(stored == 42U);
}

TEST_CASE("unsupported calls fail explicitly")
{
    auto fixture = make_fixture({0x94000000U, 0xd65f03c0U}); // bl .
    REQUIRE_FALSE(fixture);
    REQUIRE(fixture.error().code == ErrorCode::UnsupportedInstruction);
}

TEST_CASE("guest memory uses checked little-endian helpers")
{
    memory::GuestMemory memory;
    const memory::GuestAddress data_address = 0x200000U;
    REQUIRE(memory.map(data_address, 16U, memory::GuestMemoryPermissions::Read |
                                             memory::GuestMemoryPermissions::Write,
                       "fixture.data", memory::GuestRegionKind::Data));
    runtime::RuntimeContext runtime{&memory};
    REQUIRE(runtime::switchrecomp_runtime_guest_store(&runtime, data_address, 8U,
                                                       0x1122334455667788U) == 0U);
    std::uint64_t value = 0U;
    REQUIRE(runtime::switchrecomp_runtime_guest_load(&runtime, data_address, 8U, &value) == 0U);
    REQUIRE(value == 0x1122334455667788U);
    std::array<std::byte, 8> bytes{};
    REQUIRE(memory.read(data_address, bytes));
    REQUIRE(std::to_integer<unsigned int>(bytes[0]) == 0x88U);
    REQUIRE(std::to_integer<unsigned int>(bytes[7]) == 0x11U);
}

TEST_CASE("interpreter enforces execution limits and traps")
{
    auto loop = make_fixture({0x14000000U}); // b .
    REQUIRE(loop);
    runtime::CpuState cpu;
    runtime::RuntimeContext loop_runtime{&loop.value().memory};
    runtime::ExecutionOptions options;
    options.max_ir_operations = 3U;
    const auto limited = interpreter::execute(loop.value().function, cpu, loop_runtime, options);
    REQUIRE_FALSE(limited);
    REQUIRE(limited.error().code == ErrorCode::ExecutionLimitExceeded);

    ir::Function trap_function("trap", 0x1000U);
    const auto block = trap_function.add_block(0x1000U, "block_0");
    trap_function.set_entry_block(block);
    ir::Builder builder(trap_function);
    REQUIRE(builder.set_insert_block(block));
    ir::Terminator trap;
    trap.kind = ir::TerminatorKind::Trap;
    trap.trap_reason = "synthetic trap";
    REQUIRE(builder.set_terminator(trap));
    runtime::RuntimeContext trap_runtime;
    runtime::CpuState trap_cpu;
    const auto trapped = interpreter::execute(trap_function, trap_cpu, trap_runtime);
    REQUIRE_FALSE(trapped);
    REQUIRE(trapped.error().code == ErrorCode::ExecutionTrap);
}
