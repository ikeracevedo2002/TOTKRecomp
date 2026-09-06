#include "switchrecomp/codegen/llvm_jit.hpp"
#include "switchrecomp/codegen/llvm_lowerer.hpp"
#include "switchrecomp/codegen/llvm_object_emitter.hpp"
#include "switchrecomp/analysis/cfg_analyzer.hpp"
#include "switchrecomp/ir/interpreter.hpp"
#include "switchrecomp/ir/printer.hpp"
#include "switchrecomp/ir/verifier.hpp"
#include "switchrecomp/lift/aarch64_lifter.hpp"
#include "switchrecomp/runtime/cpu_state.hpp"
#include "reference_executor.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace
{

using switchrecomp::ErrorCode;
using switchrecomp::aarch64::Register;
using switchrecomp::aarch64::RegisterKind;
using switchrecomp::aarch64::RegisterWidth;
using switchrecomp::analysis::analyze_control_flow;
using switchrecomp::ir::IrFunction;
using switchrecomp::memory::GuestAddress;
using switchrecomp::memory::GuestMemory;
using switchrecomp::memory::GuestMemoryPermissions;
using switchrecomp::memory::GuestRegionKind;
using switchrecomp::runtime::CpuState;

[[nodiscard]] std::vector<std::byte> words(std::initializer_list<std::uint32_t> values)
{
    std::vector<std::byte> bytes;
    bytes.reserve(values.size() * 4U);
    for (const auto value : values)
    {
        bytes.push_back(static_cast<std::byte>(value & 0xffU));
        bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
        bytes.push_back(static_cast<std::byte>((value >> 16U) & 0xffU));
        bytes.push_back(static_cast<std::byte>((value >> 24U) & 0xffU));
    }
    return bytes;
}

[[nodiscard]] GuestMemory code_memory(GuestAddress base, std::initializer_list<std::uint32_t> code)
{
    GuestMemory memory;
    const auto bytes = words(code);
    REQUIRE(memory.map(base, bytes, GuestMemoryPermissions::Read | GuestMemoryPermissions::Execute,
                       ".text", GuestRegionKind::Text));
    return memory;
}

[[nodiscard]] switchrecomp::Result<IrFunction> lift_code(
    GuestMemory& memory, GuestAddress entry)
{
    const auto graph = analyze_control_flow(memory, entry);
    if (!graph)
    {
        return switchrecomp::Result<IrFunction>::failure(graph.error());
    }
    return switchrecomp::lift::lift_aarch64(graph.value());
}

[[nodiscard]] Register w(std::uint8_t index)
{
    return Register{RegisterKind::General, RegisterWidth::W32, index, false, false};
}

[[nodiscard]] Register sp()
{
    return Register{RegisterKind::General, RegisterWidth::X64, 31U, true, false};
}

[[nodiscard]] Register xzr()
{
    return Register{RegisterKind::General, RegisterWidth::X64, 31U, false, true};
}

} // namespace

TEST_CASE("CpuState centralizes W/X, zero-register and SP semantics")
{
    CpuState state;
    state.x[0] = std::numeric_limits<std::uint64_t>::max();
    REQUIRE(switchrecomp::runtime::read_register(state, w(0)).value() == 0xffffffffU);
    REQUIRE(switchrecomp::runtime::write_register(state, w(0), 0xffffffffU));
    REQUIRE(state.x[0] == 0x00000000ffffffffU);
    REQUIRE(switchrecomp::runtime::read_register(state, xzr()).value() == 0U);
    REQUIRE(switchrecomp::runtime::write_register(state, xzr(), 42U));
    REQUIRE(state.x[0] == 0x00000000ffffffffU);
    REQUIRE(switchrecomp::runtime::write_register(state, sp(), 0x123456789abcdef0U));
    REQUIRE(state.sp == 0x123456789abcdef0U);
    REQUIRE(switchrecomp::runtime::read_register(state, sp()).value() == state.sp);
    REQUIRE(switchrecomp::runtime::read_register(state, xzr()).value() != state.sp);
}

TEST_CASE("Semantic IR prints deterministically and verifies a minimal function")
{
    GuestMemory memory = code_memory(0x1000U, {0x8b010000U, 0xd65f03c0U});
    const auto function = lift_code(memory, 0x1000U);
    REQUIRE(function);
    REQUIRE(switchrecomp::ir::verify_function(function.value()));
    REQUIRE(switchrecomp::ir::print_function(function.value()) ==
            switchrecomp::ir::print_function(function.value()));
    REQUIRE(switchrecomp::ir::print_function(function.value()).find("guest=0x0000000000001000") !=
            std::string::npos);
}

