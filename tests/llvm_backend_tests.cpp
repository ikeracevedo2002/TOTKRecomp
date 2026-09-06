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

TEST_CASE("LLVM backend matches interpreter for expanded integer semantics")
{
    constexpr memory::GuestAddress address = 0x110000U;
    auto bytes = code({0xea020020U, 0x9a9f17e0U, 0xd65f03c0U}); // ands; cset; ret
    memory::GuestMemory memory;
    REQUIRE(memory.map(address, std::span<const std::byte>(bytes),
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "llvm.expanded.text", memory::GuestRegionKind::Text));
    analysis::AnalysisOptions options;
    options.allowed_code_range = analysis::GuestAddressRange{
        address, static_cast<memory::GuestSize>(bytes.size())};
    const auto cfg = analysis::analyze_control_flow(memory, address, options);
    REQUIRE(cfg);
    const auto function = lifter::lift_function(cfg.value());
    REQUIRE(function);
    const auto backend = codegen::LlvmBackend::create();
    REQUIRE(backend);

    runtime::CpuState reference_cpu;
    reference_cpu.x[1] = 0x8000000000000000U;
    reference_cpu.x[2] = UINT64_MAX;
    runtime::RuntimeContext reference_context{&memory};
    const auto reference = interpreter::execute(function.value(), reference_cpu, reference_context);
    REQUIRE(reference);

    runtime::CpuState native_cpu = reference_cpu;
    runtime::RuntimeContext native_context{&memory};
    const auto native = backend.value()->execute(function.value(), native_cpu, native_context);
    REQUIRE(native);
    REQUIRE(native_cpu.x == reference_cpu.x);
    REQUIRE(native_cpu.sp == reference_cpu.sp);
    REQUIRE(native_cpu.pc == reference_cpu.pc);
    REQUIRE(native_cpu.n == reference_cpu.n);
    REQUIRE(native_cpu.z == reference_cpu.z);
    REQUIRE(native_cpu.c == reference_cpu.c);
    REQUIRE(native_cpu.v == reference_cpu.v);
    REQUIRE(native.value().status == reference.value().status);
}

TEST_CASE("LLVM backend matches interpreter for typed guest memory")
{
    constexpr memory::GuestAddress address = 0x120000U;
    constexpr memory::GuestAddress data_address = 0x130000U;
    auto bytes = code({0xf9000020U, 0xf9400022U, 0xd65f03c0U}); // str x0; ldr x2; ret
    memory::GuestMemory memory;
    REQUIRE(memory.map(address, std::span<const std::byte>(bytes),
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "llvm.memory.text", memory::GuestRegionKind::Text));
    REQUIRE(memory.map(data_address, 16U,
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Write,
                       "llvm.memory.data", memory::GuestRegionKind::Data));
    analysis::AnalysisOptions options;
    options.allowed_code_range = analysis::GuestAddressRange{
        address, static_cast<memory::GuestSize>(bytes.size())};
    const auto cfg = analysis::analyze_control_flow(memory, address, options);
    REQUIRE(cfg);
    const auto function = lifter::lift_function(cfg.value());
    REQUIRE(function);
    const auto backend = codegen::LlvmBackend::create();
    REQUIRE(backend);

    runtime::CpuState reference_cpu;
    reference_cpu.x[0] = 0x1122334455667788U;
    reference_cpu.x[1] = data_address;
    runtime::RuntimeContext reference_context{&memory};
    const auto reference = interpreter::execute(function.value(), reference_cpu, reference_context);
    REQUIRE(reference);

    runtime::CpuState native_cpu;
    native_cpu.x[0] = 0x1122334455667788U;
    native_cpu.x[1] = data_address;
    runtime::RuntimeContext native_context{&memory};
    const auto native = backend.value()->execute(function.value(), native_cpu, native_context);
    REQUIRE(native);
    REQUIRE(native_cpu.x == reference_cpu.x);
    REQUIRE(native_cpu.sp == reference_cpu.sp);
    REQUIRE(native_cpu.pc == reference_cpu.pc);
    REQUIRE(native_cpu.n == reference_cpu.n);
    REQUIRE(native_cpu.z == reference_cpu.z);
    REQUIRE(native_cpu.c == reference_cpu.c);
    REQUIRE(native_cpu.v == reference_cpu.v);
}
