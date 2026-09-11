#include "switchrecomp/ir/builder.hpp"
#include "switchrecomp/ir/printer.hpp"
#include "switchrecomp/ir/verifier.hpp"
#include "switchrecomp/runtime/cpu_state.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace switchrecomp;

TEST_CASE("semantic IR is typed, printable, and deterministic")
{
    ir::Function function("guest_0000000000100000", 0x100000U);
    const auto entry = function.add_block(0x100000U, "block_0");
    function.set_entry_block(entry);
    ir::Builder builder(function);
    REQUIRE(builder.set_insert_block(entry));
    const auto left = builder.constant(ir::i64_type(), 40U);
    const auto right = builder.constant(ir::i64_type(), 2U);
    REQUIRE(left);
    REQUIRE(right);
    ir::Instruction add;
    add.opcode = ir::Opcode::Add;
    add.result_type = ir::i64_type();
    add.operands = {left.value(), right.value()};
    const auto sum = builder.emit(add);
    REQUIRE(sum);
    ir::Terminator ret;
    ret.kind = ir::TerminatorKind::Return;
    REQUIRE(builder.set_terminator(ret));

    REQUIRE(ir::verify(function));
    const auto rendered = ir::print(function);
    REQUIRE(rendered.find("%2:i64 = add %0, %1") != std::string::npos);
    REQUIRE(rendered == ir::print(function));
}

TEST_CASE("verifier rejects malformed semantic IR")
{
    SECTION("missing entry")
    {
        ir::Function function("broken", 0x1000U);
        const auto block = function.add_block(0x1000U, "block_0");
        ir::Builder builder(function);
        REQUIRE(builder.set_insert_block(block));
        ir::Terminator ret;
        ret.kind = ir::TerminatorKind::Return;
        REQUIRE(builder.set_terminator(ret));
        REQUIRE_FALSE(ir::verify(function));
    }

    SECTION("missing terminator")
    {
        ir::Function function("broken", 0x1000U);
        const auto block = function.add_block(0x1000U, "block_0");
        function.set_entry_block(block);
        REQUIRE_FALSE(ir::verify(function));
    }

    SECTION("branch to missing block")
    {
        ir::Function function("broken", 0x1000U);
        const auto block = function.add_block(0x1000U, "block_0");
        function.set_entry_block(block);
        ir::Builder builder(function);
        REQUIRE(builder.set_insert_block(block));
        ir::Terminator branch;
        branch.kind = ir::TerminatorKind::Branch;
        branch.target = 99U;
        REQUIRE(builder.set_terminator(branch));
        REQUIRE_FALSE(ir::verify(function));
    }

    SECTION("constant does not fit")
    {
        ir::Function function("broken", 0x1000U);
        const auto block = function.add_block(0x1000U, "block_0");
        function.set_entry_block(block);
        ir::Builder builder(function);
        REQUIRE(builder.set_insert_block(block));
        const auto value = builder.constant(ir::i8_type(), 0x100U);
        REQUIRE(value);
        ir::Terminator ret;
        ret.kind = ir::TerminatorKind::Return;
        REQUIRE(builder.set_terminator(ret));
        REQUIRE_FALSE(ir::verify(function));
    }
}

TEST_CASE("CPU state implements W/X and zero-register semantics")
{
    runtime::CpuState state;
    const ir::GuestRegister x0{ir::RegisterWidth::X64, 0U, false, false};
    const ir::GuestRegister w0{ir::RegisterWidth::W32, 0U, false, false};
    const ir::GuestRegister xzr{ir::RegisterWidth::X64, 31U, false, true};
    const ir::GuestRegister sp{ir::RegisterWidth::X64, 31U, true, false};

    runtime::write_register(state, x0, UINT64_MAX);
    REQUIRE(runtime::read_register(state, x0) == UINT64_MAX);
    runtime::write_register(state, w0, 0x12345678U);
    REQUIRE(runtime::read_register(state, x0) == 0x12345678U);
    REQUIRE(runtime::read_register(state, xzr) == 0U);
    runtime::write_register(state, xzr, UINT64_MAX);
    REQUIRE(runtime::read_register(state, xzr) == 0U);
    runtime::write_register(state, sp, 0x71000000U);
    REQUIRE(state.sp == 0x71000000U);
}
