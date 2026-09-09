#include "switchrecomp/analysis/function_map.hpp"
#include "switchrecomp/execution/session.hpp"
#include "switchrecomp/interpreter/interpreter.hpp"
#include "switchrecomp/ir/builder.hpp"
#include "switchrecomp/ir/verifier.hpp"
#include "switchrecomp/version.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace switchrecomp;

constexpr ir::GuestRegister x0{ir::RegisterWidth::X64, 0U, false, false};
constexpr ir::GuestRegister x1{ir::RegisterWidth::X64, 1U, false, false};

void set_pc(ir::Builder& builder, std::uint64_t pc)
{
    ir::Instruction instruction;
    instruction.opcode = ir::Opcode::SetPc;
    instruction.result_type = ir::void_type();
    instruction.source.guest_pc = pc;
    REQUIRE(builder.emit_void(std::move(instruction)));
}

ir::ValueId constant(ir::Builder& builder, std::uint64_t value,
                     std::uint64_t source_pc = 0U)
{
    auto result = builder.constant(ir::i64_type(), value, ir::SourceLocation{source_pc, 0U, {}});
    REQUIRE(result);
    return result.value();
}

void write_register(ir::Builder& builder, ir::GuestRegister reg, ir::ValueId value,
                    std::uint64_t source_pc = 0U)
{
    ir::Instruction instruction;
    instruction.opcode = ir::Opcode::WriteRegister;
    instruction.result_type = ir::void_type();
    instruction.operands = {value};
    instruction.reg = reg;
    instruction.source.guest_pc = source_pc;
    REQUIRE(builder.emit_void(std::move(instruction)));
}

void guest_store(ir::Builder& builder, ir::ValueId address, ir::ValueId value,
                 std::uint8_t size, std::uint64_t source_pc)
{
    ir::Instruction instruction;
    instruction.opcode = ir::Opcode::GuestStore;
    instruction.result_type = ir::void_type();
    instruction.operands = {address, value};
    instruction.memory_size = size;
    instruction.source.guest_pc = source_pc;
    REQUIRE(builder.emit_void(std::move(instruction)));
}

void return_from(ir::Builder& builder, std::uint64_t source_pc = 0U)
{
    ir::Terminator terminator;
    terminator.kind = ir::TerminatorKind::Return;
    terminator.source.guest_pc = source_pc;
    REQUIRE(builder.set_terminator(std::move(terminator)));
}

ir::Function make_equivalence_function()
{
    ir::Function function("m29-equivalence", 0x1000U);
    const auto entry = function.add_block(0x1000U, "entry");
    const auto continuation = function.add_block(0x1004U, "continuation");
    function.set_entry_block(entry);
    ir::Builder builder(function);

    REQUIRE(builder.set_insert_block(entry));
    set_pc(builder, 0x1000U);
    const auto data = constant(builder, 0x3000U, 0x1000U);
    const auto first = constant(builder, 0x1122334455667788U, 0x1000U);
    guest_store(builder, data, first, 8U, 0x1000U);
    const auto seven = constant(builder, 7U, 0x1000U);
    write_register(builder, x0, seven, 0x1000U);
    ir::Terminator branch;
    branch.kind = ir::TerminatorKind::Branch;
    branch.target = continuation;
    branch.source.guest_pc = 0x1000U;
    REQUIRE(builder.set_terminator(std::move(branch)));

    REQUIRE(builder.set_insert_block(continuation));
    set_pc(builder, 0x1004U);
    const auto current = [&]() {
        ir::Instruction read;
        read.opcode = ir::Opcode::ReadRegister;
        read.result_type = ir::i64_type();
        read.reg = x0;
        read.source.guest_pc = 0x1004U;
        const auto result = builder.emit(std::move(read));
        REQUIRE(result);
        return result.value();
    }();
    const auto one = constant(builder, 1U, 0x1004U);
    ir::Instruction add;
    add.opcode = ir::Opcode::Add;
    add.result_type = ir::i64_type();
    add.operands = {current, one};
    add.source.guest_pc = 0x1004U;
    const auto sum = builder.emit(std::move(add));
    REQUIRE(sum);
    write_register(builder, x1, sum.value(), 0x1004U);
    return_from(builder, 0x1004U);
    return function;
}

