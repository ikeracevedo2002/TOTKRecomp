#include "switchrecomp/lifter/lifter.hpp"

#include "switchrecomp/aarch64/instruction.hpp"
#include "switchrecomp/common/checked_arithmetic.hpp"
#include "switchrecomp/ir/builder.hpp"
#include "switchrecomp/ir/verifier.hpp"

#include <iomanip>
#include <limits>
#include <map>
#include <new>
#include <sstream>
#include <string>
#include <utility>

namespace switchrecomp::lifter
{

namespace
{

using aarch64::DecodedInstruction;
using aarch64::GuestAddress;

[[nodiscard]] std::string hex_address(GuestAddress address)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::setw(16) << std::setfill('0') << address;
    return output.str();
}

[[nodiscard]] std::string instruction_description(const DecodedInstruction& instruction)
{
    std::ostringstream output;
    output << "AArch64 " << aarch64::instruction_id_name(instruction.id) << " at "
           << hex_address(instruction.address) << " (0x" << std::hex << std::setw(8)
           << std::setfill('0') << instruction.opcode << ")";
    if (!instruction.disassembly.empty())
    {
        output << ": " << instruction.disassembly;
    }
    return output.str();
}

[[nodiscard]] Error unsupported(const DecodedInstruction& instruction, std::string reason)
{
    return make_error(ErrorCode::UnsupportedInstruction,
                      instruction_description(instruction) + ": " + std::move(reason));
}

[[nodiscard]] ir::Type type_for_width(aarch64::RegisterWidth width)
{
    return width == aarch64::RegisterWidth::W32 ? ir::i32_type() : ir::i64_type();
}

[[nodiscard]] Result<ir::GuestRegister> to_ir_register(const aarch64::Register& reg,
                                                       const DecodedInstruction& instruction)
{
    if (reg.kind != aarch64::RegisterKind::General || !reg.valid() || reg.index > 31U ||
        (reg.width != aarch64::RegisterWidth::W32 &&
         reg.width != aarch64::RegisterWidth::X64))
    {
        return Result<ir::GuestRegister>::failure(unsupported(
            instruction, "operand form is not a scalar W/X general-purpose register"));
    }
    return Result<ir::GuestRegister>::success(ir::GuestRegister{
        reg.width == aarch64::RegisterWidth::W32 ? ir::RegisterWidth::W32
                                                 : ir::RegisterWidth::X64,
        reg.index,
        reg.is_stack_pointer,
        reg.is_zero});
}

[[nodiscard]] Result<std::uint64_t> masked_constant(ir::Type type, std::int64_t value,
                                                    const DecodedInstruction& instruction)
{
    if (type == ir::i64_type())
    {
        return Result<std::uint64_t>::success(static_cast<std::uint64_t>(value));
    }
    if (type == ir::i32_type())
    {
        return Result<std::uint64_t>::success(
            static_cast<std::uint64_t>(static_cast<std::uint32_t>(value)));
    }
    return Result<std::uint64_t>::failure(
        unsupported(instruction, "immediate requires a W/X integer destination"));
}

class FunctionLifter
{
  public:
    FunctionLifter(const analysis::ControlFlowGraph& cfg, LiftOptions options)
        : cfg_(cfg), options_(options), function_(make_function(cfg)), builder_(function_)
    {
    }

    [[nodiscard]] Result<ir::Function> run()
    {
        const auto valid_cfg = analysis::validate_control_flow_graph(cfg_);
        if (!valid_cfg)
        {
            return Result<ir::Function>::failure(make_error(
                ErrorCode::InvalidControlFlow, "cannot lift invalid CFG: " + valid_cfg.error().message));
        }
        if (cfg_.blocks.empty() || !cfg_.blocks.contains(cfg_.entry))
        {
            return Result<ir::Function>::failure(
                make_error(ErrorCode::InvalidControlFlow, "CFG entry is not a basic-block leader"));
        }
        if (cfg_.blocks.size() > options_.max_basic_blocks)
        {
            return Result<ir::Function>::failure(make_error(
                ErrorCode::LiftLimitExceeded, "CFG exceeds the lifter basic-block limit"));
        }

        for (const auto& [address, block] : cfg_.blocks)
        {
            const auto id = function_.add_block(address, "block_" + std::to_string(function_.blocks().size()));
            block_ids_.emplace(address, id);
            (void)block;
        }
        function_.set_entry_block(block_ids_.at(cfg_.entry));

        for (const auto& [address, source_block] : cfg_.blocks)
        {
            const auto set_block = builder_.set_insert_block(block_ids_.at(address));
            if (!set_block)
            {
                return Result<ir::Function>::failure(set_block.error());
            }
            const auto lifted = lift_block(source_block);
            if (!lifted)
            {
                return Result<ir::Function>::failure(lifted.error());
            }
        }

        if (options_.verify_result)
        {
            const auto verified = ir::verify(function_);
            if (!verified)
            {
                return Result<ir::Function>::failure(verified.error());
            }
        }
        return Result<ir::Function>::success(std::move(function_));
    }

