#include "switchrecomp/aarch64/decoder.hpp"
#include "switchrecomp/analysis/coverage.hpp"
#include "switchrecomp/analysis/cfg_analyzer.hpp"
#ifdef TOTKRECOMP_HAS_LLVM
#include "switchrecomp/codegen/llvm_backend.hpp"
#endif
#include "switchrecomp/interpreter/interpreter.hpp"
#include "switchrecomp/ir/builder.hpp"
#include "switchrecomp/ir/verifier.hpp"
#include "switchrecomp/lifter/lifter.hpp"
#include "switchrecomp/runtime/context.hpp"
#include "switchrecomp/runtime/cpu_state.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <utility>
#include <vector>

namespace
{

using namespace switchrecomp;

constexpr memory::GuestAddress code_address = 0x7000U;
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
    auto bytes = code(words);
    memory::GuestMemory memory;
    const auto mapped = memory.map(code_address, std::span<const std::byte>(bytes),
                                   memory::GuestMemoryPermissions::Read |
                                       memory::GuestMemoryPermissions::Execute,
                                   "m38.semantic.text", memory::GuestRegionKind::Text);
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

struct Flags
{
    std::uint32_t n;
    std::uint32_t z;
    std::uint32_t c;
    std::uint32_t v;
};

void require_flags(const runtime::CpuState& cpu, Flags expected)
{
    REQUIRE(cpu.n == expected.n);
    REQUIRE(cpu.z == expected.z);
    REQUIRE(cpu.c == expected.c);
    REQUIRE(cpu.v == expected.v);
}

} // namespace

TEST_CASE("M38 conditional compare encodings normalize as liftable scalar operations")
{
    const auto decoder = aarch64::AArch64Decoder::create();
    REQUIRE(decoder);
    struct Case
    {
        std::uint32_t word;
        aarch64::InstructionId id;
    };
    constexpr Case cases[]{
        {0xfa410002U, aarch64::InstructionId::Ccmp}, // ccmp x0, x1, #2, eq
        {0x3a431045U, aarch64::InstructionId::Ccmn}, // ccmn w2, w3, #5, ne
        {0x7a47c880U, aarch64::InstructionId::Ccmp}, // ccmp w4, #7, #0, gt
        {0x1e21d40fU, aarch64::InstructionId::FpSimd},   // fccmp s0, s1, #0xf, le
        {0x1e634453U, aarch64::InstructionId::FpSimd},   // fccmpe d2, d3, #3, mi
    };
    for (const auto& test : cases)
    {
        const auto decoded = decoder.value()->decode(code_address, test.word);
        REQUIRE(decoded);
        REQUIRE(decoded.value().id == test.id);
        REQUIRE(decoded.value().normalized);
        REQUIRE(lifter::is_instruction_liftable(decoded.value()));
    }
    const auto ccmp = decoder.value()->decode(code_address, 0xfa410002U);
    REQUIRE(ccmp.value().simd_operation == aarch64::SimdOperation::None);
    const auto fccmp = decoder.value()->decode(code_address, 0x1e21d40fU);
    REQUIRE(fccmp.value().simd_operation == aarch64::SimdOperation::Fccmp);
    const auto fccmpe = decoder.value()->decode(code_address, 0x1e634453U);
    REQUIRE(fccmpe.value().simd_operation == aarch64::SimdOperation::Fccmpe);
}

TEST_CASE("M38 CCMP selects computed flags when its condition holds")
{
    auto program = lift_program({0xfa410002U, ret}); // ccmp x0, x1, #2, eq
    REQUIRE(program);
    REQUIRE(ir::verify(program.value().function));

    runtime::CpuState taken;
    taken.x[0] = 5U;
    taken.x[1] = 5U;
    taken.n = 0U;
    taken.z = 1U; // eq holds
    taken.c = 0U;
    taken.v = 1U;
    REQUIRE(execute_program(program.value(), taken));
    require_flags(taken, Flags{0U, 1U, 1U, 0U}); // 5 - 5 = 0

    runtime::CpuState fallback;
    fallback.x[0] = 5U;
    fallback.x[1] = 5U;
    fallback.n = 0U;
    fallback.z = 0U; // eq fails
    fallback.c = 0U;
    fallback.v = 1U;
    REQUIRE(execute_program(program.value(), fallback));
    require_flags(fallback, Flags{0U, 0U, 1U, 0U}); // nzcv = #2
}