ir::Function make_store_pair_function()
{
    ir::Function function("m29-store-pair", 0x2000U);
    const auto block = function.add_block(0x2000U, "entry");
    function.set_entry_block(block);
    ir::Builder builder(function);
    REQUIRE(builder.set_insert_block(block));
    set_pc(builder, 0x2000U);
    const auto address = constant(builder, 0x4000U, 0x2000U);
    const auto low = constant(builder, 0x0102030405060708U, 0x2000U);
    guest_store(builder, address, low, 8U, 0x2000U);
    ir::Instruction add;
    add.opcode = ir::Opcode::GuestAddressAdd;
    add.result_type = ir::i64_type();
    add.operands = {address};
    add.immediate = 8;
    add.source.guest_pc = 0x2000U;
    const auto second_address = builder.emit(std::move(add));
    REQUIRE(second_address);
    const auto high = constant(builder, 0x1112131415161718U, 0x2000U);
    guest_store(builder, second_address.value(), high, 8U, 0x2000U);
    return_from(builder, 0x2000U);
    return function;
}

ir::Function make_observation_function()
{
    ir::Function function("m29-observation", 0x2100U);
    const auto block = function.add_block(0x2100U, "entry");
    function.set_entry_block(block);
    ir::Builder builder(function);
    REQUIRE(builder.set_insert_block(block));
    set_pc(builder, 0x2100U);
    const auto value = constant(builder, 0x55U, 0x2100U);
    write_register(builder, x0, value, 0x2100U);
    set_pc(builder, 0x2104U);
    return_from(builder, 0x2104U);
    return function;
}

ir::Function make_three_block_function()
{
    ir::Function function("m29-blocks", 0x3000U);
    const auto first = function.add_block(0x3000U, "first");
    const auto second = function.add_block(0x3004U, "second");
    const auto third = function.add_block(0x3008U, "third");
    function.set_entry_block(first);
    ir::Builder builder(function);
    REQUIRE(builder.set_insert_block(first));
    set_pc(builder, 0x3000U);
    ir::Terminator first_branch;
    first_branch.kind = ir::TerminatorKind::Branch;
    first_branch.target = second;
    first_branch.source.guest_pc = 0x3000U;
    REQUIRE(builder.set_terminator(std::move(first_branch)));
    REQUIRE(builder.set_insert_block(second));
    set_pc(builder, 0x3004U);
    ir::Terminator second_branch;
    second_branch.kind = ir::TerminatorKind::Branch;
    second_branch.target = third;
    second_branch.source.guest_pc = 0x3004U;
    REQUIRE(builder.set_terminator(std::move(second_branch)));
    REQUIRE(builder.set_insert_block(third));
    set_pc(builder, 0x3008U);
    return_from(builder, 0x3008U);
    return function;
}

struct SlicedRun
{
    runtime::ExecutionResult result;
    interpreter::InterpreterFrame frame;
};

SlicedRun run_sliced(const ir::Function& function, memory::GuestMemory& memory,
                     runtime::CpuState& cpu, std::size_t quantum,
                     std::optional<std::size_t> hard_limit = std::nullopt,
                     std::span<const std::uint64_t> observed = {})
{
    runtime::SharedRuntimeState shared(memory);
    runtime::RuntimeContext context{&memory};
    context.shared = &shared;
    SlicedRun run;
    while (true)
    {
        runtime::ExecutionOptions options;
        if (hard_limit)
        {
            options.max_ir_operations = hard_limit.value() - run.result.executed_operations;
        }
        options.slice_ir_operations = quantum;
        options.observed_guest_pcs = observed;
        const auto step = interpreter::execute_until_boundary(
            function, cpu, context, run.frame, options);
        REQUIRE(step);
        const auto& part = step.value();
        run.result.executed_operations += part.executed_operations;
        run.result.executed_blocks += part.executed_blocks;
        run.result.executed_guest_instructions += part.executed_guest_instructions;
        run.result.observed_guest_pcs.insert(run.result.observed_guest_pcs.end(),
                                             part.observed_guest_pcs.begin(),
                                             part.observed_guest_pcs.end());
        run.result.observed_instruction_executions.insert(
            run.result.observed_instruction_executions.end(),
            part.observed_instruction_executions.begin(),
            part.observed_instruction_executions.end());
        run.result.executed_guest_pcs.insert(run.result.executed_guest_pcs.end(),
                                             part.executed_guest_pcs.begin(),
                                             part.executed_guest_pcs.end());
        run.result.final_guest_pc = part.final_guest_pc;
        run.result.status = part.status;
        run.result.boundary = part.boundary;
        if (part.status != runtime::ExecutionStatus::Yielded) return run;
    }
}