  private:
    [[nodiscard]] static ir::Function make_function(const analysis::ControlFlowGraph& cfg)
    {
        std::ostringstream name;
        name << "guest_" << std::hex << std::setw(16) << std::setfill('0') << cfg.entry;
        return ir::Function(name.str(), cfg.entry);
    }

    [[nodiscard]] Result<void> account_operation()
    {
        ++operations_for_instruction_;
        if (operations_for_instruction_ > options_.max_ir_operations_per_guest_instruction)
        {
            return Result<void>::failure(make_error(
                ErrorCode::LiftLimitExceeded,
                "one guest instruction expanded beyond the configured IR operation limit"));
        }
        if (total_ir_instructions() > options_.max_ir_instructions ||
            function_.values().size() > options_.max_values)
        {
            return Result<void>::failure(make_error(
                ErrorCode::LiftLimitExceeded, "lifted function exceeds its configured IR limits"));
        }
        return Result<void>::success();
    }

    [[nodiscard]] std::size_t total_ir_instructions() const noexcept
    {
        std::size_t count = 0U;
        for (const auto& block : function_.blocks())
        {
            count += block.instructions.size();
        }
        return count;
    }

    [[nodiscard]] ir::SourceLocation source_location(const DecodedInstruction& instruction) const
    {
        if (!options_.preserve_source_mapping)
        {
            return ir::SourceLocation{instruction.address, instruction.opcode, {}};
        }
        return ir::SourceLocation{instruction.address, instruction.opcode,
                                  instruction.disassembly};
    }

    [[nodiscard]] Result<void> emit_void(ir::Instruction instruction)
    {
        const auto counted = account_operation();
        if (!counted)
        {
            return counted;
        }
        return builder_.emit_void(std::move(instruction));
    }

    [[nodiscard]] Result<ir::ValueId> emit_value(ir::Instruction instruction)
    {
        const auto counted = account_operation();
        if (!counted)
        {
            return Result<ir::ValueId>::failure(counted.error());
        }
        return builder_.emit(std::move(instruction));
    }

    [[nodiscard]] Result<ir::ValueId> constant(ir::Type type, std::uint64_t value,
                                               const DecodedInstruction& instruction)
    {
        return emit_value(ir::Instruction{ir::Opcode::Constant, ir::invalid_value, type, {}, {},
                                          ir::Flag::N, ir::ConditionCode::Al, 0, value, 0,
                                          source_location(instruction)});
    }

    [[nodiscard]] Result<ir::ValueId> read_register(const aarch64::Register& reg,
                                                    const DecodedInstruction& instruction)
    {
        const auto converted = to_ir_register(reg, instruction);
        if (!converted)
        {
            return Result<ir::ValueId>::failure(converted.error());
        }
        const auto type = type_for_width(reg.width);
        return emit_value(ir::Instruction{ir::Opcode::ReadRegister, ir::invalid_value, type, {},
                                          converted.value(), ir::Flag::N, ir::ConditionCode::Al, 0,
                                          0, 0, source_location(instruction)});
    }

    [[nodiscard]] Result<void> write_register(const aarch64::Register& reg, ir::ValueId value,
                                              const DecodedInstruction& instruction)
    {
        const auto converted = to_ir_register(reg, instruction);
        if (!converted)
        {
            return Result<void>::failure(converted.error());
        }
        return emit_void(ir::Instruction{ir::Opcode::WriteRegister, ir::invalid_value,
                                         ir::void_type(), {value}, converted.value(), ir::Flag::N,
                                         ir::ConditionCode::Al, 0, 0, 0,
                                         source_location(instruction)});
    }

