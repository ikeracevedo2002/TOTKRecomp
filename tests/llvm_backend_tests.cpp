#include "switchrecomp/analysis/cfg_analyzer.hpp"
#include "switchrecomp/codegen/llvm_backend.hpp"
#include "switchrecomp/interpreter/interpreter.hpp"
#include "switchrecomp/lifter/lifter.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

namespace
{

using namespace switchrecomp;

[[nodiscard]] std::vector<std::byte> code(std::initializer_list<std::uint32_t> words)
{
    std::vector<std::byte> bytes;
    for (const auto word : words)
    {
        bytes.push_back(static_cast<std::byte>(word & 0xffU));
        bytes.push_back(static_cast<std::byte>((word >> 8U) & 0xffU));
        bytes.push_back(static_cast<std::byte>((word >> 16U) & 0xffU));
        bytes.push_back(static_cast<std::byte>((word >> 24U) & 0xffU));
    }
    return bytes;
}

} // namespace

TEST_CASE("LLVM backend lowers and executes Semantic IR")
{
    constexpr memory::GuestAddress address = 0x100000U;
    auto bytes = code({0x8b010000U, 0xd65f03c0U});
    memory::GuestMemory memory;
    REQUIRE(memory.map(address, std::span<const std::byte>(bytes),
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "llvm.text", memory::GuestRegionKind::Text));
    analysis::AnalysisOptions options;
    options.allowed_code_range = analysis::GuestAddressRange{
        address, static_cast<memory::GuestSize>(bytes.size())};
    const auto cfg = analysis::analyze_control_flow(memory, address, options);
    REQUIRE(cfg);
    const auto function = lifter::lift_function(cfg.value());
    REQUIRE(function);

    const auto backend = codegen::LlvmBackend::create();
    REQUIRE(backend);
    const auto llvm_ir = backend.value()->lower_to_llvm_ir(function.value());
    REQUIRE(llvm_ir);
    REQUIRE(llvm_ir.value().find("guest_0000000000100000") != std::string::npos);

    runtime::CpuState cpu;
    cpu.x[0] = 40U;
    cpu.x[1] = 2U;
    runtime::RuntimeContext runtime_context{&memory};

    runtime::CpuState reference_cpu;
    reference_cpu.x[0] = 40U;
    reference_cpu.x[1] = 2U;
    runtime::RuntimeContext reference_context{&memory};
    const auto reference = interpreter::execute(function.value(), reference_cpu, reference_context);
    REQUIRE(reference);

    const auto execution = backend.value()->execute(function.value(), cpu, runtime_context);
    REQUIRE(execution);
    REQUIRE(cpu.x[0] == 42U);
    REQUIRE(cpu.pc == address + 4U);
    REQUIRE(execution.value().status == runtime::ExecutionStatus::Returned);
    REQUIRE(reference.value().status == runtime::ExecutionStatus::Returned);
    REQUIRE(cpu.x == reference_cpu.x);
    REQUIRE(cpu.sp == reference_cpu.sp);
    REQUIRE(cpu.pc == reference_cpu.pc);
    REQUIRE(cpu.n == reference_cpu.n);
    REQUIRE(cpu.z == reference_cpu.z);
    REQUIRE(cpu.c == reference_cpu.c);
    REQUIRE(cpu.v == reference_cpu.v);
}