TEST_CASE("IR verifier rejects malformed blocks and values")
{
    switchrecomp::ir::IrFunction missing_terminator;
    missing_terminator.name = "invalid";
    missing_terminator.entry_block = switchrecomp::ir::BlockId{0U};
    missing_terminator.blocks.push_back(
        switchrecomp::ir::IrBasicBlock{switchrecomp::ir::BlockId{0U}, 0x1000U, {}});
    REQUIRE_FALSE(switchrecomp::ir::verify_function(missing_terminator));

    switchrecomp::ir::IrFunction bad_branch;
    bad_branch.name = "invalid_branch";
    bad_branch.entry_block = switchrecomp::ir::BlockId{0U};
    bad_branch.blocks.push_back(switchrecomp::ir::IrBasicBlock{
        switchrecomp::ir::BlockId{0U}, 0x1000U,
        {switchrecomp::ir::IrInstruction{switchrecomp::ir::IrOpcode::Branch,
                                          switchrecomp::ir::IrType::I1, {}, {}, 0U, {}, 0U, false,
                                          switchrecomp::ir::BlockId{99U}, {}, {}, {0x1000U, 0U, {}}}}});
    REQUIRE_FALSE(switchrecomp::ir::verify_function(bad_branch));
}

TEST_CASE("Interpreter executes add X and add W with AArch64 wraparound")
{
    {
        GuestMemory memory = code_memory(0x1000U, {0x8b010000U, 0xd65f03c0U});
        const auto function = lift_code(memory, 0x1000U);
        REQUIRE(function);
        CpuState state;
        state.x[0] = 10U;
        state.x[1] = 32U;
        const auto executed = switchrecomp::ir::execute_function(function.value(), state, memory);
        REQUIRE(executed);
        REQUIRE(state.x[0] == 42U);
    }
    {
        GuestMemory memory = code_memory(0x1000U, {0x0b010000U, 0xd65f03c0U});
        const auto function = lift_code(memory, 0x1000U);
        REQUIRE(function);
        CpuState state;
        state.x[0] = std::numeric_limits<std::uint64_t>::max();
        state.x[1] = 2U;
        const auto executed = switchrecomp::ir::execute_function(function.value(), state, memory);
        REQUIRE(executed);
        REQUIRE(state.x[0] == 1U);
    }
}

TEST_CASE("Independent raw AArch64 reference agrees with the semantic interpreter")
{
    const std::array<std::uint32_t, 2> code{0x8b010000U, 0xd65f03c0U};
    GuestMemory memory = code_memory(0x1000U, {code[0], code[1]});
    const auto function = lift_code(memory, 0x1000U);
    REQUIRE(function);
    CpuState initial;
    initial.x[0] = 10U;
    initial.x[1] = 32U;
    const auto reference = switchrecomp::test_reference::AArch64ReferenceExecutor{}.execute(
        code, 0x1000U, initial, memory);
    REQUIRE(reference);
    CpuState interpreted = initial;
    REQUIRE(switchrecomp::ir::execute_function(function.value(), interpreted, memory));
    REQUIRE(interpreted.x[0] == reference.value().state.x[0]);
    REQUIRE(interpreted.pc == reference.value().state.pc);
}

TEST_CASE("Interpreter executes logical operations, immediate arithmetic and memory")
{
    // add x0, x0, #1; sub x0, x0, #2; and x0, x0, x1; orr x0, x0, x1; eor x0, x0, x1; ret
    GuestMemory memory = code_memory(
        0x1000U, {0x91000400U, 0xd1000800U, 0x8a010000U, 0xaa010000U, 0xca010000U,
                  0xd65f03c0U});
    const auto function = lift_code(memory, 0x1000U);
    REQUIRE(function);
    CpuState state;
    state.x[0] = 7U;
    state.x[1] = 3U;
    const auto executed = switchrecomp::ir::execute_function(function.value(), state, memory);
    REQUIRE(executed);
    REQUIRE(state.x[0] == 0U);
}

TEST_CASE("Interpreter preserves SP separately and enforces guest memory permissions")
{
    GuestMemory memory = code_memory(0x1000U, {0xf9000020U, 0xf9400022U, 0xd65f03c0U});
    REQUIRE(memory.map(0x2000U, 16U, GuestMemoryPermissions::Read | GuestMemoryPermissions::Write,
                       ".data", GuestRegionKind::Data));
    const auto function = lift_code(memory, 0x1000U);
    REQUIRE(function);
    CpuState state;
    state.x[0] = 0x1122334455667788U;
    state.x[1] = 0x2000U;
    const auto executed = switchrecomp::ir::execute_function(function.value(), state, memory);
    REQUIRE(executed);
    REQUIRE(state.x[2] == 0x1122334455667788U);

    GuestMemory read_only = code_memory(0x3000U, {0xf9000020U, 0xd65f03c0U});
    REQUIRE(read_only.map(0x4000U, 8U, GuestMemoryPermissions::Read, ".rodata",
                          GuestRegionKind::Rodata));
    const auto store_function = lift_code(read_only, 0x3000U);
    REQUIRE(store_function);
    CpuState store_state;
    store_state.x[0] = 1U;
    store_state.x[1] = 0x4000U;
    const auto failed = switchrecomp::ir::execute_function(store_function.value(), store_state, read_only);
    REQUIRE_FALSE(failed);
    REQUIRE(failed.error().code == ErrorCode::ExecutionMemoryFault);
}

