#include "switchrecomp/interpreter/interpreter.hpp"

#include "switchrecomp/common/checked_arithmetic.hpp"
#include "switchrecomp/ir/verifier.hpp"

#include <cstdint>
#include <limits>
#include <vector>

namespace switchrecomp::interpreter
{
namespace
{
[[nodiscard]] bool is_m9_opcode(ir::Opcode opcode) noexcept
{
    switch (opcode)
    {
    case ir::Opcode::AtomicLoad: case ir::Opcode::AtomicStore:
    case ir::Opcode::ExclusiveLoad: case ir::Opcode::ExclusiveStore:
    case ir::Opcode::ClearExclusive: case ir::Opcode::MemoryBarrier:
    case ir::Opcode::ReadSystemRegister: case ir::Opcode::WriteSystemRegister:
        return true;
    default: return false;
    }
}

[[nodiscard]] bool contains_m9(const ir::Function& function) noexcept
{
    for (const auto& block : function.blocks())
        for (const auto& instruction : block.instructions)
            if (is_m9_opcode(instruction.opcode)) return true;
    return false;
}

[[nodiscard]] std::uint64_t mask_for(ir::Type type) noexcept
{
    if (type.bit_width() >= 64U) return std::numeric_limits<std::uint64_t>::max();
    return type.bit_width() == 0U ? 0U : (std::uint64_t{1} << type.bit_width()) - 1U;
}

[[nodiscard]] std::uint64_t width_value(std::uint64_t value, ir::Type type) noexcept
{
    return type.is_vector() ? value : value & mask_for(type);
}

[[nodiscard]] bool sign_bit(std::uint64_t value, ir::Type type) noexcept
{
    return type.bit_width() != 0U &&
           (value & (std::uint64_t{1} << (type.bit_width() - 1U))) != 0U;
}

[[nodiscard]] Result<runtime::ExecutionResult> runtime_failure(const runtime::RuntimeContext& runtime)
{
    return Result<runtime::ExecutionResult>::failure(runtime.has_error
        ? runtime.last_error
        : make_error(ErrorCode::InterpreterError, "runtime helper failed without a diagnostic"));
}
} // namespace

Result<runtime::ExecutionResult> execute(const ir::Function& function, runtime::CpuState& cpu,
                                         runtime::RuntimeContext& runtime,
                                         const runtime::ExecutionOptions& options)
{
    if (!contains_m9(function))
        return execute_legacy(function, cpu, runtime, options);

    const auto verified = ir::verify(function);
    if (!verified) return Result<runtime::ExecutionResult>::failure(verified.error());

    runtime.clear_error();
    runtime.cpu = &cpu;
    std::vector<std::uint64_t> values(function.values().size(), 0U);
    std::vector<std::uint64_t> high_values(function.values().size(), 0U);
    auto current = function.entry_block();
    runtime::ExecutionResult result;

    const auto read_value = [&](ir::ValueId id) -> Result<std::uint64_t> {
        if (function.value(id) == nullptr || id >= values.size())
            return Result<std::uint64_t>::failure(
                make_error(ErrorCode::InvalidIrValue, "interpreter encountered an invalid value id"));
        return Result<std::uint64_t>::success(values[id]);
    };

    while (true)
    {
        const auto* block = function.block(current);
        if (block == nullptr)
            return Result<runtime::ExecutionResult>::failure(
                make_error(ErrorCode::InvalidIrBlock, "interpreter reached a missing block"));
        ++result.executed_blocks;

        for (const auto& instruction : block->instructions)
        {
            if (result.executed_operations >= options.max_ir_operations)
                return Result<runtime::ExecutionResult>::failure(make_error(
                    ErrorCode::ExecutionLimitExceeded, "interpreter exceeded the configured IR operation limit"));
            ++result.executed_operations;

            const auto read = [&](std::size_t index) -> Result<std::uint64_t> {
                if (index >= instruction.operands.size())
                    return Result<std::uint64_t>::failure(
                        make_error(ErrorCode::InvalidIrOperand, "instruction is missing an operand"));
                return read_value(instruction.operands[index]);
            };
            const auto store = [&](std::uint64_t value, std::uint64_t high = 0U) -> Result<void> {
                if (instruction.result == ir::invalid_value || instruction.result >= values.size())
                    return Result<void>::failure(
                        make_error(ErrorCode::InvalidIrValue, "instruction result is invalid"));
                values[instruction.result] = width_value(value, instruction.result_type);
                high_values[instruction.result] = high;
                return Result<void>::success();
            };

            switch (instruction.opcode)
            {
            case ir::Opcode::Constant:
                if (const auto done = store(instruction.constant, instruction.constant_high); !done)
                    return Result<runtime::ExecutionResult>::failure(done.error());
                break;
            case ir::Opcode::Nop: break;
            case ir::Opcode::SetPc: cpu.pc = instruction.source.guest_pc; break;
            case ir::Opcode::ReadRegister:
                if (const auto done = store(runtime::read_register(cpu, instruction.reg)); !done)
                    return Result<runtime::ExecutionResult>::failure(done.error());
                break;
            case ir::Opcode::WriteRegister:
            {
                const auto value = read(0U); if (!value) return Result<runtime::ExecutionResult>::failure(value.error());
                runtime::write_register(cpu, instruction.reg, value.value()); break;
            }
            case ir::Opcode::ReadFlag:
                if (const auto done = store(runtime::read_flag(cpu, instruction.flag) ? 1U : 0U); !done)
                    return Result<runtime::ExecutionResult>::failure(done.error());
                break;
            case ir::Opcode::WriteFlag:
            {
                const auto value = read(0U); if (!value) return Result<runtime::ExecutionResult>::failure(value.error());
                runtime::write_flag(cpu, instruction.flag, value.value() != 0U); break;
            }
            case ir::Opcode::ReadVectorRegister:
            {
                const auto vector = runtime::read_vector_register(cpu, instruction.vector_index);
                if (const auto done = store(vector.lo, vector.hi); !done)
                    return Result<runtime::ExecutionResult>::failure(done.error());
                break;
            }
            case ir::Opcode::WriteVectorRegister:
            {
                const auto value = read(0U); if (!value) return Result<runtime::ExecutionResult>::failure(value.error());
                runtime::write_vector_register(cpu, instruction.vector_index,
                    runtime::Vector128{value.value(), high_values[instruction.operands[0]]});
                break;
            }
            case ir::Opcode::ReadFpControl:
                if (const auto done = store(cpu.fpcr); !done) return Result<runtime::ExecutionResult>::failure(done.error());
                break;
            case ir::Opcode::WriteFpControl:
            {
                const auto value = read(0U); if (!value) return Result<runtime::ExecutionResult>::failure(value.error());
                cpu.fpcr = static_cast<std::uint32_t>(value.value()); break;
            }
            case ir::Opcode::ReadFpStatus:
                if (const auto done = store(cpu.fpsr); !done) return Result<runtime::ExecutionResult>::failure(done.error());
                break;
            case ir::Opcode::WriteFpStatus:
            {
                const auto value = read(0U); if (!value) return Result<runtime::ExecutionResult>::failure(value.error());
                cpu.fpsr = static_cast<std::uint32_t>(value.value()); break;
            }
            case ir::Opcode::Add: case ir::Opcode::Sub: case ir::Opcode::Mul:
            case ir::Opcode::And: case ir::Opcode::Or: case ir::Opcode::Xor:
            {
                const auto left = read(0U), right = read(1U);
                if (!left || !right) return Result<runtime::ExecutionResult>::failure(!left ? left.error() : right.error());
                const auto value = instruction.opcode == ir::Opcode::Add ? left.value() + right.value()
                    : instruction.opcode == ir::Opcode::Sub ? left.value() - right.value()
                    : instruction.opcode == ir::Opcode::Mul ? left.value() * right.value()
                    : instruction.opcode == ir::Opcode::And ? left.value() & right.value()
                    : instruction.opcode == ir::Opcode::Or ? left.value() | right.value()
                    : left.value() ^ right.value();
                if (const auto done = store(value); !done) return Result<runtime::ExecutionResult>::failure(done.error());
                break;
            }
            case ir::Opcode::Not:
            {
                const auto value = read(0U); if (!value) return Result<runtime::ExecutionResult>::failure(value.error());
                if (const auto done = store(~value.value()); !done) return Result<runtime::ExecutionResult>::failure(done.error());
                break;
            }
            case ir::Opcode::ShiftLeft: case ir::Opcode::LogicalShiftRight:
            case ir::Opcode::ArithmeticShiftRight: case ir::Opcode::RotateRight:
            {
                const auto value = read(0U), amount = read(1U);
                if (!value || !amount) return Result<runtime::ExecutionResult>::failure(!value ? value.error() : amount.error());
                const auto bits = instruction.result_type.bit_width();
                if (amount.value() >= bits) return Result<runtime::ExecutionResult>::failure(
                    make_error(ErrorCode::InterpreterError, "IR shift amount is outside operand width"));
                const auto shift = static_cast<unsigned int>(amount.value());
                std::uint64_t shifted = value.value();
                if (instruction.opcode == ir::Opcode::ShiftLeft) shifted <<= shift;
                else if (instruction.opcode == ir::Opcode::LogicalShiftRight) shifted >>= shift;
                else if (instruction.opcode == ir::Opcode::ArithmeticShiftRight)
                {
                    shifted >>= shift;
                    if (shift != 0U && sign_bit(value.value(), instruction.result_type))
                        shifted |= std::numeric_limits<std::uint64_t>::max() << (bits - shift);
                }
                else if (shift != 0U) shifted = (value.value() >> shift) | (value.value() << (bits - shift));
                if (const auto done = store(shifted); !done) return Result<runtime::ExecutionResult>::failure(done.error());
                break;
            }
            case ir::Opcode::Truncate: case ir::Opcode::ZeroExtend: case ir::Opcode::SignExtend:
            {
                const auto value = read(0U); if (!value) return Result<runtime::ExecutionResult>::failure(value.error());
                auto converted = value.value();
                const auto* source = function.value(instruction.operands[0]);
                if (instruction.opcode == ir::Opcode::SignExtend && source != nullptr && sign_bit(converted, source->type))
                    converted |= ~mask_for(source->type);
                if (const auto done = store(converted); !done) return Result<runtime::ExecutionResult>::failure(done.error());
                break;
            }
            case ir::Opcode::CompareEqual: case ir::Opcode::CompareNotEqual:
            case ir::Opcode::CompareUnsigned: case ir::Opcode::CompareSigned:
            {
                const auto left = read(0U), right = read(1U);
                if (!left || !right) return Result<runtime::ExecutionResult>::failure(!left ? left.error() : right.error());
                bool value = false;
                if (instruction.opcode == ir::Opcode::CompareEqual) value = left.value() == right.value();
                else if (instruction.opcode == ir::Opcode::CompareNotEqual) value = left.value() != right.value();
                else if (instruction.opcode == ir::Opcode::CompareUnsigned) value = left.value() < right.value();
                else
                {
                    const auto type = function.value(instruction.operands[0])->type;
                    const bool ls = sign_bit(left.value(), type), rs = sign_bit(right.value(), type);
                    value = ls != rs ? ls : left.value() < right.value();
                }
                if (const auto done = store(value ? 1U : 0U); !done) return Result<runtime::ExecutionResult>::failure(done.error());
                break;
            }
            case ir::Opcode::Select:
            {
                const auto condition = read(0U), yes = read(1U), no = read(2U);
                if (!condition || !yes || !no) return Result<runtime::ExecutionResult>::failure(!condition ? condition.error() : !yes ? yes.error() : no.error());
                if (const auto done = store(condition.value() != 0U ? yes.value() : no.value()); !done)
                    return Result<runtime::ExecutionResult>::failure(done.error());
                break;
            }
            case ir::Opcode::EvaluateCondition:
            {
                const auto n = read(0U), z = read(1U), c = read(2U), v = read(3U);
                if (!n || !z || !c || !v) return Result<runtime::ExecutionResult>::failure(!n ? n.error() : !z ? z.error() : !c ? c.error() : v.error());
                const bool value = runtime::evaluate_condition(instruction.condition,
                    n.value() != 0U, z.value() != 0U, c.value() != 0U, v.value() != 0U);
                if (const auto done = store(value ? 1U : 0U); !done) return Result<runtime::ExecutionResult>::failure(done.error());
                break;
            }
            case ir::Opcode::AddCarry: case ir::Opcode::SubCarry:
            case ir::Opcode::AddOverflow: case ir::Opcode::SubOverflow:
            {
                const auto left = read(0U), right = read(1U);
                if (!left || !right) return Result<runtime::ExecutionResult>::failure(!left ? left.error() : right.error());
                const auto type = function.value(instruction.operands[0])->type;
                const auto sum = width_value(left.value() + right.value(), type);
                const auto diff = width_value(left.value() - right.value(), type);
                bool value = false;
                if (instruction.opcode == ir::Opcode::AddCarry) value = sum < width_value(left.value(), type);
                else if (instruction.opcode == ir::Opcode::SubCarry) value = width_value(left.value(), type) >= width_value(right.value(), type);
                else if (instruction.opcode == ir::Opcode::AddOverflow)
                    value = ((~(left.value() ^ right.value()) & (left.value() ^ sum)) &
                             (std::uint64_t{1} << (type.bit_width() - 1U))) != 0U;
                else value = (((left.value() ^ right.value()) & (left.value() ^ diff)) &
                              (std::uint64_t{1} << (type.bit_width() - 1U))) != 0U;
                if (const auto done = store(value ? 1U : 0U); !done) return Result<runtime::ExecutionResult>::failure(done.error());
                break;
            }
            case ir::Opcode::GuestAddressAdd:
            {
                const auto base = read(0U); if (!base) return Result<runtime::ExecutionResult>::failure(base.error());
                const auto sum = checked_add_signed_u64(base.value(), instruction.immediate);
                if (!sum) return Result<runtime::ExecutionResult>::failure(sum.error());
                if (const auto done = store(sum.value()); !done) return Result<runtime::ExecutionResult>::failure(done.error());
                break;
            }
            case ir::Opcode::GuestAddressAddValue:
            {
                const auto base = read(0U), offset = read(1U);
                if (!base || !offset) return Result<runtime::ExecutionResult>::failure(!base ? base.error() : offset.error());
                const auto sum = instruction.address_offset_signed
                    ? checked_add_signed_u64(base.value(), signed_value_from_u64(offset.value()))
                    : checked_add_u64(base.value(), offset.value());
                if (!sum) return Result<runtime::ExecutionResult>::failure(sum.error());
                if (const auto done = store(sum.value()); !done) return Result<runtime::ExecutionResult>::failure(done.error());
                break;
            }
            case ir::Opcode::GuestLoad: case ir::Opcode::AtomicLoad: case ir::Opcode::ExclusiveLoad:
            {
                const auto address = read(0U); if (!address) return Result<runtime::ExecutionResult>::failure(address.error());
                std::uint64_t loaded = 0U;
                const auto rc = instruction.opcode == ir::Opcode::GuestLoad
                    ? runtime::switchrecomp_runtime_guest_load(&runtime, address.value(), instruction.memory_size, &loaded)
                    : instruction.opcode == ir::Opcode::AtomicLoad
                        ? runtime::switchrecomp_runtime_atomic_load(&runtime, address.value(), instruction.memory_size,
                            static_cast<std::uint8_t>(instruction.memory_order), &loaded)
                        : runtime::switchrecomp_runtime_exclusive_load(&runtime, address.value(), instruction.memory_size,
                            static_cast<std::uint8_t>(instruction.memory_order), &loaded);
                if (rc != 0U) return runtime_failure(runtime);
                if (const auto done = store(loaded); !done) return Result<runtime::ExecutionResult>::failure(done.error());
                break;
            }
            case ir::Opcode::GuestStore: case ir::Opcode::AtomicStore:
            {
                const auto address = read(0U), value = read(1U);
                if (!address || !value) return Result<runtime::ExecutionResult>::failure(!address ? address.error() : value.error());
                const auto rc = instruction.opcode == ir::Opcode::GuestStore
                    ? runtime::switchrecomp_runtime_guest_store(&runtime, address.value(), instruction.memory_size, value.value())
                    : runtime::switchrecomp_runtime_atomic_store(&runtime, address.value(), instruction.memory_size,
                        value.value(), static_cast<std::uint8_t>(instruction.memory_order));
                if (rc != 0U) return runtime_failure(runtime);
                break;
            }
            case ir::Opcode::ExclusiveStore:
            {
                const auto address = read(0U), value = read(1U);
                if (!address || !value) return Result<runtime::ExecutionResult>::failure(!address ? address.error() : value.error());
                std::uint32_t status = 1U;
                if (runtime::switchrecomp_runtime_exclusive_store(&runtime, address.value(), instruction.memory_size,
                    value.value(), static_cast<std::uint8_t>(instruction.memory_order), &status) != 0U)
                    return runtime_failure(runtime);
                if (const auto done = store(status); !done) return Result<runtime::ExecutionResult>::failure(done.error());
                break;
            }
            case ir::Opcode::ClearExclusive:
                if (runtime::switchrecomp_runtime_clear_exclusive(&runtime) != 0U) return runtime_failure(runtime);
                break;
            case ir::Opcode::MemoryBarrier:
                if (runtime::switchrecomp_runtime_memory_barrier(&runtime,
                    static_cast<std::uint8_t>(instruction.barrier_kind),
                    static_cast<std::uint8_t>(instruction.barrier_option)) != 0U) return runtime_failure(runtime);
                break;
            case ir::Opcode::ReadSystemRegister:
            {
                std::uint64_t value = 0U;
                if (runtime::switchrecomp_runtime_read_system_register(&runtime,
                    static_cast<std::uint8_t>(instruction.system_register), &value) != 0U) return runtime_failure(runtime);
                if (const auto done = store(value); !done) return Result<runtime::ExecutionResult>::failure(done.error());
                break;
            }
            case ir::Opcode::WriteSystemRegister:
            {
                const auto value = read(0U); if (!value) return Result<runtime::ExecutionResult>::failure(value.error());
                if (runtime::switchrecomp_runtime_write_system_register(&runtime,
                    static_cast<std::uint8_t>(instruction.system_register), value.value()) != 0U) return runtime_failure(runtime);
                break;
            }
            case ir::Opcode::BitCast:
            {
                const auto value = read(0U); if (!value) return Result<runtime::ExecutionResult>::failure(value.error());
                const auto high = high_values[instruction.operands[0]];
                if (const auto done = store(value.value(), instruction.result_type.is_vector() ? high : 0U); !done)
                    return Result<runtime::ExecutionResult>::failure(done.error());
                break;
            }
            case ir::Opcode::GuestLoadVector:
            {
                const auto address = read(0U); if (!address) return Result<runtime::ExecutionResult>::failure(address.error());
                runtime::Vector128 vector{};
                if (runtime::switchrecomp_runtime_guest_load_vector(&runtime, address.value(), &vector) != 0U)
                    return runtime_failure(runtime);
                if (const auto done = store(vector.lo, vector.hi); !done) return Result<runtime::ExecutionResult>::failure(done.error());
                break;
            }
            case ir::Opcode::GuestStoreVector:
            {
                const auto address = read(0U), low = read(1U);
                if (!address || !low) return Result<runtime::ExecutionResult>::failure(!address ? address.error() : low.error());
                const runtime::Vector128 vector{low.value(), high_values[instruction.operands[1]]};
                if (runtime::switchrecomp_runtime_guest_store_vector(&runtime, address.value(), &vector) != 0U)
                    return runtime_failure(runtime);
                break;
            }
            default:
                return Result<runtime::ExecutionResult>::failure(make_error(
                    ErrorCode::UnsupportedInstruction,
                    "Milestone 9 reference interpreter encountered a legacy FP/SIMD operation mixed with concurrency IR"));
            }
        }

        if (!block->has_terminator)
            return Result<runtime::ExecutionResult>::failure(
                make_error(ErrorCode::InvalidIrBlock, "interpreter reached an unterminated block"));
        const auto& terminator = block->terminator;
        switch (terminator.kind)
        {
        case ir::TerminatorKind::Branch: current = terminator.target; break;
        case ir::TerminatorKind::ConditionalBranch:
        {
            const auto condition = read_value(terminator.condition);
            if (!condition) return Result<runtime::ExecutionResult>::failure(condition.error());
            current = condition.value() != 0U ? terminator.target : terminator.false_target; break;
        }
        case ir::TerminatorKind::Return:
            if (terminator.target_value != ir::invalid_value)
            {
                const auto target = read_value(terminator.target_value);
                if (!target) return Result<runtime::ExecutionResult>::failure(target.error());
                if (target.value() != 0U) cpu.pc = target.value();
            }
            result.final_guest_pc = cpu.pc;
            return Result<runtime::ExecutionResult>::success(result);
        case ir::TerminatorKind::DirectCall:
        case ir::TerminatorKind::IndirectBranch:
        case ir::TerminatorKind::IndirectCall:
        {
            const auto target = read_value(terminator.target_value);
            if (!target) return Result<runtime::ExecutionResult>::failure(target.error());
            cpu.pc = target.value(); result.final_guest_pc = cpu.pc;
            return Result<runtime::ExecutionResult>::success(result);
        }
        case ir::TerminatorKind::Trap:
            (void)runtime::switchrecomp_runtime_trap(&runtime, terminator.trap_reason.c_str());
            return runtime_failure(runtime);
        }
    }
}

} // namespace switchrecomp::interpreter