TEST_CASE("M38 CCMN selects computed flags when its condition holds")
{
    auto program = lift_program({0x3a431045U, ret}); // ccmn w2, w3, #5, ne
    REQUIRE(program);
    REQUIRE(ir::verify(program.value().function));

    runtime::CpuState taken;
    taken.x[2] = 1U;
    taken.x[3] = 2U;
    taken.n = 1U;
    taken.z = 0U; // ne holds
    taken.c = 1U;
    taken.v = 1U;
    REQUIRE(execute_program(program.value(), taken));
    require_flags(taken, Flags{0U, 0U, 0U, 0U}); // 1 + 2 = 3

    runtime::CpuState fallback;
    fallback.x[2] = 1U;
    fallback.x[3] = 2U;
    fallback.n = 0U;
    fallback.z = 1U; // ne fails
    fallback.c = 0U;
    fallback.v = 0U;
    REQUIRE(execute_program(program.value(), fallback));
    require_flags(fallback, Flags{0U, 1U, 0U, 1U}); // nzcv = #5
}

TEST_CASE("M38 CCMP immediate form respects the condition and fallback")
{
    auto program = lift_program({0x7a47c880U, ret}); // ccmp w4, #7, #0, gt
    REQUIRE(program);

    runtime::CpuState taken;
    taken.x[4] = 10U;
    taken.n = 0U;
    taken.z = 0U; // gt holds: Z == 0 && N == V
    taken.c = 0U;
    taken.v = 0U;
    REQUIRE(execute_program(program.value(), taken));
    require_flags(taken, Flags{0U, 0U, 1U, 0U}); // 10 - 7 = 3

    runtime::CpuState fallback;
    fallback.x[4] = 3U;
    fallback.n = 1U;
    fallback.z = 0U; // gt fails on the incoming flags: N != V
    fallback.c = 0U;
    fallback.v = 0U;
    REQUIRE(execute_program(program.value(), fallback));
    require_flags(fallback, Flags{0U, 0U, 0U, 0U}); // nzcv = #0
}

TEST_CASE("M38 FCCMP updates FPSR and conditionally overrides NZCV")
{
    auto program = lift_program({0x1e21d40fU, ret}); // fccmp s0, s1, #0xf, le
    REQUIRE(program);
    REQUIRE(ir::verify(program.value().function));

    runtime::CpuState taken;
    taken.vreg[0] = runtime::Vector128{0x000000003f800000ULL, 0U}; // s0 = 1.0
    taken.vreg[1] = runtime::Vector128{0x0000000040000000ULL, 0U}; // s1 = 2.0
    taken.n = 0U;
    taken.z = 1U; // le holds because less-than sets N != V
    taken.c = 1U;
    taken.v = 0U;
    REQUIRE(execute_program(program.value(), taken));
    require_flags(taken, Flags{1U, 0U, 0U, 0U}); // 1.0 < 2.0

    runtime::CpuState fallback;
    fallback.vreg[0] = runtime::Vector128{0x0000000040000000ULL, 0U}; // s0 = 2.0
    fallback.vreg[1] = runtime::Vector128{0x000000003f800000ULL, 0U}; // s1 = 1.0
    fallback.n = 0U;
    fallback.z = 0U; // le fails for greater-than
    fallback.c = 0U;
    fallback.v = 0U;
    REQUIRE(execute_program(program.value(), fallback));
    require_flags(fallback, Flags{1U, 1U, 1U, 1U}); // nzcv = #0xf
}

TEST_CASE("M38 UDIV and SDIV produce architectural quotients for W and X")
{
    struct Case
    {
        std::uint32_t word;
        std::uint64_t left;
        std::uint64_t right;
        std::uint64_t expected;
    };
    constexpr Case cases[]{
        {0x9ac20820U, 100U, 7U, 14U},                             // udiv x0, x1, x2
        {0x9ac20820U, 7U, 100U, 0U},                              // udiv truncates
        {0x9ac20820U, UINT64_MAX, 1U, UINT64_MAX},                // udiv unsigned max
        {0x9ac20820U, 42U, 0U, 0U},                               // udiv by zero = 0
        {0x9ac20c20U, 100U, 7U, 14U},                             // sdiv x0, x1, x2
        {0x9ac20c20U, 0xffffffffffffff9cULL, 7U, 0xfffffffffffffff2ULL}, // -100 / 7
        {0x9ac20c20U, 0x8000000000000000ULL, UINT64_MAX, 0x8000000000000000ULL}, // INT64_MIN / -1
        {0x9ac20c20U, 0xffffffffffffffffULL, 0U, 0U},             // sdiv by zero = 0
        {0x1ac20c20U, 100U, 7U, 14U},                             // sdiv w0, w1, w2
        {0x1ac20c20U, 0xffffff9cULL, 7U, 0xfffffff2ULL},          // -100 / 7 in 32 bits
        {0x1ac20c20U, 0x80000000ULL, 0xffffffffULL, 0x80000000ULL}, // INT32_MIN / -1
        {0x1ac20c20U, 100U, 0U, 0U},                              // sdiv by zero = 0
    };
    for (const auto& test : cases)
    {
        auto program = lift_program({test.word, ret});
        REQUIRE(program);
        REQUIRE(ir::verify(program.value().function));
        runtime::CpuState cpu;
        cpu.x[1] = test.left;
        cpu.x[2] = test.right;
        REQUIRE(execute_program(program.value(), cpu));
        REQUIRE(cpu.x[0] == test.expected);
    }
}