bool same_cpu(const runtime::CpuState& left, const runtime::CpuState& right)
{
    return left.x == right.x && left.sp == right.sp && left.pc == right.pc &&
           left.n == right.n && left.z == right.z && left.c == right.c && left.v == right.v &&
           left.fpcr == right.fpcr && left.fpsr == right.fpsr && left.vreg == right.vreg &&
           left.tpidr_el0 == right.tpidr_el0 && left.tpidrro_el0 == right.tpidrro_el0;
}

void require_same_observations(const std::vector<runtime::ObservedInstructionExecution>& left,
                               const std::vector<runtime::ObservedInstructionExecution>& right)
{
    REQUIRE(left.size() == right.size());
    for (std::size_t index = 0U; index < left.size(); ++index)
    {
        REQUIRE(left[index].guest_pc == right[index].guest_pc);
        REQUIRE(same_cpu(left[index].pre_state, right[index].pre_state));
        REQUIRE(same_cpu(left[index].post_state, right[index].post_state));
        REQUIRE(left[index].next_guest_pc == right[index].next_guest_pc);
    }
}

void require_same_run(const SlicedRun& left, const runtime::CpuState& left_cpu,
                      const SlicedRun& right, const runtime::CpuState& right_cpu)
{
    REQUIRE(left.result.status == right.result.status);
    REQUIRE(left.result.boundary.kind == right.result.boundary.kind);
    REQUIRE(left.result.final_guest_pc == right.result.final_guest_pc);
    REQUIRE(left.result.executed_operations == right.result.executed_operations);
    REQUIRE(left.result.executed_blocks == right.result.executed_blocks);
    REQUIRE(left.result.executed_guest_instructions == right.result.executed_guest_instructions);
    REQUIRE(left.result.executed_guest_pcs == right.result.executed_guest_pcs);
    REQUIRE(left.result.observed_guest_pcs == right.result.observed_guest_pcs);
    require_same_observations(left.result.observed_instruction_executions,
                              right.result.observed_instruction_executions);
    REQUIRE(same_cpu(left_cpu, right_cpu));
}

std::vector<std::byte> words(std::initializer_list<std::uint32_t> values)
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

std::uint32_t branch(std::uint64_t pc, std::uint64_t target)
{
    const auto displacement = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pc);
    return 0x14000000U | (static_cast<std::uint32_t>(displacement / 4) & 0x03ffffffU);
}

std::uint32_t bl(std::uint64_t pc, std::uint64_t target)
{
    const auto displacement = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pc);
    return 0x94000000U | (static_cast<std::uint32_t>(displacement / 4) & 0x03ffffffU);
}

std::uint32_t movz(std::uint8_t reg, std::uint16_t value)
{
    return 0xd2800000U | (static_cast<std::uint32_t>(value) << 5U) | reg;
}

struct SessionFixture
{
    memory::GuestMemory memory;
    analysis::FinalizedFunctionMap map;
};