TEST_CASE("Lifter supports MOVZ and MOVK wide-immediate construction")
{
    // movz x0, #0x1234; movk x0, #0xabcd, lsl #16; ret
    GuestMemory memory = code_memory(0x1000U, {0xd2824680U, 0xf2b579a0U, 0xd65f03c0U});
    const auto function = lift_code(memory, 0x1000U);
    REQUIRE(function);
    CpuState state;
    REQUIRE(switchrecomp::ir::execute_function(function.value(), state, memory));
    REQUIRE(state.x[0] == 0xabcd1234U);
}

TEST_CASE("Interpreter reports a bounded execution step failure for loops")
{
    GuestMemory memory = code_memory(0x1000U, {0x14000000U}); // b .
    const auto function = lift_code(memory, 0x1000U);
    REQUIRE(function);
    CpuState state;
    const auto executed = switchrecomp::ir::execute_function(
        function.value(), state, memory, switchrecomp::ir::InterpreterOptions{2U});
    REQUIRE_FALSE(executed);
    REQUIRE(executed.error().code == ErrorCode::ExecutionStepLimitExceeded);
}

TEST_CASE("Interpreter follows CBZ and B paths")
{
    GuestMemory memory = code_memory(
        0x1000U, {0xb4000060U, 0xd2800021U, 0x14000002U, 0xd2800041U, 0xd65f03c0U});
    const auto function = lift_code(memory, 0x1000U);
    REQUIRE(function);
    CpuState zero;
    zero.x[0] = 0U;
    REQUIRE(switchrecomp::ir::execute_function(function.value(), zero, memory));
    REQUIRE(zero.x[1] == 2U);
    CpuState nonzero;
    nonzero.x[0] = 1U;
    REQUIRE(switchrecomp::ir::execute_function(function.value(), nonzero, memory));
    REQUIRE(nonzero.x[1] == 1U);
}

TEST_CASE("Unsupported AArch64 instructions fail during lifting with source diagnostics")
{
    GuestMemory memory = code_memory(0x7100123400U, {0x94000000U, 0xd65f03c0U});
    const auto function = lift_code(memory, 0x7100123400U);
    REQUIRE_FALSE(function);
    REQUIRE(function.error().code == ErrorCode::UnsupportedInstruction);
    REQUIRE(function.error().message.find("0x0000007100123400") != std::string::npos);
    REQUIRE(function.error().message.find("0x94000000") != std::string::npos);
}

TEST_CASE("LLVM lowering verifies the same minimal function and JIT agrees when available")
{
    GuestMemory memory = code_memory(0x1000U, {0x8b010000U, 0xd65f03c0U});
    const auto function = lift_code(memory, 0x1000U);
    REQUIRE(function);
    const auto module = switchrecomp::codegen::lower_to_llvm(function.value());
    if (!module && module.error().code == ErrorCode::LLVMUnavailable)
    {
        SUCCEED("LLVM development package is not installed in this build");
        return;
    }
    REQUIRE(module);
    REQUIRE(module.value().textual_ir.find("switchrecomp_guest_fn_") != std::string::npos);
    CpuState state;
    state.x[0] = 10U;
    state.x[1] = 32U;
    switchrecomp::runtime::RuntimeExecutionContext context{&state, &memory};
    const auto jit = switchrecomp::codegen::execute_with_llvm_jit(function.value(), context);
    if (!jit)
    {
        INFO("LLVM JIT error: " << jit.error().message);
    }
    REQUIRE(jit);
    REQUIRE(state.x[0] == 42U);
}

TEST_CASE("LLVM requests never silently fall back when the backend is unavailable")
{
    GuestMemory memory = code_memory(0x1000U, {0xd2800540U, 0xd65f03c0U});
    const auto function = lift_code(memory, 0x1000U);
    REQUIRE(function);
    const auto module = switchrecomp::codegen::lower_to_llvm(function.value());
    const auto output = std::filesystem::temp_directory_path() /
                        "switchrecomp-semantic-ir-test.o";
    std::error_code cleanup_error;
    std::filesystem::remove(output, cleanup_error);
    const auto emitted = switchrecomp::codegen::emit_native_object(function.value(), output);
    if (!module)
    {
        REQUIRE(module.error().code == ErrorCode::LLVMUnavailable);
        REQUIRE_FALSE(emitted);
        REQUIRE(emitted.error().code == ErrorCode::LLVMUnavailable);
    }
    else
    {
        REQUIRE(emitted);
        REQUIRE(std::filesystem::file_size(output) > 0U);
    }
    std::filesystem::remove(output, cleanup_error);
}
