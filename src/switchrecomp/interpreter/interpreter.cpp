#include "switchrecomp/interpreter/interpreter.hpp"

#include "switchrecomp/common/checked_arithmetic.hpp"
#include "switchrecomp/common/portable_arithmetic.hpp"
#include "switchrecomp/ir/verifier.hpp"
#include "switchrecomp/runtime/fp.hpp"

#include <cstdint>
#include <algorithm>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace switchrecomp::interpreter
{

void InterpreterFrame::reset(const ir::Function& function_value)
{
    function = &function_value;
    current_block = function_value.entry_block();
    values.assign(function_value.values().size(), 0U);
    high_values.assign(function_value.values().size(), 0U);
    provenance.assign(function_value.values().size(), InterpreterValueProvenance{});
    register_provenance.fill(InterpreterValueProvenance{});
}

namespace
{

[[nodiscard]] bool add_signed_without_overflow(std::int64_t left, std::int64_t right,
                                                std::int64_t& result) noexcept
{
    if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right))
    {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] std::uint64_t mask_for(ir::Type type) noexcept
{
    if (type.bit_width() >= 64U) return std::numeric_limits<std::uint64_t>::max();
    return type.bit_width() == 64U ? std::numeric_limits<std::uint64_t>::max()
                                   : (std::uint64_t{1} << type.bit_width()) - 1U;
}

[[nodiscard]] std::uint64_t value_for_width(std::uint64_t value, ir::Type type) noexcept
{
    if (type.is_vector()) return value;
    return value & mask_for(type);
}

[[nodiscard]] bool signed_bit(std::uint64_t value, ir::Type type) noexcept
{
    return (value & (std::uint64_t{1} << (type.bit_width() - 1U))) != 0U;
}

[[nodiscard]] bool add_overflow(std::uint64_t left, std::uint64_t right, ir::Type type) noexcept
{
    const auto result = value_for_width(left + right, type);
    return ((~(left ^ right) & (left ^ result)) &
            (std::uint64_t{1} << (type.bit_width() - 1U))) != 0U;
}

[[nodiscard]] bool sub_overflow(std::uint64_t left, std::uint64_t right, ir::Type type) noexcept
{
    const auto result = value_for_width(left - right, type);
    return (((left ^ right) & (left ^ result)) &
            (std::uint64_t{1} << (type.bit_width() - 1U))) != 0U;
}

[[nodiscard]] Result<std::uint64_t> get_value(const ir::Function& function,
                                              const std::vector<std::uint64_t>& values,
                                              ir::ValueId id)
{
    if (function.value(id) == nullptr || id >= values.size())
    {
        return Result<std::uint64_t>::failure(
            make_error(ErrorCode::InvalidIrValue, "interpreter encountered an invalid value id"));
    }
    return Result<std::uint64_t>::success(values[id]);
}

[[nodiscard]] Result<std::uint64_t> get_high_value(const ir::Function& function,
                                                   const std::vector<std::uint64_t>& values,
                                                   const std::vector<std::uint64_t>& high_values,
                                                   ir::ValueId id)
{
    if (function.value(id) == nullptr || id >= values.size() || id >= high_values.size())
        return Result<std::uint64_t>::failure(
            make_error(ErrorCode::InvalidIrValue, "interpreter encountered an invalid vector value id"));
    return Result<std::uint64_t>::success(high_values[id]);
}

[[nodiscard]] Result<runtime::ExecutionResult> runtime_failure(const runtime::RuntimeContext& runtime)
{
    if (runtime.has_error)
    {
        return Result<runtime::ExecutionResult>::failure(runtime.last_error);
    }
    return Result<runtime::ExecutionResult>::failure(
        make_error(ErrorCode::InterpreterError, "runtime helper failed without a diagnostic"));
}

} // namespace