Result<SessionFixture> make_session_fixture(memory::GuestAddress base,
                                             std::span<const std::byte> code,
                                             std::vector<analysis::FunctionSeed> seeds)
{
    SessionFixture fixture;
    const auto mapped = fixture.memory.map(
        base, code, memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Execute,
        "m29.synthetic.text", memory::GuestRegionKind::Text);
    if (!mapped) return Result<SessionFixture>::failure(mapped.error());
    analysis::ModuleIdentity identity;
    identity.module = "m29-synthetic";
    identity.build_id = "m29-build";
    identity.input_sha256 = "m29-sha";
    identity.translator_version = version;
    identity.guest_base = base;
    identity.executable_ranges.push_back({base, static_cast<memory::GuestSize>(code.size())});
    identity.entry_points.push_back({base, analysis::EntryPointKind::DynamicInit,
                                     "synthetic M29 entry", analysis::FunctionConfidence::High,
                                     false, "synthetic resumable execution"});
    const auto map = analysis::FunctionMapBuilder::build(
        analysis::ModuleAnalysisInput{std::move(identity), &fixture.memory, std::move(seeds)});
    if (!map) return Result<SessionFixture>::failure(map.error());
    fixture.map = std::move(map).value();
    return Result<SessionFixture>::success(std::move(fixture));
}

Result<execution::ExecutionSessionResult> run_session(
    SessionFixture& fixture, memory::GuestAddress entry,
    execution::ExecutionSessionOptions options = {})
{
    execution::ExecutionSession session(fixture.memory, fixture.map, {}, std::move(options));
    auto selected = execution::select_entry(fixture.map.identity(),
                                             execution::EntrySelectionKind::DynamicInit);
    if (!selected) return Result<execution::ExecutionSessionResult>::failure(selected.error());
    selected.value().address = entry;
    return session.run(selected.value());
}

analysis::FunctionSeed seed(memory::GuestAddress address)
{
    return analysis::FunctionSeed{address, analysis::FunctionDiscoverySource::AnalystSeed,
                                  analysis::FunctionConfidence::Manual, std::nullopt, std::nullopt,
                                  "synthetic M29 function"};
}

std::uint64_t read_word(const memory::GuestMemory& memory, memory::GuestAddress address)
{
    std::array<std::byte, 8> bytes{};
    REQUIRE(memory.read(address, bytes));
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < bytes.size(); ++index)
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned int>(bytes[index])) << (index * 8U);
    return value;
}

} // namespace

TEST_CASE("M29 tiny IR slices are semantically equivalent to a large slice")
{
    const auto function = make_equivalence_function();
    REQUIRE(ir::verify(function));
    memory::GuestMemory large_memory;
    memory::GuestMemory tiny_memory;
    REQUIRE(large_memory.map(0x3000U, 0x100U,
                             memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Write,
                             "m29.large.data", memory::GuestRegionKind::Data));
    REQUIRE(tiny_memory.map(0x3000U, 0x100U,
                            memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Write,
                            "m29.tiny.data", memory::GuestRegionKind::Data));
    runtime::CpuState large_cpu{};
    runtime::CpuState tiny_cpu{};
    const std::array<std::uint64_t, 1> observed{0x1000U};
    const auto large = run_sliced(function, large_memory, large_cpu, 4096U, std::nullopt, observed);
    const auto tiny = run_sliced(function, tiny_memory, tiny_cpu, 1U, std::nullopt, observed);
    require_same_run(large, large_cpu, tiny, tiny_cpu);
    REQUIRE(large.result.executed_blocks == 2U);
    REQUIRE(large.result.executed_guest_instructions == 2U);
    REQUIRE(read_word(large_memory, 0x3000U) == read_word(tiny_memory, 0x3000U));
    REQUIRE(large_cpu.x[0U] == 7U);
    REQUIRE(large_cpu.x[1U] == 8U);
}