TEST_CASE("M38 divide verifier rejects mismatched or narrow operands")
{
    const auto make_function = [](ir::Type left_type, ir::Type right_type, ir::Type result_type) {
        ir::Function function("m38_malformed_divide", code_address);
        const auto block = function.add_block(code_address, "entry");
        function.set_entry_block(block);
        ir::Builder builder(function);
        REQUIRE(builder.set_insert_block(block));
        const auto left = builder.constant(left_type, 1U, ir::SourceLocation{code_address, 0U, "m38"});
        const auto right = builder.constant(right_type, 1U, ir::SourceLocation{code_address, 0U, "m38"});
        REQUIRE(left);
        REQUIRE(right);
        ir::Instruction divide;
        divide.opcode = ir::Opcode::DivideSigned;
        divide.result_type = result_type;
        divide.operands = {left.value(), right.value()};
        divide.source = ir::SourceLocation{code_address, 0U, "m38"};
        REQUIRE(builder.emit(std::move(divide)));
        ir::Terminator terminator;
        terminator.kind = ir::TerminatorKind::Return;
        terminator.source = ir::SourceLocation{code_address, 4U, "m38"};
        REQUIRE(builder.set_terminator(std::move(terminator)));
        return function;
    };
    REQUIRE_FALSE(ir::verify(make_function(ir::i32_type(), ir::i64_type(), ir::i32_type())));
    REQUIRE_FALSE(ir::verify(make_function(ir::i16_type(), ir::i16_type(), ir::i16_type())));
    REQUIRE(ir::verify(make_function(ir::i32_type(), ir::i32_type(), ir::i32_type())));
}

TEST_CASE("M38 coverage scanner now classifies conditional compare and divide as liftable")
{
    const auto bytes = code({0xfa410002U, 0x3a431045U, 0x9ac20820U, 0x1ac20c20U, 0x1e21d40fU, ret});
    memory::GuestMemory memory;
    REQUIRE(memory.map(code_address, std::span<const std::byte>(bytes),
                       memory::GuestMemoryPermissions::Read |
                           memory::GuestMemoryPermissions::Execute,
                       "m38.coverage.text", memory::GuestRegionKind::Text));
    const auto report = analysis::scan_coverage(memory, code_address, bytes.size(), "m38.bin");
    REQUIRE(report);
    REQUIRE(report.value().decoded == 6U);
    REQUIRE(report.value().liftable == 6U);
    REQUIRE(report.value().unsupported == 0U);
}

#ifdef TOTKRECOMP_HAS_LLVM
TEST_CASE("M38 conditional compare and divide interpreter and LLVM execution agree")
{
    constexpr std::uint32_t programs[][2]{
        {0xfa410002U, 0x3a431045U},
        {0x9ac20820U, 0x1ac20c20U},
    };
    for (const auto& words : programs)
    {
        auto program = lift_program({words[0], words[1], ret});
        REQUIRE(program);
        const auto backend = codegen::LlvmBackend::create();
        REQUIRE(backend);

        runtime::CpuState interpreted;
        interpreted.x[0] = 5U;
        interpreted.x[1] = 5U;
        interpreted.x[2] = 0xffffff9cULL;
        interpreted.x[3] = 7U;
        interpreted.n = 0U;
        interpreted.z = 1U;
        interpreted.c = 0U;
        interpreted.v = 1U;
        runtime::RuntimeContext interpreted_context{&program.value().memory};
        REQUIRE(interpreter::execute(program.value().function, interpreted, interpreted_context));

        runtime::CpuState llvm;
        llvm.x[0] = interpreted.x[0];
        llvm.x[1] = interpreted.x[1];
        llvm.x[2] = interpreted.x[2];
        llvm.x[3] = interpreted.x[3];
        llvm.n = 0U;
        llvm.z = 1U;
        llvm.c = 0U;
        llvm.v = 1U;
        runtime::RuntimeContext llvm_context{&program.value().memory};
        REQUIRE(backend.value()->execute(program.value().function, llvm, llvm_context));
        REQUIRE(llvm.x == interpreted.x);
        REQUIRE(llvm.n == interpreted.n);
        REQUIRE(llvm.z == interpreted.z);
        REQUIRE(llvm.c == interpreted.c);
        REQUIRE(llvm.v == interpreted.v);
    }
}
#endif