Result<runtime::ExecutionResult> execute_until_boundary(
    const ir::Function& function, runtime::CpuState& cpu, runtime::RuntimeContext& runtime,
    InterpreterFrame& frame, const runtime::ExecutionOptions& options)
{
    const auto verified = ir::verify(function);
    if (!verified)
    {
        return Result<runtime::ExecutionResult>::failure(verified.error());
    }
    runtime.clear_error();
    if (frame.function != &function || frame.values.size() != function.values().size())
    {
        frame.reset(function);
    }
    runtime::ExecutionResult result;
    std::optional<runtime::ObservedInstructionExecution> pending_observation;
    const auto flush_observation = [&](std::optional<std::uint64_t> next_guest_pc) {
        if (!pending_observation) return;
        pending_observation->post_state = cpu;
        pending_observation->next_guest_pc = next_guest_pc;
        result.observed_instruction_executions.push_back(std::move(pending_observation.value()));
        pending_observation.reset();
    };

    while (true)
    {
        const auto* block = function.block(frame.current_block);
        if (block == nullptr)
        {
            return Result<runtime::ExecutionResult>::failure(
                make_error(ErrorCode::InvalidIrBlock, "interpreter reached a missing block"));
        }
        ++result.executed_blocks;
        for (const auto& instruction : block->instructions)
        {
            if (result.executed_operations >= options.max_ir_operations)
            {
                result.status = runtime::ExecutionStatus::LimitExceeded;
                result.boundary = runtime::ExecutionBoundary{
                    runtime::ExecutionBoundaryKind::BudgetExhaustion, cpu.pc, 0U, false,
                    ir::invalid_block, 0U, function.guest_entry(), false, 0U, {}, {}};
                result.final_guest_pc = cpu.pc;
                return Result<runtime::ExecutionResult>::success(std::move(result));
            }
            ++result.executed_operations;
            const bool observe_instruction = instruction.opcode == ir::Opcode::SetPc &&
                std::find(options.observed_guest_pcs.begin(), options.observed_guest_pcs.end(),
                          instruction.source.guest_pc) != options.observed_guest_pcs.end();
            if (instruction.opcode == ir::Opcode::SetPc)
            {
                flush_observation(instruction.source.guest_pc);
                ++result.executed_guest_instructions;
                result.executed_guest_pcs.push_back(instruction.source.guest_pc);
            }
            if (observe_instruction &&
                result.observed_guest_pcs.size() < options.max_observed_guest_pcs &&
                std::find(result.observed_guest_pcs.begin(), result.observed_guest_pcs.end(),
                          instruction.source.guest_pc) == result.observed_guest_pcs.end())
            {
                result.observed_guest_pcs.push_back(instruction.source.guest_pc);
            }
            const auto read = [&](ir::ValueId id) { return get_value(function, frame.values, id); };
            const auto store_result = [&](std::uint64_t value, std::uint64_t high = 0U,
                                          InterpreterValueProvenance value_provenance = {}) -> Result<void> {
                if (instruction.result == ir::invalid_value || instruction.result >= frame.values.size())
                {
                    return Result<void>::failure(
                        make_error(ErrorCode::InvalidIrValue, "instruction result is invalid"));
                }
                frame.values[instruction.result] = value_for_width(value, instruction.result_type);
                frame.high_values[instruction.result] = high;
                frame.provenance[instruction.result] = value_provenance;
                return Result<void>::success();
            };

            switch (instruction.opcode)
            {
            case ir::Opcode::Constant:
                if (const auto stored = store_result(
                        instruction.constant, instruction.constant_high,
                        InterpreterValueProvenance{InterpreterValueProvenance::Kind::Constant,
                                                   instruction.constant, 0}); !stored)
                {
                    return Result<runtime::ExecutionResult>::failure(stored.error());
                }
                break;
            case ir::Opcode::Nop:
                break;
            case ir::Opcode::SetPc:
                cpu.pc = instruction.source.guest_pc;
                if (observe_instruction &&
                    std::find_if(result.observed_instruction_executions.begin(),
                                 result.observed_instruction_executions.end(),
                                 [&](const auto& observed) {
                                     return observed.guest_pc == instruction.source.guest_pc;
                                 }) == result.observed_instruction_executions.end())
                {
                    pending_observation = runtime::ObservedInstructionExecution{};
                    pending_observation->guest_pc = instruction.source.guest_pc;
                    pending_observation->pre_state = cpu;
                }
                break;
            case ir::Opcode::ReadRegister:
            {
                InterpreterValueProvenance value_provenance;
                if (!instruction.reg.is_zero && !instruction.reg.is_stack_pointer &&
                    instruction.reg.index < frame.register_provenance.size())
                {
                    value_provenance = frame.register_provenance[instruction.reg.index];
                }
                if (const auto stored = store_result(runtime::read_register(cpu, instruction.reg), 0U,
                                                     value_provenance); !stored)
                {
                    return Result<runtime::ExecutionResult>::failure(stored.error());
                }
                break;
            }
            case ir::Opcode::WriteRegister:
            {
                const auto operand = read(instruction.operands[0]);
                if (!operand)
                {
                    return Result<runtime::ExecutionResult>::failure(operand.error());
                }
                runtime::write_register(cpu, instruction.reg, operand.value());
                if (!instruction.reg.is_zero && !instruction.reg.is_stack_pointer &&
                    instruction.reg.index < frame.register_provenance.size())
                {
                    frame.register_provenance[instruction.reg.index] =
                        instruction.operands[0] < frame.provenance.size()
                            ? frame.provenance[instruction.operands[0]]
                            : InterpreterValueProvenance{};
                }
                break;
            }
            case ir::Opcode::ReadFlag:
                if (const auto stored = store_result(runtime::read_flag(cpu, instruction.flag) ? 1U : 0U);
                    !stored)
                {
                    return Result<runtime::ExecutionResult>::failure(stored.error());
                }
                break;
            case ir::Opcode::WriteFlag:
            {
                const auto operand = read(instruction.operands[0]);
                if (!operand)
                {
                    return Result<runtime::ExecutionResult>::failure(operand.error());
                }
                runtime::write_flag(cpu, instruction.flag, operand.value() != 0U);
                break;
            }
            case ir::Opcode::ReadVectorRegister:
            {
                const auto vector = runtime::read_vector_register(cpu, instruction.vector_index);
                const auto stored = store_result(vector.lo, vector.hi);
                if (!stored) return Result<runtime::ExecutionResult>::failure(stored.error());
                break;
            }
            case ir::Opcode::WriteVectorRegister:
            {
                const auto low = read(instruction.operands[0]);
                const auto high = get_high_value(function, frame.values, frame.high_values, instruction.operands[0]);
                if (!low || !high)
                    return Result<runtime::ExecutionResult>::failure(!low ? low.error() : high.error());
                runtime::write_vector_register(cpu, instruction.vector_index,
                                               runtime::Vector128{low.value(), high.value()});
                break;
            }
            case ir::Opcode::ReadFpControl:
            {
                const auto stored = store_result(cpu.fpcr);
                if (!stored) return Result<runtime::ExecutionResult>::failure(stored.error());
                break;
            }
            case ir::Opcode::WriteFpControl:
            {
                const auto value = read(instruction.operands[0]);
                if (!value) return Result<runtime::ExecutionResult>::failure(value.error());
                cpu.fpcr = static_cast<std::uint32_t>(value.value());
                break;
            }
            case ir::Opcode::ReadFpStatus:
            {
                const auto stored = store_result(cpu.fpsr);
                if (!stored) return Result<runtime::ExecutionResult>::failure(stored.error());
                break;
            }
            case ir::Opcode::WriteFpStatus:
            {
                const auto value = read(instruction.operands[0]);
                if (!value) return Result<runtime::ExecutionResult>::failure(value.error());
                cpu.fpsr = static_cast<std::uint32_t>(value.value());
                break;
            }
            case ir::Opcode::Add:
            case ir::Opcode::Sub:
            case ir::Opcode::Mul:
            case ir::Opcode::And:
            case ir::Opcode::Or:
            case ir::Opcode::Xor:
            {
                const auto left = read(instruction.operands[0]);
                const auto right = read(instruction.operands[1]);
                if (!left || !right)
                {
                    return Result<runtime::ExecutionResult>::failure(!left ? left.error() : right.error());
                }
                const auto value = instruction.opcode == ir::Opcode::Add
                                       ? left.value() + right.value()
                                       : instruction.opcode == ir::Opcode::Sub
                                             ? left.value() - right.value()
                                             : instruction.opcode == ir::Opcode::Mul
                                                   ? left.value() * right.value()
                                                   : instruction.opcode == ir::Opcode::And
                                                         ? left.value() & right.value()
                                                         : instruction.opcode == ir::Opcode::Or
                                                               ? left.value() | right.value()
                                                               : left.value() ^ right.value();
                const auto stored = store_result(value);
                if (!stored)
                {
                    return Result<runtime::ExecutionResult>::failure(stored.error());
                }
                break;
            }
            case ir::Opcode::MulHighUnsigned:
            {
                const auto left = read(instruction.operands[0]);
                const auto right = read(instruction.operands[1]);
                if (!left || !right)
                {
                    return Result<runtime::ExecutionResult>::failure(!left ? left.error() : right.error());
                }
                const auto stored = store_result(
                    common::multiply_high_unsigned_64(left.value(), right.value()));
                if (!stored)
                {
                    return Result<runtime::ExecutionResult>::failure(stored.error());
                }
                break;
            }
            case ir::Opcode::Not:
            {
                const auto operand = read(instruction.operands[0]);
                if (!operand)
                {
                    return Result<runtime::ExecutionResult>::failure(operand.error());
                }
                const auto stored = store_result(~operand.value());
                if (!stored)
                {
                    return Result<runtime::ExecutionResult>::failure(stored.error());
                }
                break;
            }
            case ir::Opcode::ShiftLeft:
            case ir::Opcode::LogicalShiftRight:
            case ir::Opcode::ArithmeticShiftRight:
            case ir::Opcode::RotateRight:
            {
                const auto value = read(instruction.operands[0]);
                const auto amount = read(instruction.operands[1]);
                if (!value || !amount)
                {
                    return Result<runtime::ExecutionResult>::failure(!value ? value.error() : amount.error());
                }
                if (amount.value() >= instruction.result_type.bit_width())
                {
                    return Result<runtime::ExecutionResult>::failure(make_error(
                        ErrorCode::InterpreterError, "IR shift amount is outside the operand width"));
                }
                const auto shift = static_cast<unsigned int>(amount.value());
                std::uint64_t shifted = 0U;
                if (instruction.opcode == ir::Opcode::ShiftLeft)
                {
                    shifted = value.value() << shift;
                }
                else
                {
                    shifted = value.value() >> shift;
                    if (instruction.opcode == ir::Opcode::ArithmeticShiftRight &&
                        signed_bit(value.value(), instruction.result_type) && shift != 0U)
                    {
                        shifted |= std::numeric_limits<std::uint64_t>::max() <<
                                   (instruction.result_type.bit_width() - shift);
                    }
                    else if (instruction.opcode == ir::Opcode::RotateRight && shift != 0U)
                    {
                        shifted = (value.value() >> shift) |
                                  (value.value() << (instruction.result_type.bit_width() - shift));
                    }
                }
                const auto stored = store_result(shifted);
                if (!stored)
                {
                    return Result<runtime::ExecutionResult>::failure(stored.error());
                }
                break;
            }
            case ir::Opcode::Truncate:
            case ir::Opcode::ZeroExtend:
            case ir::Opcode::SignExtend:
            {
                const auto operand = read(instruction.operands[0]);
                const auto definition = function.value(instruction.operands[0]);
                if (!operand || definition == nullptr)
                {
                    return Result<runtime::ExecutionResult>::failure(!operand
                                                                          ? operand.error()
                                                                          : make_error(ErrorCode::InvalidIrValue,
                                                                                      "cast source is invalid"));
                }
                auto value = operand.value();
                if (instruction.opcode == ir::Opcode::SignExtend &&
                    signed_bit(value, definition->type))
                {
                    value |= ~mask_for(definition->type);
                }
                const auto stored = store_result(value);
                if (!stored)
                {
                    return Result<runtime::ExecutionResult>::failure(stored.error());
                }
                break;
            }
            case ir::Opcode::CompareEqual:
            case ir::Opcode::CompareNotEqual:
            case ir::Opcode::CompareUnsigned:
            case ir::Opcode::CompareSigned:
            {
                const auto left = read(instruction.operands[0]);
                const auto right = read(instruction.operands[1]);
                if (!left || !right)
                {
                    return Result<runtime::ExecutionResult>::failure(!left ? left.error() : right.error());
                }
                bool value = false;
                if (instruction.opcode == ir::Opcode::CompareEqual)
                {
                    value = left.value() == right.value();
                }
                else if (instruction.opcode == ir::Opcode::CompareNotEqual)
                {
                    value = left.value() != right.value();
                }
                else if (instruction.opcode == ir::Opcode::CompareUnsigned)
                {
                    value = left.value() < right.value();
                }
                else
                {
                    const auto type = function.value(instruction.operands[0])->type;
                    const auto left_sign = signed_bit(left.value(), type);
                    const auto right_sign = signed_bit(right.value(), type);
                    value = left_sign != right_sign ? left_sign : left.value() < right.value();
                }
                const auto stored = store_result(value ? 1U : 0U);
                if (!stored)
                {
                    return Result<runtime::ExecutionResult>::failure(stored.error());
                }
                break;
            }
            case ir::Opcode::Select:
            {
                const auto condition = read(instruction.operands[0]);
                const auto when_true = read(instruction.operands[1]);
                const auto when_false = read(instruction.operands[2]);
                if (!condition || !when_true || !when_false)
                {
                    return Result<runtime::ExecutionResult>::failure(
                        !condition ? condition.error() : !when_true ? when_true.error() : when_false.error());
                }
                const auto stored = store_result(condition.value() != 0U ? when_true.value() : when_false.value());
                if (!stored)
                {
                    return Result<runtime::ExecutionResult>::failure(stored.error());
                }
                break;
            }
            case ir::Opcode::AddCarry:
            case ir::Opcode::SubCarry:
            case ir::Opcode::AddOverflow:
            case ir::Opcode::SubOverflow:
            {
                const auto left = read(instruction.operands[0]);
                const auto right = read(instruction.operands[1]);
                const auto type = function.value(instruction.operands[0])->type;
                if (!left || !right)
                {
                    return Result<runtime::ExecutionResult>::failure(!left ? left.error() : right.error());
                }
                const auto value = instruction.opcode == ir::Opcode::AddCarry
                                       ? value_for_width(left.value() + right.value(), type) < left.value()
                                       : instruction.opcode == ir::Opcode::SubCarry
                                             ? left.value() >= right.value()
                                             : instruction.opcode == ir::Opcode::AddOverflow
                                                   ? add_overflow(left.value(), right.value(), type)
                                                   : sub_overflow(left.value(), right.value(), type);
                const auto stored = store_result(value ? 1U : 0U);
                if (!stored)
                {
                    return Result<runtime::ExecutionResult>::failure(stored.error());
                }
                break;
            }
            case ir::Opcode::EvaluateCondition:
            {
                const auto n = read(instruction.operands[0]);
                const auto z = read(instruction.operands[1]);
                const auto c = read(instruction.operands[2]);
                const auto v = read(instruction.operands[3]);
                if (!n || !z || !c || !v)
                {
                    return Result<runtime::ExecutionResult>::failure(!n ? n.error() : !z ? z.error() : !c ? c.error() : v.error());
                }
                const auto value = runtime::evaluate_condition(
                    instruction.condition, n.value() != 0U, z.value() != 0U, c.value() != 0U,
                    v.value() != 0U);
                const auto stored = store_result(value ? 1U : 0U);
                if (!stored)
                {
                    return Result<runtime::ExecutionResult>::failure(stored.error());
                }
                break;
            }
            case ir::Opcode::GuestAddressAdd:
            {
                const auto base = read(instruction.operands[0]);
                if (!base)
                {
                    return Result<runtime::ExecutionResult>::failure(base.error());
                }
                const auto sum = checked_add_signed_u64(base.value(), instruction.immediate);
                if (!sum)
                {
                    return Result<runtime::ExecutionResult>::failure(sum.error());
                }
                auto provenance = frame.provenance[instruction.operands[0]];
                if (provenance.kind == InterpreterValueProvenance::Kind::GuestLoad)
                {
                    if (!add_signed_without_overflow(provenance.adjustment, instruction.immediate,
                                                     provenance.adjustment))
                    {
                        provenance = InterpreterValueProvenance{};
                    }
                }
                const auto stored = store_result(sum.value(), 0U, provenance);
                if (!stored)
                {
                    return Result<runtime::ExecutionResult>::failure(stored.error());
                }
                break;
            }
            case ir::Opcode::GuestAddressAddValue:
            {
                const auto base = read(instruction.operands[0]);
                const auto offset = read(instruction.operands[1]);
                if (!base || !offset)
                {
                    return Result<runtime::ExecutionResult>::failure(!base ? base.error()
                                                                            : offset.error());
                }
                const auto sum = instruction.address_offset_signed
                                     ? checked_add_signed_u64(base.value(),
                                                              signed_value_from_u64(offset.value()))
                                     : checked_add_u64(base.value(), offset.value());
                if (!sum)
                {
                    return Result<runtime::ExecutionResult>::failure(sum.error());
                }
                auto provenance = frame.provenance[instruction.operands[0]];
                if (provenance.kind == InterpreterValueProvenance::Kind::GuestLoad &&
                    instruction.operands[1] < frame.provenance.size() &&
                    frame.provenance[instruction.operands[1]].kind ==
                        InterpreterValueProvenance::Kind::Constant)
                {
                    if (offset.value() >
                            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
                        !add_signed_without_overflow(
                            provenance.adjustment, static_cast<std::int64_t>(offset.value()),
                            provenance.adjustment))
                    {
                        provenance = InterpreterValueProvenance{};
                    }
                }
                const auto stored = store_result(sum.value(), 0U, provenance);
                if (!stored)
                {
                    return Result<runtime::ExecutionResult>::failure(stored.error());
                }
                break;
            }
            case ir::Opcode::GuestLoad:
            {
                const auto address = read(instruction.operands[0]);
                if (!address)
                {
                    return Result<runtime::ExecutionResult>::failure(address.error());
                }
                std::uint64_t loaded = 0U;
                if (runtime::switchrecomp_runtime_guest_load(&runtime, address.value(),
                                                             instruction.memory_size, &loaded) != 0U)
                {
                    return runtime_failure(runtime);
                }
                const auto stored = store_result(
                    loaded, 0U,
                    InterpreterValueProvenance{InterpreterValueProvenance::Kind::GuestLoad,
                                               address.value(), 0});
                if (!stored)
                {
                    return Result<runtime::ExecutionResult>::failure(stored.error());
                }
                break;
            }
            case ir::Opcode::GuestStore:
            {
                const auto address = read(instruction.operands[0]);
                const auto value = read(instruction.operands[1]);
                if (!address || !value)
                {
                    return Result<runtime::ExecutionResult>::failure(!address ? address.error() : value.error());
                }
                if (runtime::switchrecomp_runtime_guest_store(&runtime, address.value(),
                                                              instruction.memory_size,
                                                              value.value()) != 0U)
                {
                    return runtime_failure(runtime);
                }
                break;
            }
            case ir::Opcode::BitCast:
            {
                const auto source = read(instruction.operands[0]);
                const auto high = get_high_value(function, frame.values, frame.high_values, instruction.operands[0]);
                if (!source || !high)
                    return Result<runtime::ExecutionResult>::failure(!source ? source.error() : high.error());
                const auto stored = store_result(source.value(), instruction.result_type.is_vector() ? high.value() : 0U);
                if (!stored) return Result<runtime::ExecutionResult>::failure(stored.error());
                break;
            }
            case ir::Opcode::FpBinary:
            {
                const auto left = read(instruction.operands[0]);
                const auto right = read(instruction.operands[1]);
                if (!left || !right)
                    return Result<runtime::ExecutionResult>::failure(!left ? left.error() : right.error());
                const auto value = runtime::fp_binary(cpu,
                    static_cast<runtime::FpBinaryOperation>(instruction.fp_binary),
                    instruction.result_type == ir::f32_type() ? 32U : 64U,
                    left.value(), right.value());
                const auto stored = store_result(value);
                if (!stored) return Result<runtime::ExecutionResult>::failure(stored.error());
                break;
            }
            case ir::Opcode::FpUnary:
            {
                const auto value = read(instruction.operands[0]);
                if (!value) return Result<runtime::ExecutionResult>::failure(value.error());
                const auto result_value = runtime::fp_unary(cpu,
                    static_cast<runtime::FpUnaryOperation>(instruction.fp_unary),
                    instruction.result_type == ir::f32_type() ? 32U : 64U, value.value());
                const auto stored = store_result(result_value);
                if (!stored) return Result<runtime::ExecutionResult>::failure(stored.error());
                break;
            }
            case ir::Opcode::FpCompare:
            {
                const auto left = read(instruction.operands[0]);
                const auto right = read(instruction.operands[1]);
                if (!left || !right)
                    return Result<runtime::ExecutionResult>::failure(!left ? left.error() : right.error());
                const auto result_value = runtime::fp_compare(cpu,
                    instruction.result_type == ir::i32_type() && function.value(instruction.operands[0])->type == ir::f32_type() ? 32U : 64U,
                    left.value(), right.value(), instruction.signaling);
                const auto stored = store_result(result_value);
                if (!stored) return Result<runtime::ExecutionResult>::failure(stored.error());
                break;
            }
            case ir::Opcode::FpConvert:
            {
                const auto value = read(instruction.operands[0]);
                const auto source = function.value(instruction.operands[0]);
                if (!value || source == nullptr)
                    return Result<runtime::ExecutionResult>::failure(!value ? value.error() : make_error(ErrorCode::InvalidIrValue, "FP conversion source is invalid"));
                const auto result_value = runtime::fp_convert(cpu,
                    static_cast<runtime::FpConversion>(instruction.fp_conversion),
                    source->type.bit_width(), instruction.result_type.bit_width(), value.value());
                const auto stored = store_result(result_value);
                if (!stored) return Result<runtime::ExecutionResult>::failure(stored.error());
                break;
            }
            case ir::Opcode::FpRound:
            {
                const auto value = read(instruction.operands[0]);
                if (!value) return Result<runtime::ExecutionResult>::failure(value.error());
                const auto result_value = runtime::fp_round(cpu,
                    instruction.result_type == ir::f32_type() ? 32U : 64U, value.value(),
                    static_cast<runtime::FpRoundingMode>(instruction.rounding_mode));
                const auto stored = store_result(result_value);
                if (!stored) return Result<runtime::ExecutionResult>::failure(stored.error());
                break;
            }
            case ir::Opcode::VectorExtractLane:
            {
                const auto vector = read(instruction.operands[0]);
                const auto high = get_high_value(function, frame.values, frame.high_values, instruction.operands[0]);
                if (!vector || !high)
                    return Result<runtime::ExecutionResult>::failure(!vector ? vector.error() : high.error());
                const auto lane = runtime::read_lane_bits(runtime::Vector128{vector.value(), high.value()},
                                                          static_cast<std::uint8_t>(instruction.result_type.bit_width()),
                                                          instruction.lane_index);
                const auto stored = store_result(lane);
                if (!stored) return Result<runtime::ExecutionResult>::failure(stored.error());
                break;
            }
            case ir::Opcode::VectorInsertLane:
            {
                const auto vector = read(instruction.operands[0]);
                const auto vector_high = get_high_value(function, frame.values, frame.high_values, instruction.operands[0]);
                const auto lane = read(instruction.operands[1]);
                if (!vector || !vector_high || !lane)
                    return Result<runtime::ExecutionResult>::failure(!vector ? vector.error() : !vector_high ? vector_high.error() : lane.error());
                auto result_vector = runtime::Vector128{vector.value(), vector_high.value()};
                runtime::write_lane_bits(result_vector, static_cast<std::uint8_t>(function.value(instruction.operands[1])->type.bit_width()), instruction.lane_index, lane.value());
                const auto stored = store_result(result_vector.lo, result_vector.hi);
                if (!stored) return Result<runtime::ExecutionResult>::failure(stored.error());
                break;
            }
            case ir::Opcode::VectorBroadcast:
            {
                const auto lane = read(instruction.operands[0]);
                if (!lane) return Result<runtime::ExecutionResult>::failure(lane.error());
                const auto bits = static_cast<std::uint8_t>(instruction.arrangement == ir::VectorArrangement::B8 || instruction.arrangement == ir::VectorArrangement::B16 ? 8U : instruction.arrangement == ir::VectorArrangement::H4 || instruction.arrangement == ir::VectorArrangement::H8 ? 16U : instruction.arrangement == ir::VectorArrangement::S2 || instruction.arrangement == ir::VectorArrangement::S4 ? 32U : 64U);
                const auto count = static_cast<std::uint8_t>(instruction.arrangement == ir::VectorArrangement::B8 ? 8U : instruction.arrangement == ir::VectorArrangement::B16 ? 16U : instruction.arrangement == ir::VectorArrangement::H4 ? 4U : instruction.arrangement == ir::VectorArrangement::H8 ? 8U : instruction.arrangement == ir::VectorArrangement::S2 ? 2U : instruction.arrangement == ir::VectorArrangement::S4 ? 4U : instruction.arrangement == ir::VectorArrangement::D1 ? 1U : 2U);
                const auto result_vector = runtime::broadcast_lane(lane.value(), bits, count);
                const auto stored = store_result(result_vector.lo, result_vector.hi);
                if (!stored) return Result<runtime::ExecutionResult>::failure(stored.error());
                break;
            }
            case ir::Opcode::VectorBinary:
            case ir::Opcode::VectorCompare:
            {
                const auto left = read(instruction.operands[0]);
                const auto left_high = get_high_value(function, frame.values, frame.high_values, instruction.operands[0]);
                const auto right = read(instruction.operands[1]);
                const auto right_high = get_high_value(function, frame.values, frame.high_values, instruction.operands[1]);
                if (!left || !left_high || !right || !right_high)
                    return Result<runtime::ExecutionResult>::failure(!left ? left.error() : !left_high ? left_high.error() : !right ? right.error() : right_high.error());
                const auto arrangement = static_cast<std::uint8_t>(instruction.arrangement);
                const auto result_vector = instruction.opcode == ir::Opcode::VectorBinary
                    ? runtime::vector_binary(cpu, static_cast<std::uint8_t>(instruction.vector_operation), arrangement,
                                             runtime::Vector128{left.value(), left_high.value()}, runtime::Vector128{right.value(), right_high.value()})
                    : runtime::vector_compare(cpu, static_cast<std::uint8_t>(instruction.vector_compare), arrangement,
                                              runtime::Vector128{left.value(), left_high.value()}, runtime::Vector128{right.value(), right_high.value()});
                const auto stored = store_result(result_vector.lo, result_vector.hi);
                if (!stored) return Result<runtime::ExecutionResult>::failure(stored.error());
                break;
            }
            case ir::Opcode::VectorShuffle:
            {
                const auto left = read(instruction.operands[0]);
                const auto left_high = get_high_value(function, frame.values, frame.high_values, instruction.operands[0]);
                const auto right = read(instruction.operands[1]);
                const auto right_high = get_high_value(function, frame.values, frame.high_values, instruction.operands[1]);
                if (!left || !left_high || !right || !right_high)
                    return Result<runtime::ExecutionResult>::failure(!left ? left.error() : !left_high ? left_high.error() : !right ? right.error() : right_high.error());
                const auto result_vector = runtime::vector_shuffle(instruction.vector_index,
                    static_cast<std::uint8_t>(instruction.arrangement),
                    runtime::Vector128{left.value(), left_high.value()}, runtime::Vector128{right.value(), right_high.value()},
                    static_cast<std::uint8_t>(instruction.immediate));
                const auto stored = store_result(result_vector.lo, result_vector.hi);
                if (!stored) return Result<runtime::ExecutionResult>::failure(stored.error());
                break;
            }
            case ir::Opcode::GuestLoadVector:
            {
                const auto address = read(instruction.operands[0]);
                if (!address) return Result<runtime::ExecutionResult>::failure(address.error());
                runtime::Vector128 vector{};
                if (runtime::switchrecomp_runtime_guest_load_vector(&runtime, address.value(), &vector) != 0U)
                    return runtime_failure(runtime);
                const auto stored = store_result(vector.lo, vector.hi);
                if (!stored) return Result<runtime::ExecutionResult>::failure(stored.error());
                break;
            }
            case ir::Opcode::GuestStoreVector:
            {
                const auto address = read(instruction.operands[0]);
                const auto low = read(instruction.operands[1]);
                const auto high = get_high_value(function, frame.values, frame.high_values, instruction.operands[1]);
                if (!address || !low || !high)
                    return Result<runtime::ExecutionResult>::failure(!address ? address.error() : !low ? low.error() : high.error());
                const runtime::Vector128 vector{low.value(), high.value()};
                if (runtime::switchrecomp_runtime_guest_store_vector(&runtime, address.value(), &vector) != 0U)
                    return runtime_failure(runtime);
                break;
            }
            default:
                return Result<runtime::ExecutionResult>::failure(make_error(
                    ErrorCode::UnsupportedInstruction,
                    "legacy interpreter received a Milestone 9 opcode"));
            }
        }

        if (!block->has_terminator)
        {
            return Result<runtime::ExecutionResult>::failure(
                make_error(ErrorCode::InvalidIrBlock, "interpreter reached an unterminated block"));
        }
        const auto& terminator = block->terminator;
        switch (terminator.kind)
        {
        case ir::TerminatorKind::Branch:
            frame.current_block = terminator.target;
            break;
        case ir::TerminatorKind::ConditionalBranch:
        {
            const auto condition = get_value(function, frame.values, terminator.condition);
            if (!condition)
            {
                return Result<runtime::ExecutionResult>::failure(condition.error());
            }
            frame.current_block = condition.value() != 0U ? terminator.target : terminator.false_target;
            break;
        }
        case ir::TerminatorKind::Return:
            if (terminator.target_value != ir::invalid_value)
            {
                const auto target = get_value(function, frame.values, terminator.target_value);
                if (!target)
                {
                    return Result<runtime::ExecutionResult>::failure(target.error());
                }
                if (target.value() != 0U)
                {
                    cpu.pc = target.value();
                }
            }
            result.final_guest_pc = cpu.pc;
            result.boundary = runtime::ExecutionBoundary{
                runtime::ExecutionBoundaryKind::Return, terminator.source.guest_pc, cpu.pc,
                cpu.pc != 0U, ir::invalid_block, 0U, function.guest_entry(), false, 0U, {}, {}};
            flush_observation(cpu.pc == 0U ? std::nullopt
                                          : std::optional<std::uint64_t>(cpu.pc));
            return Result<runtime::ExecutionResult>::success(result);
        case ir::TerminatorKind::DirectCall:
        case ir::TerminatorKind::FunctionTransfer:
        case ir::TerminatorKind::IndirectBranch:
        case ir::TerminatorKind::IndirectCall:
        {
            const auto target = get_value(function, frame.values, terminator.target_value);
            if (!target)
            {
                return Result<runtime::ExecutionResult>::failure(target.error());
            }
            cpu.pc = target.value();
            result.final_guest_pc = cpu.pc;
            const auto kind = terminator.kind == ir::TerminatorKind::DirectCall
                                  ? runtime::ExecutionBoundaryKind::DirectCall
                              : terminator.kind == ir::TerminatorKind::FunctionTransfer
                                  ? runtime::ExecutionBoundaryKind::FunctionTransfer
                              : terminator.kind == ir::TerminatorKind::IndirectCall
                                  ? runtime::ExecutionBoundaryKind::IndirectCall
                                  : runtime::ExecutionBoundaryKind::IndirectBranch;
            const auto provenance = terminator.target_value < frame.provenance.size()
                                        ? frame.provenance[terminator.target_value]
                                        : InterpreterValueProvenance{};
            std::string target_provenance;
            if (provenance.kind == InterpreterValueProvenance::Kind::GuestLoad)
            {
                std::ostringstream provenance_text;
                provenance_text << "guest_load:0x" << std::hex << std::setw(16)
                                << std::setfill('0') << provenance.address;
                target_provenance = provenance_text.str();
                if (provenance.adjustment != 0)
                {
                    target_provenance += ",adjustment=" + std::to_string(provenance.adjustment);
                }
            }
            result.status = runtime::ExecutionStatus::Boundary;
            result.boundary = runtime::ExecutionBoundary{
                kind, terminator.source.guest_pc, target.value(), true,
                terminator.continuation, terminator.continuation_guest_pc,
                function.guest_entry(),
                provenance.kind == InterpreterValueProvenance::Kind::GuestLoad,
                provenance.address, std::move(target_provenance),
                terminator.target_register ? ir::register_name(terminator.target_register.value()) : ""};
            flush_observation(cpu.pc == 0U ? std::nullopt
                                          : std::optional<std::uint64_t>(cpu.pc));
            return Result<runtime::ExecutionResult>::success(result);
        }
        case ir::TerminatorKind::Trap:
            if (terminator.trap_reason.rfind("unsupported_instruction:", 0U) == 0U)
            {
                result.status = runtime::ExecutionStatus::Boundary;
                result.boundary = runtime::ExecutionBoundary{
                    runtime::ExecutionBoundaryKind::UnsupportedInstruction,
                    terminator.source.guest_pc, 0U, false, ir::invalid_block, 0U,
                    function.guest_entry(), false, 0U,
                    terminator.trap_reason.substr(std::string("unsupported_instruction:").size()), {}};
                result.final_guest_pc = cpu.pc;
                flush_observation(std::nullopt);
                return Result<runtime::ExecutionResult>::success(std::move(result));
            }
            (void)runtime::switchrecomp_runtime_trap(&runtime, terminator.trap_reason.c_str());
            if (runtime.has_error)
            {
                result.status = runtime::ExecutionStatus::Trapped;
                result.boundary = runtime::ExecutionBoundary{
                    runtime::ExecutionBoundaryKind::Trap, terminator.source.guest_pc, 0U, false,
                    ir::invalid_block, 0U, function.guest_entry(), false, 0U,
                    runtime.last_error.message, {}};
                result.final_guest_pc = cpu.pc;
                flush_observation(std::nullopt);
                return Result<runtime::ExecutionResult>::success(std::move(result));
            }
            return runtime_failure(runtime);
        }
    }
}

// Preserve the historical one-function API. M11 callers use
// execute_until_boundary directly so a call boundary remains typed and
// resumable; legacy callers still receive an error for operation exhaustion.
Result<runtime::ExecutionResult> execute_legacy(const ir::Function& function,
                                                runtime::CpuState& cpu,
                                                runtime::RuntimeContext& runtime,
                                                const runtime::ExecutionOptions& options)
{
    InterpreterFrame frame;
    const auto result = execute_until_boundary(function, cpu, runtime, frame, options);
    if (!result)
    {
        return result;
    }
    if (result.value().status == runtime::ExecutionStatus::LimitExceeded)
    {
        return Result<runtime::ExecutionResult>::failure(make_error(
            ErrorCode::ExecutionLimitExceeded,
            "interpreter exceeded the configured IR operation limit"));
    }
    if (result.value().status == runtime::ExecutionStatus::Trapped)
    {
        return Result<runtime::ExecutionResult>::failure(
            runtime.has_error ? runtime.last_error
                              : make_error(ErrorCode::ExecutionTrap, "guest execution trapped"));
    }
    return result;
}

} // namespace switchrecomp::interpreter