    [[nodiscard]] Result<ir::ValueId> binary(ir::Opcode opcode, ir::ValueId left,
                                             ir::ValueId right, ir::Type type,
                                             const DecodedInstruction& instruction)
    {
        return emit_value(ir::Instruction{opcode, ir::invalid_value, type, {left, right}, {},
                                          ir::Flag::N, ir::ConditionCode::Al, 0, 0, 0,
                                          source_location(instruction)});
    }

    [[nodiscard]] Result<ir::ValueId> apply_shift(ir::ValueId value, ir::Type type,
                                                  const aarch64::Operand& operand,
                                                  const DecodedInstruction& instruction)
    {
        if (operand.shift_kind == aarch64::ShiftKind::None || operand.shift == 0U)
        {
            return Result<ir::ValueId>::success(value);
        }
        if (operand.shift >= type.bit_width())
        {
            return Result<ir::ValueId>::failure(
                unsupported(instruction, "shift amount is outside the operand width"));
        }
        const auto shift = constant(type, operand.shift, instruction);
        if (!shift)
        {
            return shift;
        }
        const auto opcode = operand.shift_kind == aarch64::ShiftKind::Lsl
                                ? ir::Opcode::ShiftLeft
                                : operand.shift_kind == aarch64::ShiftKind::Lsr
                                      ? ir::Opcode::LogicalShiftRight
                                      : ir::Opcode::ArithmeticShiftRight;
        return binary(opcode, value, shift.value(), type, instruction);
    }

    [[nodiscard]] Result<ir::ValueId> operand_value(const aarch64::Operand& operand,
                                                    ir::Type type,
                                                    const DecodedInstruction& instruction)
    {
        if (operand.kind == aarch64::OperandKind::Register)
        {
            const auto value = read_register(operand.reg, instruction);
            if (!value)
            {
                return value;
            }
            const auto actual_type = type_for_width(operand.reg.width);
            if (actual_type != type)
            {
                return Result<ir::ValueId>::failure(
                    unsupported(instruction, "register operand width does not match destination"));
            }
            return apply_shift(value.value(), type, operand, instruction);
        }
        if (operand.kind == aarch64::OperandKind::Immediate &&
            operand.shift_kind != aarch64::ShiftKind::Lsr &&
            operand.shift_kind != aarch64::ShiftKind::Asr)
        {
            auto immediate = operand.immediate;
            if (operand.shift_kind == aarch64::ShiftKind::Lsl && operand.shift != 0U)
            {
                if (operand.shift >= type.bit_width())
                {
                    return Result<ir::ValueId>::failure(
                        unsupported(instruction, "immediate shift is outside the operand width"));
                }
                const auto unsigned_value = static_cast<std::uint64_t>(immediate);
                const auto shifted = unsigned_value << operand.shift;
                immediate = static_cast<std::int64_t>(shifted);
            }
            const auto value = masked_constant(type, immediate, instruction);
            if (!value)
            {
                return Result<ir::ValueId>::failure(value.error());
            }
            return constant(type, value.value(), instruction);
        }
        return Result<ir::ValueId>::failure(
            unsupported(instruction, "operand is not a supported register or immediate"));
    }

    [[nodiscard]] Result<void> lift_arithmetic(const DecodedInstruction& instruction,
                                               ir::Opcode opcode, bool sets_flags)
    {
        if (instruction.operands.size() != 3U || instruction.operands[0].kind != aarch64::OperandKind::Register ||
            instruction.operands[1].kind != aarch64::OperandKind::Register)
        {
            return Result<void>::failure(unsupported(instruction, "expected destination, register, operand"));
        }
        const auto converted = to_ir_register(instruction.operands[0].reg, instruction);
        if (!converted)
        {
            return Result<void>::failure(converted.error());
        }
        const auto type = type_for_width(instruction.operands[0].reg.width);
        const auto left = operand_value(instruction.operands[1], type, instruction);
        const auto right = operand_value(instruction.operands[2], type, instruction);
        if (!left || !right)
        {
            return Result<void>::failure(!left ? left.error() : right.error());
        }
        const auto result = binary(opcode, left.value(), right.value(), type, instruction);
        if (!result)
        {
            return Result<void>::failure(result.error());
        }
        if (sets_flags)
        {
            const auto flags = emit_flags(left.value(), right.value(), result.value(),
                                          opcode == ir::Opcode::Sub, type, instruction);
            if (!flags)
            {
                return flags;
            }
        }
        return write_register(instruction.operands[0].reg, result.value(), instruction);
    }