TEST_CASE("M29 mid-instruction stores are not replayed across slices")
{
    const auto function = make_store_pair_function();
    REQUIRE(ir::verify(function));
    memory::GuestMemory reference_memory;
    memory::GuestMemory sliced_memory;
    REQUIRE(reference_memory.map(0x4000U, 0x40U,
                                 memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Write,
                                 "m29.reference.data", memory::GuestRegionKind::Data));
    REQUIRE(sliced_memory.map(0x4000U, 0x40U,
                              memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Write,
                              "m29.sliced.data", memory::GuestRegionKind::Data));
    runtime::CpuState reference_cpu{};
    runtime::CpuState sliced_cpu{};
    const auto reference = run_sliced(function, reference_memory, reference_cpu, 4096U);

    runtime::SharedRuntimeState shared(sliced_memory);
    runtime::RuntimeContext context{&sliced_memory};
    context.shared = &shared;
    interpreter::InterpreterFrame frame;
    runtime::ExecutionOptions options;
    options.slice_ir_operations = 4U;
    const std::array<std::uint64_t, 1> observed{0x2000U};
    options.observed_guest_pcs = observed;
    const auto first = interpreter::execute_until_boundary(
        function, sliced_cpu, context, frame, options);
    REQUIRE(first);
    REQUIRE(first.value().status == runtime::ExecutionStatus::Yielded);
    REQUIRE(first.value().executed_operations == 4U);
    REQUIRE(frame.ir_operation_index == 4U);
    REQUIRE(frame.pending_observation.has_value());
    REQUIRE(read_word(sliced_memory, 0x4000U) == 0x0102030405060708U);
    REQUIRE(read_word(sliced_memory, 0x4008U) == 0U);

    while (true)
    {
        const auto next = interpreter::execute_until_boundary(
            function, sliced_cpu, context, frame, options);
        REQUIRE(next);
        if (next.value().status != runtime::ExecutionStatus::Yielded) break;
    }
    REQUIRE(sliced_cpu.x == reference_cpu.x);
    REQUIRE(sliced_cpu.pc == reference_cpu.pc);
    REQUIRE(read_word(sliced_memory, 0x4000U) == read_word(reference_memory, 0x4000U));
    REQUIRE(read_word(sliced_memory, 0x4008U) == read_word(reference_memory, 0x4008U));
    REQUIRE(read_word(sliced_memory, 0x4000U) == 0x0102030405060708U);
    REQUIRE(read_word(sliced_memory, 0x4008U) == 0x1112131415161718U);
}

TEST_CASE("M29 pending instruction observations survive an internal yield")
{
    const auto function = make_observation_function();
    memory::GuestMemory memory;
    runtime::CpuState cpu{};
    const std::array<std::uint64_t, 1> observed{0x2100U};
    const auto run = run_sliced(function, memory, cpu, 1U, std::nullopt, observed);
    REQUIRE(run.result.status == runtime::ExecutionStatus::Returned);
    REQUIRE(run.result.observed_instruction_executions.size() == 1U);
    const auto& execution = run.result.observed_instruction_executions.front();
    REQUIRE(execution.guest_pc == 0x2100U);
    REQUIRE(execution.pre_state.x[0U] == 0U);
    REQUIRE(execution.post_state.x[0U] == 0x55U);
    REQUIRE(execution.next_guest_pc == std::optional<std::uint64_t>(0x2104U));
    REQUIRE(run.result.executed_guest_pcs == std::vector<std::uint64_t>{0x2100U, 0x2104U});
}

TEST_CASE("M29 block cursor and block counts are independent of slice size")
{
    const auto function = make_three_block_function();
    memory::GuestMemory large_memory;
    memory::GuestMemory tiny_memory;
    runtime::CpuState large_cpu{};
    runtime::CpuState tiny_cpu{};
    const auto large = run_sliced(function, large_memory, large_cpu, 4096U);
    const auto tiny = run_sliced(function, tiny_memory, tiny_cpu, 2U);
    require_same_run(large, large_cpu, tiny, tiny_cpu);
    REQUIRE(tiny.result.executed_blocks == 3U);
    REQUIRE(tiny.result.executed_guest_instructions == 3U);
    REQUIRE(tiny.result.executed_guest_pcs ==
            std::vector<std::uint64_t>{0x3000U, 0x3004U, 0x3008U});
}

