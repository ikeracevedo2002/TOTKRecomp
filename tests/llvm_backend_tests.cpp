#include "switchrecomp/analysis/cfg_analyzer.hpp"
#include "switchrecomp/codegen/llvm_backend.hpp"
#include "switchrecomp/interpreter/interpreter.hpp"
#include "switchrecomp/lifter/lifter.hpp"
#include "switchrecomp/runtime/atomic_memory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
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

TEST_CASE("LLVM M9 backend matches interpreter for TLS and exclusive ordering")
{
    constexpr memory::GuestAddress code_address = 0x140000U;
    constexpr memory::GuestAddress data_address = 0x150000U;
    const auto bytes = code({
        0xaa0503e0U, // mov x0, x5
        0xd51bd040U, // msr tpidr_el0, x0
        0xd53bd044U, // mrs x4, tpidr_el0
        0xc85ffc20U, // ldaxr x0, [x1]
        0xc802fc20U, // stlxr w2, x0, [x1]
        0xd503305fU, // clrex
        0xd5033fbfU, // dmb sy
        0xd5033f9fU, // dsb sy
        0xd5033fdfU, // isb
        0xd65f03c0U, // ret
    });

    const auto make_memory = [&]() {
        memory::GuestMemory result;
        REQUIRE(result.map(code_address, std::span<const std::byte>(bytes),
                           memory::GuestMemoryPermissions::Read |
                               memory::GuestMemoryPermissions::Execute,
                           "llvm.m9.text", memory::GuestRegionKind::Text));
        REQUIRE(result.map(data_address, 16U,
                           memory::GuestMemoryPermissions::Read |
                               memory::GuestMemoryPermissions::Write,
                           "llvm.m9.data", memory::GuestRegionKind::Data));
        return result;
    };

    auto reference_memory = make_memory();
    auto native_memory = make_memory();
    analysis::AnalysisOptions options;
    options.allowed_code_range = analysis::GuestAddressRange{
        code_address, static_cast<memory::GuestSize>(bytes.size())};
    const auto cfg = analysis::analyze_control_flow(reference_memory, code_address, options);
    REQUIRE(cfg);
    const auto function = lifter::lift_function(cfg.value());
    REQUIRE(function);

    const auto backend = codegen::LlvmBackend::create();
    REQUIRE(backend);
    const auto llvm_ir = backend.value()->lower_to_llvm_ir(function.value());
    REQUIRE(llvm_ir);
    REQUIRE(llvm_ir.value().find("switchrecomp_runtime_exclusive_load") != std::string::npos);
    REQUIRE(llvm_ir.value().find("switchrecomp_runtime_read_system_register") != std::string::npos);

    runtime::SharedRuntimeState reference_shared(reference_memory);
    runtime::CpuState reference_cpu;
    reference_cpu.x[1] = data_address;
    reference_cpu.x[5] = 0x123456789abcdef0U;
    REQUIRE(runtime::synchronized_store(reference_shared, data_address, 8U, 0x55U,
                                         ir::MemoryOrder::Relaxed));
    runtime::RuntimeContext reference_context{&reference_memory};
    reference_context.shared = &reference_shared;
    reference_context.cpu = &reference_cpu;
    const auto reference = interpreter::execute(function.value(), reference_cpu, reference_context);
    REQUIRE(reference);

    runtime::SharedRuntimeState native_shared(native_memory);
    runtime::CpuState native_cpu;
    native_cpu.x[1] = data_address;
    native_cpu.x[5] = 0x123456789abcdef0U;
    REQUIRE(runtime::synchronized_store(native_shared, data_address, 8U, 0x55U,
                                         ir::MemoryOrder::Relaxed));
    runtime::RuntimeContext native_context{&native_memory};
    native_context.shared = &native_shared;
    native_context.cpu = &native_cpu;
    const auto native = backend.value()->execute(function.value(), native_cpu, native_context);
    REQUIRE(native);

    REQUIRE(native.value().status == reference.value().status);
    REQUIRE(native_cpu.x == reference_cpu.x);
    REQUIRE(native_cpu.sp == reference_cpu.sp);
    REQUIRE(native_cpu.pc == reference_cpu.pc);
    REQUIRE(native_cpu.n == reference_cpu.n);
    REQUIRE(native_cpu.z == reference_cpu.z);
    REQUIRE(native_cpu.c == reference_cpu.c);
    REQUIRE(native_cpu.v == reference_cpu.v);
    REQUIRE(native_cpu.tpidr_el0 == reference_cpu.tpidr_el0);
    REQUIRE(native_cpu.tpidrro_el0 == reference_cpu.tpidrro_el0);
    const auto reference_data = runtime::synchronized_load(reference_shared, data_address, 8U,
                                                            ir::MemoryOrder::Relaxed);
    const auto native_data = runtime::synchronized_load(native_shared, data_address, 8U,
                                                        ir::MemoryOrder::Relaxed);
    REQUIRE(reference_data);
    REQUIRE(native_data);
    REQUIRE(native_data.value() == reference_data.value());
}