    [[nodiscard]] Result<void> lift_cmp(const DecodedInstruction& instruction)
    {
        if (instruction.operands.size() != 2U || instruction.operands[0].kind != aarch64::OperandKind::Register)
        {
            return Result<void>::failure(unsupported(instruction, "expected two arithmetic operands"));
        }
        const auto type = type_for_width(instruction.operands[0].reg.width);
        const auto left = operand_value(instruction.operands[0], type, instruction);
        const auto right = operand_value(instruction.operands[1], type, instruction);
        if (!left || !right)
        {
            return Result<void>::failure(!left ? left.error() : right.error());
        }
        const auto result = binary(ir::Opcode::Sub, left.value(), right.value(), type, instruction);
        if (!result)
        {
            return Result<void>::failure(result.error());
        }
        return emit_flags(left.value(), right.value(), result.value(), true, type, instruction);
    }

    [[nodiscard]] Result<void> emit_flags(ir::ValueId left, ir::ValueId right, ir::ValueId result,
                                          bool subtraction, ir::Type type,
                                          const DecodedInstruction& instruction)
    {
        const auto zero = constant(type, 0U, instruction);
        if (!zero)
        {
            return Result<void>::failure(zero.error());
        }
        const auto z = binary(ir::Opcode::CompareEqual, result, zero.value(), ir::i1_type(), instruction);
        if (!z)
        {
            return Result<void>::failure(z.error());
        }
        const auto shift_amount = constant(type, type.bit_width() - 1U, instruction);
        if (!shift_amount)
        {
            return Result<void>::failure(shift_amount.error());
        }
        const auto shifted = binary(ir::Opcode::LogicalShiftRight, result, shift_amount.value(), type, instruction);
        if (!shifted)
        {
            return Result<void>::failure(shifted.error());
        }
        ir::Instruction truncate{ir::Opcode::Truncate, ir::invalid_value, ir::i1_type(),
                                 {shifted.value()}, {}, ir::Flag::N, ir::ConditionCode::Al, 0, 0,
                                 0, source_location(instruction)};
        const auto n = emit_value(std::move(truncate));
        if (!n)
        {
            return Result<void>::failure(n.error());
        }
        ir::Instruction carry_instruction;
        carry_instruction.opcode = subtraction ? ir::Opcode::SubCarry : ir::Opcode::AddCarry;
        carry_instruction.result_type = ir::i1_type();
        carry_instruction.operands = {left, right};
        carry_instruction.source = source_location(instruction);
        const auto carry = emit_value(std::move(carry_instruction));
        if (!carry)
        {
            return Result<void>::failure(carry.error());
        }
        ir::Instruction overflow_instruction;
        overflow_instruction.opcode = subtraction ? ir::Opcode::SubOverflow : ir::Opcode::AddOverflow;
        overflow_instruction.result_type = ir::i1_type();
        overflow_instruction.operands = {left, right};
        overflow_instruction.source = source_location(instruction);
        const auto overflow = emit_value(std::move(overflow_instruction));
        if (!overflow)
        {
            return Result<void>::failure(overflow.error());
        }
        for (const auto& [flag, value] : {std::pair{ir::Flag::N, n.value()},
                                          std::pair{ir::Flag::Z, z.value()},
                                          std::pair{ir::Flag::C, carry.value()},
                                          std::pair{ir::Flag::V, overflow.value()}})
        {
            ir::Instruction write;
            write.opcode = ir::Opcode::WriteFlag;
            write.operands = {value};
            write.flag = flag;
            write.source = source_location(instruction);
            const auto emitted = emit_void(std::move(write));
            if (!emitted)
            {
                return emitted;
            }
        }
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> lift_logic(const DecodedInstruction& instruction, ir::Opcode opcode)
    {
        if (instruction.operands.size() != 3U || instruction.operands[0].kind != aarch64::OperandKind::Register ||
            instruction.operands[1].kind != aarch64::OperandKind::Register)
        {
            return Result<void>::failure(unsupported(instruction, "expected destination, register, operand"));
        }
        const auto type = type_for_width(instruction.operands[0].reg.width);
        const auto left = operand_value(instruction.operands[1], type, instruction);
        const auto right = operand_value(instruction.operands[2], type, instruction);
        if (!left || !right)
        {
            return Result<void>::failure(!left ? left.error() : right.error());
        }
        const auto result = binary(opcode, left.value(), right.value(), type, instruction);
        if (!result)
        {
            return Result<void>::failure(result.error());
        }
        return write_register(instruction.operands[0].reg, result.value(), instruction);
    }

    [[nodiscard]] Result<void> lift_mov(const DecodedInstruction& instruction)
    {
        if (instruction.operands.size() != 2U || instruction.operands[0].kind != aarch64::OperandKind::Register)
        {
            return Result<void>::failure(unsupported(instruction, "expected destination and source"));
        }
        const auto type = type_for_width(instruction.operands[0].reg.width);
        const auto source = operand_value(instruction.operands[1], type, instruction);
        if (!source)
        {
            return Result<void>::failure(source.error());
        }
        return write_register(instruction.operands[0].reg, source.value(), instruction);
    }

    [[nodiscard]] Result<void> lift_movz(const DecodedInstruction& instruction, bool keep)
    {
        if (instruction.operands.size() != 2U || instruction.operands[0].kind != aarch64::OperandKind::Register ||
            instruction.operands[1].kind != aarch64::OperandKind::Immediate)
        {
            return Result<void>::failure(unsupported(instruction, "expected register and 16-bit immediate"));
        }
        const auto type = type_for_width(instruction.operands[0].reg.width);
        const auto shift = instruction.operands[1].shift;
        const auto max_shift = type == ir::i32_type() ? 16U : 48U;
        if (instruction.operands[1].immediate < 0 || instruction.operands[1].immediate > 0xffff ||
            (instruction.operands[1].shift_kind != aarch64::ShiftKind::None &&
             instruction.operands[1].shift_kind != aarch64::ShiftKind::Lsl) ||
            shift > max_shift || (shift % 16U) != 0U)
        {
            return Result<void>::failure(unsupported(instruction, "invalid MOVZ/MOVK immediate or shift"));
        }
        if (!keep)
        {
            const auto value = static_cast<std::uint64_t>(instruction.operands[1].immediate) << shift;
            const auto constant_value = constant(type, value, instruction);
            if (!constant_value)
            {
                return Result<void>::failure(constant_value.error());
            }
            return write_register(instruction.operands[0].reg, constant_value.value(), instruction);
        }
        const auto old = read_register(instruction.operands[0].reg, instruction);
        if (!old)
        {
            return Result<void>::failure(old.error());
        }
        const auto mask = type == ir::i32_type()
                              ? static_cast<std::uint64_t>(~(std::uint32_t{0xffff} << shift))
                              : ~(std::uint64_t{0xffff} << shift);
        const auto mask_value = constant(type, mask, instruction);
        const auto insert_value = constant(
            type, static_cast<std::uint64_t>(instruction.operands[1].immediate) << shift, instruction);
        if (!mask_value || !insert_value)
        {
            return Result<void>::failure(!mask_value ? mask_value.error() : insert_value.error());
        }
        const auto cleared = binary(ir::Opcode::And, old.value(), mask_value.value(), type, instruction);
        const auto combined = cleared ? binary(ir::Opcode::Or, cleared.value(), insert_value.value(), type,
                                                instruction)
                                      : Result<ir::ValueId>::failure(cleared.error());
        if (!combined)
        {
            return Result<void>::failure(combined.error());
        }
        return write_register(instruction.operands[0].reg, combined.value(), instruction);
    }

    [[nodiscard]] Result<void> lift_pc_relative(const DecodedInstruction& instruction)
    {
        if (instruction.operands.size() != 2U || instruction.operands[0].kind != aarch64::OperandKind::Register ||
            instruction.operands[1].kind != aarch64::OperandKind::Immediate ||
            !instruction.pc_relative_value || instruction.operands[0].reg.width != aarch64::RegisterWidth::X64)
        {
            return Result<void>::failure(unsupported(instruction, "expected X destination and validated PC-relative value"));
        }
        const auto value = constant(ir::i64_type(), instruction.pc_relative_value.value(), instruction);
        if (!value)
        {
            return Result<void>::failure(value.error());
        }
        return write_register(instruction.operands[0].reg, value.value(), instruction);
    }

    [[nodiscard]] Result<ir::ValueId> effective_address(const aarch64::Operand& operand,
                                                        const DecodedInstruction& instruction)
    {
        if (operand.kind != aarch64::OperandKind::Memory ||
            operand.memory.addressing != aarch64::MemoryAddressingMode::Base ||
            operand.memory.writeback || operand.memory.index.valid() ||
            !operand.memory.base.valid() || operand.memory.base.width != aarch64::RegisterWidth::X64)
        {
            return Result<ir::ValueId>::failure(
                unsupported(instruction, "only X/SP base plus signed immediate memory addressing is supported"));
        }
        const auto base = read_register(operand.memory.base, instruction);
        if (!base)
        {
            return base;
        }
        ir::Instruction address_add;
        address_add.opcode = ir::Opcode::GuestAddressAdd;
        address_add.result_type = ir::i64_type();
        address_add.operands = {base.value()};
        address_add.immediate = operand.memory.displacement;
        address_add.source = source_location(instruction);
        return emit_value(std::move(address_add));
    }

    [[nodiscard]] Result<void> lift_memory(const DecodedInstruction& instruction, bool store)
    {
        if (instruction.operands.size() != 2U || instruction.operands[0].kind !=
                                                          aarch64::OperandKind::Register ||
            instruction.operands[1].kind != aarch64::OperandKind::Memory)
        {
            return Result<void>::failure(unsupported(instruction, "expected W/X register and memory operand"));
        }
        const auto& data_reg = instruction.operands[0].reg;
        if (data_reg.width != aarch64::RegisterWidth::W32 && data_reg.width != aarch64::RegisterWidth::X64)
        {
            return Result<void>::failure(unsupported(instruction, "memory data register must be W or X"));
        }
        const auto type = type_for_width(data_reg.width);
        const auto address = effective_address(instruction.operands[1], instruction);
        if (!address)
        {
            return Result<void>::failure(address.error());
        }
        const auto size = data_reg.width == aarch64::RegisterWidth::W32 ? 4U : 8U;
        if (store)
        {
            const auto value = read_register(data_reg, instruction);
            if (!value)
            {
                return Result<void>::failure(value.error());
            }
            ir::Instruction store_instruction;
            store_instruction.opcode = ir::Opcode::GuestStore;
            store_instruction.operands = {address.value(), value.value()};
            store_instruction.memory_size = static_cast<std::uint8_t>(size);
            store_instruction.source = source_location(instruction);
            return emit_void(std::move(store_instruction));
        }
        ir::Instruction load;
        load.opcode = ir::Opcode::GuestLoad;
        load.result_type = type;
        load.operands = {address.value()};
        load.memory_size = static_cast<std::uint8_t>(size);
        load.source = source_location(instruction);
        const auto value = emit_value(std::move(load));
        if (!value)
        {
            return Result<void>::failure(value.error());
        }
        return write_register(data_reg, value.value(), instruction);
    }

    [[nodiscard]] Result<ir::ValueId> condition_value(aarch64::ConditionCode condition,
                                                       const DecodedInstruction& instruction)
    {
        const auto n = read_flag(ir::Flag::N, instruction);
        const auto z = read_flag(ir::Flag::Z, instruction);
        const auto c = read_flag(ir::Flag::C, instruction);
        const auto v = read_flag(ir::Flag::V, instruction);
        if (!n || !z || !c || !v)
        {
            return Result<ir::ValueId>::failure(!n ? n.error() : !z ? z.error() : !c ? c.error() : v.error());
        }
        ir::Instruction evaluate;
        evaluate.opcode = ir::Opcode::EvaluateCondition;
        evaluate.result_type = ir::i1_type();
        evaluate.operands = {n.value(), z.value(), c.value(), v.value()};
        evaluate.condition = static_cast<ir::ConditionCode>(condition);
        evaluate.source = source_location(instruction);
        return emit_value(std::move(evaluate));
    }

    [[nodiscard]] Result<ir::ValueId> read_flag(ir::Flag flag, const DecodedInstruction& instruction)
    {
        ir::Instruction read;
        read.opcode = ir::Opcode::ReadFlag;
        read.result_type = ir::i1_type();
        read.flag = flag;
        read.source = source_location(instruction);
        return emit_value(std::move(read));
    }

    [[nodiscard]] Result<void> terminate(const analysis::BasicBlock& block,
                                         const DecodedInstruction& instruction)
    {
        const auto& flow = instruction.control_flow;
        ir::Terminator terminator;
        terminator.source = source_location(instruction);
        if (flow.kind == aarch64::ControlFlowKind::Return)
        {
            terminator.kind = ir::TerminatorKind::Return;
            return builder_.set_terminator(std::move(terminator));
        }
        if (flow.kind == aarch64::ControlFlowKind::DirectBranch)
        {
            const auto target = direct_target(block, analysis::EdgeKind::Branch);
            if (!target)
            {
                return Result<void>::failure(target.error());
            }
            terminator.kind = ir::TerminatorKind::Branch;
            terminator.target = target.value();
            return builder_.set_terminator(std::move(terminator));
        }
        if (flow.kind == aarch64::ControlFlowKind::ConditionalBranch)
        {
            const auto taken = direct_target(block, analysis::EdgeKind::ConditionalTaken);
            const auto not_taken = direct_target(block, analysis::EdgeKind::ConditionalNotTaken);
            if (!taken || !not_taken)
            {
                return Result<void>::failure(!taken ? taken.error() : not_taken.error());
            }
            ir::ValueId condition = ir::invalid_value;
            if (instruction.id == aarch64::InstructionId::BCond)
            {
                if (!flow.condition)
                {
                    return Result<void>::failure(unsupported(instruction, "conditional branch has no condition code"));
                }
                const auto value = condition_value(flow.condition.value(), instruction);
                if (!value)
                {
                    return Result<void>::failure(value.error());
                }
                condition = value.value();
            }
            else if (instruction.id == aarch64::InstructionId::Cbz ||
                     instruction.id == aarch64::InstructionId::Cbnz)
            {
                if (instruction.operands.empty() || instruction.operands[0].kind != aarch64::OperandKind::Register)
                {
                    return Result<void>::failure(unsupported(instruction, "compare-and-branch has no register operand"));
                }
                const auto type = type_for_width(instruction.operands[0].reg.width);
                const auto value = operand_value(instruction.operands[0], type, instruction);
                const auto zero = constant(type, 0U, instruction);
                if (!value || !zero)
                {
                    return Result<void>::failure(!value ? value.error() : zero.error());
                }
                const auto equal = binary(ir::Opcode::CompareEqual, value.value(), zero.value(), ir::i1_type(), instruction);
                if (!equal)
                {
                    return Result<void>::failure(equal.error());
                }
                if (instruction.id == aarch64::InstructionId::Cbnz)
                {
                    ir::Instruction invert;
                    invert.opcode = ir::Opcode::CompareNotEqual;
                    invert.result_type = ir::i1_type();
                    invert.operands = {value.value(), zero.value()};
                    invert.source = source_location(instruction);
                    const auto nonzero = emit_value(std::move(invert));
                    if (!nonzero)
                    {
                        return Result<void>::failure(nonzero.error());
                    }
                    condition = nonzero.value();
                }
                else
                {
                    condition = equal.value();
                }
            }
            else
            {
                return Result<void>::failure(unsupported(instruction, "conditional branch form is not supported"));
            }
            terminator.kind = ir::TerminatorKind::ConditionalBranch;
            terminator.condition = condition;
            terminator.target = taken.value();
            terminator.false_target = not_taken.value();
            return builder_.set_terminator(std::move(terminator));
        }
        if (flow.kind == aarch64::ControlFlowKind::Fallthrough)
        {
            if (block.successors.size() != 1U || !block.successors.front().internal)
            {
                return Result<void>::failure(make_error(
                    ErrorCode::InvalidControlFlow,
                    instruction_description(instruction) + " has no single internal fallthrough target"));
            }
            const auto target = block_ids_.find(block.successors.front().target);
            if (target == block_ids_.end())
            {
                return Result<void>::failure(make_error(
                    ErrorCode::InvalidControlFlow, "fallthrough target is missing from lifted CFG"));
            }
            terminator.kind = ir::TerminatorKind::Branch;
            terminator.target = target->second;
            return builder_.set_terminator(std::move(terminator));
        }
        return Result<void>::failure(unsupported(instruction, "control-flow form is outside Milestone 6"));
    }

    [[nodiscard]] Result<ir::BlockId> direct_target(const analysis::BasicBlock& block,
                                                    analysis::EdgeKind kind) const
    {
        for (const auto& edge : block.successors)
        {
            if (edge.kind == kind)
            {
                const auto found = block_ids_.find(edge.target);
                if (found == block_ids_.end() || !edge.internal)
                {
                    return Result<ir::BlockId>::failure(make_error(
                        ErrorCode::InvalidControlFlow, "branch target is not an internal lifted block"));
                }
                return Result<ir::BlockId>::success(found->second);
            }
        }
        return Result<ir::BlockId>::failure(
            make_error(ErrorCode::InvalidControlFlow, "required CFG edge is missing"));
    }

    [[nodiscard]] Result<void> lift_block(const analysis::BasicBlock& block)
    {
        if (block.instructions.empty())
        {
            return Result<void>::failure(
                make_error(ErrorCode::InvalidControlFlow, "cannot lift an empty basic block"));
        }
        for (std::size_t index = 0U; index < block.instructions.size(); ++index)
        {
            operations_for_instruction_ = 0U;
            const auto& instruction = block.instructions[index];
            ir::Instruction set_pc;
            set_pc.opcode = ir::Opcode::SetPc;
            set_pc.source = source_location(instruction);
            const auto pc = emit_void(std::move(set_pc));
            if (!pc)
            {
                return pc;
            }

            const bool last = index + 1U == block.instructions.size();
            if (last && instruction.control_flow.kind != aarch64::ControlFlowKind::Fallthrough)
            {
                switch (instruction.id)
                {
                case aarch64::InstructionId::B:
                case aarch64::InstructionId::BCond:
                case aarch64::InstructionId::Cbz:
                case aarch64::InstructionId::Cbnz:
                    break;
                case aarch64::InstructionId::Ret:
                    break;
                default:
                    if (instruction.control_flow.kind != aarch64::ControlFlowKind::Return)
                    {
                        return Result<void>::failure(unsupported(
                            instruction, "terminating instruction is not supported by the standalone lifter"));
                    }
                    break;
                }
                const auto end = terminate(block, instruction);
                if (!end)
                {
                    return end;
                }
                continue;
            }
            const auto lifted = lift_non_terminator(instruction);
            if (!lifted)
            {
                return lifted;
            }
        }
        const auto& last = block.instructions.back();
        if (last.control_flow.kind == aarch64::ControlFlowKind::Fallthrough)
        {
            return terminate(block, last);
        }
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> lift_non_terminator(const DecodedInstruction& instruction)
    {
        switch (instruction.id)
        {
        case aarch64::InstructionId::Nop:
            return emit_void(ir::Instruction{ir::Opcode::Nop, ir::invalid_value, ir::void_type(), {}, {},
                                             ir::Flag::N, ir::ConditionCode::Al, 0, 0, 0,
                                             source_location(instruction)});
        case aarch64::InstructionId::Mov:
            return lift_mov(instruction);
        case aarch64::InstructionId::Movz:
            return lift_movz(instruction, false);
        case aarch64::InstructionId::Movk:
            return lift_movz(instruction, true);
        case aarch64::InstructionId::Add:
            return lift_arithmetic(instruction, ir::Opcode::Add, false);
        case aarch64::InstructionId::Adds:
            return lift_arithmetic(instruction, ir::Opcode::Add, true);
        case aarch64::InstructionId::Sub:
            return lift_arithmetic(instruction, ir::Opcode::Sub, false);
        case aarch64::InstructionId::Subs:
            return lift_arithmetic(instruction, ir::Opcode::Sub, true);
        case aarch64::InstructionId::Cmp:
            return lift_cmp(instruction);
        case aarch64::InstructionId::And:
            return lift_logic(instruction, ir::Opcode::And);
        case aarch64::InstructionId::Orr:
            return lift_logic(instruction, ir::Opcode::Or);
        case aarch64::InstructionId::Eor:
            return lift_logic(instruction, ir::Opcode::Xor);
        case aarch64::InstructionId::Adr:
        case aarch64::InstructionId::Adrp:
            return lift_pc_relative(instruction);
        case aarch64::InstructionId::Ldr:
        case aarch64::InstructionId::Ldur:
            return lift_memory(instruction, false);
        case aarch64::InstructionId::Str:
        case aarch64::InstructionId::Stur:
            return lift_memory(instruction, true);
        default:
            return Result<void>::failure(unsupported(instruction, "instruction is not in the initial lifting subset"));
        }
    }

    const analysis::ControlFlowGraph& cfg_;
    LiftOptions options_;
    ir::Function function_;
    ir::Builder builder_;
    std::map<GuestAddress, ir::BlockId> block_ids_;
    std::size_t operations_for_instruction_ = 0U;
};

} // namespace

Result<ir::Function> lift_function(const analysis::ControlFlowGraph& cfg, const LiftOptions& options)
{
    try
    {
        return FunctionLifter(cfg, options).run();
    }
    catch (const std::bad_alloc&)
    {
        return Result<ir::Function>::failure(
            make_error(ErrorCode::ResourceLimit, "unable to allocate semantic IR for function"));
    }
}

} // namespace switchrecomp::lifter