TEST_CASE("M29 call and return accounting is unaffected by slices")
{
    constexpr memory::GuestAddress base = 0x5000U;
    const auto code = words({movz(0U, 1U), bl(base + 4U, base + 0x10U),
                             0x91000400U, 0xd65f03c0U,
                             0x91000800U, 0xd65f03c0U});
    auto large_fixture = make_session_fixture(base, code, {seed(base), seed(base + 0x10U)});
    auto tiny_fixture = make_session_fixture(base, code, {seed(base), seed(base + 0x10U)});
    REQUIRE(large_fixture);
    REQUIRE(tiny_fixture);
    execution::ExecutionSessionOptions large_options;
    large_options.budgets.slice_ir_operations = 4096U;
    execution::ExecutionSessionOptions tiny_options;
    tiny_options.budgets.slice_ir_operations = 1U;
    const auto large = run_session(large_fixture.value(), base, large_options);
    const auto tiny = run_session(tiny_fixture.value(), base, tiny_options);
    REQUIRE(large);
    REQUIRE(tiny);
    REQUIRE(large.value().stop_reason == execution::ExecutionStopReason::EntryReturned);
    REQUIRE(tiny.value().stop_reason == large.value().stop_reason);
    REQUIRE(tiny.value().final_cpu.x == large.value().final_cpu.x);
    REQUIRE(tiny.value().direct_calls == large.value().direct_calls);
    REQUIRE(tiny.value().indirect_calls == large.value().indirect_calls);
    REQUIRE(tiny.value().returns == large.value().returns);
    REQUIRE(tiny.value().maximum_call_depth == large.value().maximum_call_depth);
    REQUIRE(tiny.value().executed_functions == large.value().executed_functions);
    REQUIRE(tiny.value().function_transfers == large.value().function_transfers);
    REQUIRE(tiny.value().guest_instruction_count == large.value().guest_instruction_count);
    REQUIRE(tiny.value().guest_blocks == large.value().guest_blocks);
    REQUIRE(tiny.value().execution_slices > large.value().execution_slices);
    REQUIRE(tiny.value().resumable_yields > 0U);
}

TEST_CASE("M29 explicit IR hard limits remain exact and typed")
{
    const auto function = make_store_pair_function();
    memory::GuestMemory memory;
    REQUIRE(memory.map(0x4000U, 0x40U,
                       memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Write,
                       "m29.hard.data", memory::GuestRegionKind::Data));
    runtime::CpuState cpu{};
    const auto run = run_sliced(function, memory, cpu, 1U, 4U);
    REQUIRE(run.result.status == runtime::ExecutionStatus::LimitExceeded);
    REQUIRE(run.result.boundary.kind == runtime::ExecutionBoundaryKind::BudgetExhaustion);
    REQUIRE(run.result.executed_operations == 4U);
    REQUIRE(run.frame.ir_operation_index == 4U);
    REQUIRE(read_word(memory, 0x4000U) == 0x0102030405060708U);
    REQUIRE(read_word(memory, 0x4008U) == 0U);

    memory::GuestMemory exact_memory;
    REQUIRE(exact_memory.map(0x4000U, 0x40U,
                             memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Write,
                             "m29.exact.data", memory::GuestRegionKind::Data));
    runtime::CpuState exact_cpu{};
    const auto exact = run_sliced(function, exact_memory, exact_cpu, 1U, 7U);
    REQUIRE(exact.result.status == runtime::ExecutionStatus::Returned);
    REQUIRE(exact.result.executed_operations == 7U);
    REQUIRE(read_word(exact_memory, 0x4008U) == 0x1112131415161718U);
}

TEST_CASE("M29 legacy execute wrapper carries an explicit hard limit across slices")
{
    const auto function = make_store_pair_function();
    memory::GuestMemory memory;
    REQUIRE(memory.map(0x4000U, 0x40U,
                       memory::GuestMemoryPermissions::Read | memory::GuestMemoryPermissions::Write,
                       "m29.legacy-hard.data", memory::GuestRegionKind::Data));
    runtime::CpuState cpu{};
    runtime::SharedRuntimeState shared(memory);
    runtime::RuntimeContext context{&memory};
    context.shared = &shared;
    runtime::ExecutionOptions options;
    options.max_ir_operations = 4U;
    options.slice_ir_operations = 1U;
    const auto result = interpreter::execute(function, cpu, context, options);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == ErrorCode::ExecutionLimitExceeded);
    REQUIRE(read_word(memory, 0x4000U) == 0x0102030405060708U);
    REQUIRE(read_word(memory, 0x4008U) == 0U);
}

TEST_CASE("M29 ordinary loop execution terminates through guest-block accounting")
{
    constexpr memory::GuestAddress base = 0x6000U;
    const auto code = words({branch(base, base)});
    auto fixture = make_session_fixture(base, code, {seed(base)});
    REQUIRE(fixture);
    execution::ExecutionSessionOptions options;
    options.budgets.max_guest_blocks = 3U;
    options.budgets.slice_ir_operations = 1U;
    const auto result = run_session(fixture.value(), base, options);
    REQUIRE(result);
    REQUIRE(result.value().stop_reason == execution::ExecutionStopReason::GuestBlockLimitExceeded);
    REQUIRE(result.value().guest_blocks > options.budgets.max_guest_blocks);
    REQUIRE(result.value().ir_operations == result.value().guest_blocks);
    REQUIRE(result.value().execution_slices == result.value().guest_blocks);
}

TEST_CASE("M29 every yielded self-loop makes checked progress")
{
    ir::Function function("m29-progress", 0x7000U);
    const auto block = function.add_block(0x7000U, "loop");
    function.set_entry_block(block);
    ir::Builder builder(function);
    REQUIRE(builder.set_insert_block(block));
    set_pc(builder, 0x7000U);
    ir::Terminator branch_terminator;
    branch_terminator.kind = ir::TerminatorKind::Branch;
    branch_terminator.target = block;
    branch_terminator.source.guest_pc = 0x7000U;
    REQUIRE(builder.set_terminator(std::move(branch_terminator)));

    memory::GuestMemory memory;
    runtime::CpuState cpu{};
    runtime::SharedRuntimeState shared(memory);
    runtime::RuntimeContext context{&memory};
    context.shared = &shared;
    interpreter::InterpreterFrame frame;
    runtime::ExecutionOptions options;
    options.slice_ir_operations = 1U;
    std::size_t prior_serial = 0U;
    for (std::size_t iteration = 0U; iteration < 8U; ++iteration)
    {
        const auto result = interpreter::execute_until_boundary(
            function, cpu, context, frame, options);
        REQUIRE(result);
        REQUIRE(result.value().status == runtime::ExecutionStatus::Yielded);
        REQUIRE(result.value().executed_operations == 1U);
        REQUIRE(frame.ir_operation_index == 1U);
        REQUIRE(frame.block_entry_serial > prior_serial);
        prior_serial = frame.block_entry_serial;
    }
}

TEST_CASE("M29 rejects zero quanta and overflowing resume cursors")
{
    const auto function = make_three_block_function();
    memory::GuestMemory memory;
    runtime::CpuState cpu{};
    runtime::RuntimeContext context{&memory};
    interpreter::InterpreterFrame zero_frame;
    runtime::ExecutionOptions zero_options;
    zero_options.slice_ir_operations = 0U;
    const auto zero = interpreter::execute_until_boundary(
        function, cpu, context, zero_frame, zero_options);
    REQUIRE_FALSE(zero);
    REQUIRE(zero.error().code == ErrorCode::InvalidArgument);

    interpreter::InterpreterFrame overflowing_frame;
    overflowing_frame.reset(function);
    overflowing_frame.block_entry_serial = std::numeric_limits<std::size_t>::max();
    runtime::ExecutionOptions options;
    options.slice_ir_operations = 1U;
    const auto overflow = interpreter::execute_until_boundary(
        function, cpu, context, overflowing_frame, options);
    REQUIRE_FALSE(overflow);
    REQUIRE(overflow.error().code == ErrorCode::ArithmeticOverflow);
}
