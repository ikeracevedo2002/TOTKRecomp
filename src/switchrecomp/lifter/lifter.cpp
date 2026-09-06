#include "switchrecomp/lifter/lifter.hpp"

#include "switchrecomp/aarch64/instruction.hpp"
#include "switchrecomp/common/checked_arithmetic.hpp"
#include "switchrecomp/ir/builder.hpp"
#include "switchrecomp/ir/verifier.hpp"

#include <bit>
#include <iomanip>
#include <limits>
#include <map>
#include <new>
#include <optional>
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

[[nodiscard]] ir::VectorArrangement ir_arrangement(aarch64::VectorArrangement arrangement) noexcept
{
    switch (arrangement)
    {
    case aarch64::VectorArrangement::B8: return ir::VectorArrangement::B8;
    case aarch64::VectorArrangement::B16: return ir::VectorArrangement::B16;
    case aarch64::VectorArrangement::H4: return ir::VectorArrangement::H4;
    case aarch64::VectorArrangement::H8: return ir::VectorArrangement::H8;
    case aarch64::VectorArrangement::S2: return ir::VectorArrangement::S2;
    case aarch64::VectorArrangement::S4: return ir::VectorArrangement::S4;
    case aarch64::VectorArrangement::D1: return ir::VectorArrangement::D1;
    case aarch64::VectorArrangement::D2: return ir::VectorArrangement::D2;
    case aarch64::VectorArrangement::Q1: return ir::VectorArrangement::Raw128;
    case aarch64::VectorArrangement::Invalid: return ir::VectorArrangement::Raw128;
    }
    return ir::VectorArrangement::Raw128;
}

[[nodiscard]] ir::VectorArrangement scalar_arrangement(aarch64::RegisterWidth width) noexcept
{
    return width == aarch64::RegisterWidth::S32 ? ir::VectorArrangement::S2
                                                : ir::VectorArrangement::D2;
}

[[nodiscard]] bool is_vector_register(const aarch64::Register& reg) noexcept
{
    return reg.kind == aarch64::RegisterKind::Vector && reg.index < 32U &&
           (reg.width == aarch64::RegisterWidth::B8 || reg.width == aarch64::RegisterWidth::H16 ||
            reg.width == aarch64::RegisterWidth::S32 || reg.width == aarch64::RegisterWidth::D64 ||
            reg.width == aarch64::RegisterWidth::Q128);
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

    [[nodiscard]] Result<ir::ValueId> unary(ir::Opcode opcode, ir::ValueId value, ir::Type type,
                                            const DecodedInstruction& instruction)
    {
        return emit_value(ir::Instruction{opcode, ir::invalid_value, type, {value}, {},
                                          ir::Flag::N, ir::ConditionCode::Al, 0, 0, 0,
                                          source_location(instruction)});
    }

    [[nodiscard]] Result<ir::ValueId> cast(ir::Opcode opcode, ir::ValueId value, ir::Type type,
                                           const DecodedInstruction& instruction)
    {
        ir::Instruction instruction_ir;
        instruction_ir.opcode = opcode;
        instruction_ir.result_type = type;
        instruction_ir.operands = {value};
        instruction_ir.source = source_location(instruction);
        return emit_value(std::move(instruction_ir));
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
                                      : operand.shift_kind == aarch64::ShiftKind::Asr
                                            ? ir::Opcode::ArithmeticShiftRight
                                            : ir::Opcode::RotateRight;
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
                if (operand.extension == aarch64::ExtensionKind::None || actual_type != ir::i32_type() ||
                    type != ir::i64_type())
                {
                    return Result<ir::ValueId>::failure(
                        unsupported(instruction, "register operand width does not match destination"));
                }
            }
            auto extended = value.value();
            if (actual_type != type)
            {
                const auto extension = operand.extension;
                const bool sign = extension == aarch64::ExtensionKind::Sxtb ||
                                  extension == aarch64::ExtensionKind::Sxth ||
                                  extension == aarch64::ExtensionKind::Sxtw ||
                                  extension == aarch64::ExtensionKind::Sxtx;
                unsigned int source_width = 32U;
                switch (extension)
                {
                case aarch64::ExtensionKind::Uxtb:
                case aarch64::ExtensionKind::Sxtb: source_width = 8U; break;
                case aarch64::ExtensionKind::Uxth:
                case aarch64::ExtensionKind::Sxth: source_width = 16U; break;
                case aarch64::ExtensionKind::Uxtw:
                case aarch64::ExtensionKind::Sxtw: source_width = 32U; break;
                case aarch64::ExtensionKind::Uxtx:
                case aarch64::ExtensionKind::Sxtx: source_width = 64U; break;
                case aarch64::ExtensionKind::None: break;
                }
                if (source_width < actual_type.bit_width())
                {
                    const auto narrow = source_width == 8U ? ir::i8_type()
                                      : source_width == 16U ? ir::i16_type() : ir::i32_type();
                    const auto truncated = cast(ir::Opcode::Truncate, extended, narrow, instruction);
                    if (!truncated)
                    {
                        return truncated;
                    }
                    extended = truncated.value();
                }
                const auto widened = cast(sign ? ir::Opcode::SignExtend : ir::Opcode::ZeroExtend,
                                          extended, type, instruction);
                if (!widened)
                {
                    return widened;
                }
                extended = widened.value();
            }
            return apply_shift(extended, type, operand, instruction);
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

    [[nodiscard]] Result<ir::VectorArrangement> vector_arrangement(
        const aarch64::Operand& operand, const DecodedInstruction& instruction,
        bool allow_raw = false)
    {
        if (operand.arrangement != aarch64::VectorArrangement::Invalid)
        {
            const auto result = ir_arrangement(operand.arrangement);
            if (result == ir::VectorArrangement::Raw128 && !allow_raw)
            {
                return Result<ir::VectorArrangement>::failure(
                    unsupported(instruction, "the vector arrangement is not lane-addressable"));
            }
            return Result<ir::VectorArrangement>::success(result);
        }
        if (operand.kind == aarch64::OperandKind::Register && operand.reg.kind == aarch64::RegisterKind::Vector)
        {
            switch (operand.reg.width)
            {
            case aarch64::RegisterWidth::B8: return Result<ir::VectorArrangement>::success(ir::VectorArrangement::B8);
            case aarch64::RegisterWidth::H16: return Result<ir::VectorArrangement>::success(ir::VectorArrangement::B16);
            case aarch64::RegisterWidth::S32: return Result<ir::VectorArrangement>::success(ir::VectorArrangement::S2);
            case aarch64::RegisterWidth::D64: return Result<ir::VectorArrangement>::success(ir::VectorArrangement::D2);
            case aarch64::RegisterWidth::Q128:
                if (allow_raw) return Result<ir::VectorArrangement>::success(ir::VectorArrangement::Raw128);
                break;
            default: break;
            }
        }
        return Result<ir::VectorArrangement>::failure(
            unsupported(instruction, "vector operand has no supported arrangement"));
    }

    [[nodiscard]] Result<ir::ValueId> read_vector(const aarch64::Register& reg,
                                                  const DecodedInstruction& instruction)
    {
        if (!is_vector_register(reg))
        {
            return Result<ir::ValueId>::failure(
                unsupported(instruction, "expected a V/B/H/S/D/Q register"));
        }
        ir::Instruction read;
        read.opcode = ir::Opcode::ReadVectorRegister;
        read.result_type = ir::v128_type();
        read.vector_index = reg.index;
        read.source = source_location(instruction);
        return emit_value(std::move(read));
    }

    [[nodiscard]] Result<void> write_vector(const aarch64::Register& reg, ir::ValueId value,
                                             const DecodedInstruction& instruction)
    {
        if (!is_vector_register(reg))
        {
            return Result<void>::failure(
                unsupported(instruction, "expected a V/B/H/S/D/Q register"));
        }
        ir::Instruction write;
        write.opcode = ir::Opcode::WriteVectorRegister;
        write.result_type = ir::void_type();
        write.operands = {value};
        write.vector_index = reg.index;
        write.source = source_location(instruction);
        return emit_void(std::move(write));
    }

    [[nodiscard]] Result<ir::ValueId> bitcast(ir::ValueId value, ir::Type result_type,
                                              const DecodedInstruction& instruction)
    {
        ir::Instruction cast_instruction;
        cast_instruction.opcode = ir::Opcode::BitCast;
        cast_instruction.result_type = result_type;
        cast_instruction.operands = {value};
        cast_instruction.source = source_location(instruction);
        return emit_value(std::move(cast_instruction));
    }

    [[nodiscard]] Result<ir::ValueId> vector_extract(ir::ValueId vector,
                                                     ir::VectorArrangement arrangement,
                                                     std::uint8_t lane,
                                                     const DecodedInstruction& instruction)
    {
        const auto bits = ir::Type(arrangement == ir::VectorArrangement::B8 ? ir::TypeKind::I8
                              : arrangement == ir::VectorArrangement::B16 ? ir::TypeKind::I8
                              : arrangement == ir::VectorArrangement::H4 ? ir::TypeKind::I16
                              : arrangement == ir::VectorArrangement::H8 ? ir::TypeKind::I16
                              : arrangement == ir::VectorArrangement::S2 ? ir::TypeKind::I32
                              : arrangement == ir::VectorArrangement::S4 ? ir::TypeKind::I32
                              : arrangement == ir::VectorArrangement::D1 ? ir::TypeKind::I64
                              : arrangement == ir::VectorArrangement::D2 ? ir::TypeKind::I64
                                                                           : ir::TypeKind::Void);
        if (bits.is_void())
        {
            return Result<ir::ValueId>::failure(
                unsupported(instruction, "cannot extract a lane from a raw 128-bit arrangement"));
        }
        ir::Instruction extract;
        extract.opcode = ir::Opcode::VectorExtractLane;
        extract.result_type = bits;
        extract.operands = {vector};
        extract.arrangement = arrangement;
        extract.lane_index = lane;
        extract.source = source_location(instruction);
        return emit_value(std::move(extract));
    }

    [[nodiscard]] Result<ir::ValueId> zero_vector(const DecodedInstruction& instruction)
    {
        ir::Instruction zero;
        zero.opcode = ir::Opcode::Constant;
        zero.result_type = ir::v128_type();
        zero.constant = 0U;
        zero.constant_high = 0U;
        zero.source = source_location(instruction);
        return emit_value(std::move(zero));
    }

    [[nodiscard]] Result<ir::ValueId> scalar_operand(const aarch64::Operand& operand,
                                                      ir::Type fp_type,
                                                      const DecodedInstruction& instruction)
    {
        if (operand.kind == aarch64::OperandKind::Register && operand.reg.kind == aarch64::RegisterKind::Vector)
        {
            if ((fp_type == ir::f32_type() && operand.reg.width != aarch64::RegisterWidth::S32) ||
                (fp_type == ir::f64_type() && operand.reg.width != aarch64::RegisterWidth::D64))
            {
                return Result<ir::ValueId>::failure(
                    unsupported(instruction, "scalar FP operand width does not match"));
            }
            const auto vector = read_vector(operand.reg, instruction);
            const auto arrangement = scalar_arrangement(operand.reg.width);
            const auto lane = vector ? vector_extract(vector.value(), arrangement, 0U, instruction)
                                     : Result<ir::ValueId>::failure(vector.error());
            if (!lane)
            {
                return Result<ir::ValueId>::failure(lane.error());
            }
            return bitcast(lane.value(), fp_type, instruction);
        }
        if (operand.kind == aarch64::OperandKind::FloatingImmediate ||
            (operand.kind == aarch64::OperandKind::Immediate && operand.immediate == 0))
        {
            const auto raw = fp_type == ir::f32_type()
                                 ? static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(
                                       static_cast<float>(operand.kind == aarch64::OperandKind::FloatingImmediate
                                                              ? operand.floating_immediate
                                                              : 0.0)))
                                 : std::bit_cast<std::uint64_t>(operand.kind == aarch64::OperandKind::FloatingImmediate
                                                                    ? operand.floating_immediate
                                                                    : 0.0);
            return constant(fp_type, raw, instruction);
        }
        return Result<ir::ValueId>::failure(
            unsupported(instruction, "scalar FP operand is not a register or immediate"));
    }

    [[nodiscard]] Result<ir::ValueId> scalar_read(const aarch64::Register& reg,
                                                  const DecodedInstruction& instruction)
    {
        const auto fp_type = reg.width == aarch64::RegisterWidth::S32
                                 ? ir::f32_type()
                                 : reg.width == aarch64::RegisterWidth::D64 ? ir::f64_type() : ir::void_type();
        if (fp_type.is_void())
        {
            return Result<ir::ValueId>::failure(
                unsupported(instruction, "expected an S or D scalar FP register"));
        }
        aarch64::Operand operand;
        operand.kind = aarch64::OperandKind::Register;
        operand.reg = reg;
        return scalar_operand(operand, fp_type, instruction);
    }

    [[nodiscard]] Result<void> scalar_write(const aarch64::Register& reg, ir::ValueId value,
                                             const DecodedInstruction& instruction)
    {
        const auto fp_type = reg.width == aarch64::RegisterWidth::S32
                                 ? ir::f32_type()
                                 : reg.width == aarch64::RegisterWidth::D64 ? ir::f64_type() : ir::void_type();
        if (fp_type.is_void())
        {
            return Result<void>::failure(
                unsupported(instruction, "expected an S or D scalar FP register"));
        }
        const auto raw = bitcast(value, fp_type == ir::f32_type() ? ir::i32_type() : ir::i64_type(), instruction);
        const auto zero = raw ? zero_vector(instruction) : Result<ir::ValueId>::failure(raw.error());
        if (!raw || !zero)
        {
            return Result<void>::failure(!raw ? raw.error() : zero.error());
        }
        ir::Instruction insert;
        insert.opcode = ir::Opcode::VectorInsertLane;
        insert.result_type = ir::v128_type();
        insert.operands = {zero.value(), raw.value()};
        insert.arrangement = reg.width == aarch64::RegisterWidth::S32 ? ir::VectorArrangement::S2
                                                                       : ir::VectorArrangement::D2;
        insert.lane_index = 0U;
        insert.source = source_location(instruction);
        const auto vector = emit_value(std::move(insert));
        if (!vector)
        {
            return Result<void>::failure(vector.error());
        }
        return write_vector(reg, vector.value(), instruction);
    }

    [[nodiscard]] Result<ir::ValueId> emit_fp_binary(ir::ValueId left, ir::ValueId right,
                                                     ir::Type type, ir::FpBinaryOperation operation,
                                                     const DecodedInstruction& instruction)
    {
        ir::Instruction fp;
        fp.opcode = ir::Opcode::FpBinary;
        fp.result_type = type;
        fp.operands = {left, right};
        fp.fp_binary = operation;
        fp.source = source_location(instruction);
        return emit_value(std::move(fp));
    }

    [[nodiscard]] Result<ir::ValueId> emit_fp_unary(ir::ValueId value, ir::Type type,
                                                    ir::FpUnaryOperation operation,
                                                    const DecodedInstruction& instruction)
    {
        ir::Instruction fp;
        fp.opcode = ir::Opcode::FpUnary;
        fp.result_type = type;
        fp.operands = {value};
        fp.fp_unary = operation;
        fp.source = source_location(instruction);
        return emit_value(std::move(fp));
    }

    [[nodiscard]] Result<ir::ValueId> emit_fp_convert(ir::ValueId value, ir::Type result_type,
                                                      ir::FpConversion conversion,
                                                      const DecodedInstruction& instruction)
    {
        ir::Instruction fp;
        fp.opcode = ir::Opcode::FpConvert;
        fp.result_type = result_type;
        fp.operands = {value};
        fp.fp_conversion = conversion;
        fp.source = source_location(instruction);
        return emit_value(std::move(fp));
    }

    [[nodiscard]] Result<ir::ValueId> emit_fp_round(ir::ValueId value, ir::Type type,
                                                     ir::RoundingMode mode,
                                                     const DecodedInstruction& instruction)
    {
        ir::Instruction fp;
        fp.opcode = ir::Opcode::FpRound;
        fp.result_type = type;
        fp.operands = {value};
        fp.rounding_mode = mode;
        fp.source = source_location(instruction);
        return emit_value(std::move(fp));
    }

    [[nodiscard]] Result<ir::ValueId> vector_lane_operand(const aarch64::Operand& operand,
                                                          ir::VectorArrangement destination,
                                                          const DecodedInstruction& instruction)
    {
        if (operand.kind == aarch64::OperandKind::Register && operand.reg.kind == aarch64::RegisterKind::General)
        {
            const auto source = read_register(operand.reg, instruction);
            if (!source)
            {
                return source;
            }
            const auto bits = destination == ir::VectorArrangement::B8 || destination == ir::VectorArrangement::B16
                                  ? 8U
                                  : destination == ir::VectorArrangement::H4 || destination == ir::VectorArrangement::H8
                                        ? 16U
                                        : destination == ir::VectorArrangement::S2 || destination == ir::VectorArrangement::S4
                                              ? 32U
                                              : 64U;
            const auto lane_type = bits == 8U ? ir::i8_type() : bits == 16U ? ir::i16_type()
                                             : bits == 32U ? ir::i32_type() : ir::i64_type();
            if (source.value() != ir::invalid_value)
            {
                const auto source_type = type_for_width(operand.reg.width);
                if (source_type == lane_type)
                {
                    return source;
                }
                if (source_type.bit_width() > lane_type.bit_width())
                {
                    return cast(ir::Opcode::Truncate, source.value(), lane_type, instruction);
                }
                return cast(ir::Opcode::ZeroExtend, source.value(), lane_type, instruction);
            }
        }
        if (operand.kind == aarch64::OperandKind::Register && operand.reg.kind == aarch64::RegisterKind::Vector)
        {
            const auto source_vector = read_vector(operand.reg, instruction);
            if (!source_vector)
            {
                return source_vector;
            }
            const auto source_arrangement = vector_arrangement(operand, instruction);
            if (!source_arrangement)
            {
                return Result<ir::ValueId>::failure(source_arrangement.error());
            }
            return vector_extract(source_vector.value(), source_arrangement.value(),
                                  operand.vector_index < 0 ? 0U : static_cast<std::uint8_t>(operand.vector_index),
                                  instruction);
        }
        return Result<ir::ValueId>::failure(
            unsupported(instruction, "vector lane source must be a general or vector register"));
    }

    [[nodiscard]] Result<void> lift_vector_common(const DecodedInstruction& instruction)
    {
        if (instruction.operands.size() < 3U || instruction.operands[0].kind != aarch64::OperandKind::Register ||
            instruction.operands[0].reg.kind != aarch64::RegisterKind::Vector ||
            instruction.operands[1].kind != aarch64::OperandKind::Register ||
            instruction.operands[2].kind != aarch64::OperandKind::Register)
        {
            return Result<void>::failure(unsupported(instruction, "expected a vector destination and two vector operands"));
        }
        const auto arrangement = vector_arrangement(instruction.operands[0], instruction);
        if (!arrangement || arrangement.value() == ir::VectorArrangement::Raw128)
        {
            return Result<void>::failure(!arrangement ? arrangement.error()
                                                      : unsupported(instruction, "vector operation requires an arrangement"));
        }
        const auto left = read_vector(instruction.operands[1].reg, instruction);
        const auto right = read_vector(instruction.operands[2].reg, instruction);
        if (!left || !right)
        {
            return Result<void>::failure(!left ? left.error() : right.error());
        }
        ir::VectorOperation operation = ir::VectorOperation::Add;
        switch (instruction.simd_operation)
        {
        case aarch64::SimdOperation::Fadd: operation = ir::VectorOperation::FpAdd; break;
        case aarch64::SimdOperation::Fsub: operation = ir::VectorOperation::FpSub; break;
        case aarch64::SimdOperation::Fmul: operation = ir::VectorOperation::FpMul; break;
        case aarch64::SimdOperation::Fdiv: operation = ir::VectorOperation::FpDiv; break;
        default:
            switch (instruction.id)
            {
            case aarch64::InstructionId::And: case aarch64::InstructionId::Ands: operation = ir::VectorOperation::And; break;
            case aarch64::InstructionId::Orr: operation = ir::VectorOperation::Or; break;
            case aarch64::InstructionId::Eor: operation = ir::VectorOperation::Xor; break;
            case aarch64::InstructionId::Bic: case aarch64::InstructionId::Bics: operation = ir::VectorOperation::Bic; break;
            case aarch64::InstructionId::Add: operation = ir::VectorOperation::Add; break;
            case aarch64::InstructionId::Sub: operation = ir::VectorOperation::Sub; break;
            case aarch64::InstructionId::Mul: operation = ir::VectorOperation::Mul; break;
            default:
                return Result<void>::failure(unsupported(instruction, "SIMD operation is not a supported vector binary"));
            }
            break;
        }
        ir::Instruction vector;
        vector.opcode = ir::Opcode::VectorBinary;
        vector.result_type = ir::v128_type();
        vector.operands = {left.value(), right.value()};
        vector.arrangement = arrangement.value();
        vector.vector_operation = operation;
        vector.source = source_location(instruction);
        const auto result = emit_value(std::move(vector));
        if (!result)
        {
            return Result<void>::failure(result.error());
        }
        return write_vector(instruction.operands[0].reg, result.value(), instruction);
    }

    [[nodiscard]] Result<void> lift_fp_simd(const DecodedInstruction& instruction)
    {
        using Op = aarch64::SimdOperation;
        const auto op = instruction.simd_operation;
        if (op == Op::None)
        {
            return Result<void>::failure(unsupported(instruction, "decoder did not normalize the FP/SIMD opcode"));
        }

        const bool destination_is_vector = !instruction.operands.empty() &&
                                           instruction.operands[0].kind == aarch64::OperandKind::Register &&
                                           instruction.operands[0].reg.kind == aarch64::RegisterKind::Vector;
        const auto scalar_type_for_destination = [&]() -> ir::Type {
            if (!destination_is_vector) return ir::void_type();
            return instruction.operands[0].reg.width == aarch64::RegisterWidth::S32
                       ? ir::f32_type()
                       : instruction.operands[0].reg.width == aarch64::RegisterWidth::D64 ? ir::f64_type()
                                                                                            : ir::void_type();
        };
        const auto scalar_binary = [&](ir::FpBinaryOperation operation) -> Result<void> {
            if (instruction.operands.size() != 3U || !destination_is_vector)
            {
                return Result<void>::failure(unsupported(instruction, "scalar FP binary requires S/D destination and two operands"));
            }
            const auto type = scalar_type_for_destination();
            if (type.is_void()) return Result<void>::failure(unsupported(instruction, "scalar FP binary requires S or D destination"));
            const auto left = scalar_operand(instruction.operands[1], type, instruction);
            const auto right = scalar_operand(instruction.operands[2], type, instruction);
            if (!left || !right) return Result<void>::failure(!left ? left.error() : right.error());
            const auto result = emit_fp_binary(left.value(), right.value(), type, operation, instruction);
            return result ? scalar_write(instruction.operands[0].reg, result.value(), instruction)
                          : Result<void>::failure(result.error());
        };
        const auto scalar_unary = [&](ir::FpUnaryOperation operation) -> Result<void> {
            if (instruction.operands.size() != 2U || !destination_is_vector)
            {
                return Result<void>::failure(unsupported(instruction, "scalar FP unary requires S/D destination and one operand"));
            }
            const auto type = scalar_type_for_destination();
            if (type.is_void()) return Result<void>::failure(unsupported(instruction, "scalar FP unary requires S or D destination"));
            const auto source = scalar_operand(instruction.operands[1], type, instruction);
            if (!source) return Result<void>::failure(source.error());
            const auto result = emit_fp_unary(source.value(), type, operation, instruction);
            return result ? scalar_write(instruction.operands[0].reg, result.value(), instruction)
                          : Result<void>::failure(result.error());
        };

        switch (op)
        {
        case Op::Fadd: case Op::Fsub: case Op::Fmul: case Op::Fdiv:
        {
            if (destination_is_vector && instruction.operands[0].reg.width == aarch64::RegisterWidth::Q128)
                return lift_vector_common(instruction);
            return scalar_binary(op == Op::Fadd ? ir::FpBinaryOperation::Add
                               : op == Op::Fsub ? ir::FpBinaryOperation::Sub
                               : op == Op::Fmul ? ir::FpBinaryOperation::Mul : ir::FpBinaryOperation::Div);
        }
        case Op::Fmin: return scalar_binary(ir::FpBinaryOperation::Min);
        case Op::Fmax: return scalar_binary(ir::FpBinaryOperation::Max);
        case Op::Fneg: return scalar_unary(ir::FpUnaryOperation::Neg);
        case Op::Fabs: return scalar_unary(ir::FpUnaryOperation::Abs);
        case Op::Fsqrt: return scalar_unary(ir::FpUnaryOperation::Sqrt);
        case Op::Fcmp: case Op::Fcmpe:
        {
            if (instruction.operands.size() != 2U || !destination_is_vector)
                return Result<void>::failure(unsupported(instruction, "FCMP requires two scalar FP operands"));
            const auto type = instruction.operands[0].reg.width == aarch64::RegisterWidth::S32 ? ir::f32_type() : ir::f64_type();
            const auto left = scalar_operand(instruction.operands[0], type, instruction);
            const auto right = scalar_operand(instruction.operands[1], type, instruction);
            if (!left || !right) return Result<void>::failure(!left ? left.error() : right.error());
            ir::Instruction compare;
            compare.opcode = ir::Opcode::FpCompare;
            compare.result_type = ir::i32_type();
            compare.operands = {left.value(), right.value()};
            compare.signaling = op == Op::Fcmpe;
            compare.source = source_location(instruction);
            const auto flags = emit_value(std::move(compare));
            if (!flags) return Result<void>::failure(flags.error());
            for (const auto& [flag, shift] : {std::pair{ir::Flag::N, 3U}, std::pair{ir::Flag::Z, 2U},
                                               std::pair{ir::Flag::C, 1U}, std::pair{ir::Flag::V, 0U}})
            {
                const auto amount = constant(ir::i32_type(), shift, instruction);
                if (!amount) return Result<void>::failure(amount.error());
                const auto shifted = binary(ir::Opcode::LogicalShiftRight, flags.value(), amount.value(), ir::i32_type(), instruction);
                if (!shifted) return Result<void>::failure(shifted.error());
                const auto bit = cast(ir::Opcode::Truncate, shifted.value(), ir::i1_type(), instruction);
                if (!bit) return Result<void>::failure(bit.error());
                ir::Instruction write;
                write.opcode = ir::Opcode::WriteFlag;
                write.result_type = ir::void_type();
                write.operands = {bit.value()};
                write.flag = flag;
                write.source = source_location(instruction);
                const auto emitted = emit_void(std::move(write));
                if (!emitted) return emitted;
            }
            return Result<void>::success();
        }
        case Op::Fcsel:
        {
            if (instruction.operands.size() != 3U || !destination_is_vector)
                return Result<void>::failure(unsupported(instruction, "FCSEL requires destination, two sources, and a condition"));
            const auto type = scalar_type_for_destination();
            const auto condition = condition_value(instruction.condition.value_or(aarch64::ConditionCode::Al), instruction);
            const auto left = scalar_operand(instruction.operands[1], type, instruction);
            const auto right = scalar_operand(instruction.operands[2], type, instruction);
            if (!condition || !left || !right) return Result<void>::failure(!condition ? condition.error() : !left ? left.error() : right.error());
            ir::Instruction select;
            select.opcode = ir::Opcode::Select;
            select.result_type = type;
            select.operands = {condition.value(), left.value(), right.value()};
            select.source = source_location(instruction);
            const auto result = emit_value(std::move(select));
            return result ? scalar_write(instruction.operands[0].reg, result.value(), instruction)
                          : Result<void>::failure(result.error());
        }
        case Op::Scvtf: case Op::Ucvtf: case Op::Fcvtzs: case Op::Fcvtzu:
        {
            if (instruction.operands.size() != 2U || instruction.operands[0].kind != aarch64::OperandKind::Register ||
                instruction.operands[1].kind != aarch64::OperandKind::Register)
                return Result<void>::failure(unsupported(instruction, "FP/integer conversion requires two registers"));
            const auto& destination = instruction.operands[0].reg;
            const auto& source_reg = instruction.operands[1].reg;
            const bool to_fp = op == Op::Scvtf || op == Op::Ucvtf;
            const auto result_type = to_fp ? (destination.width == aarch64::RegisterWidth::S32 ? ir::f32_type() : ir::f64_type())
                                           : type_for_width(destination.width);
            const auto source = to_fp ? read_register(source_reg, instruction) : scalar_read(source_reg, instruction);
            if (!source) return Result<void>::failure(source.error());
            const auto conversion = op == Op::Scvtf ? ir::FpConversion::SignedIntToFp
                                  : op == Op::Ucvtf ? ir::FpConversion::UnsignedIntToFp
                                  : op == Op::Fcvtzs ? ir::FpConversion::FpToSignedIntTowardZero
                                                     : ir::FpConversion::FpToUnsignedIntTowardZero;
            const auto result = emit_fp_convert(source.value(), result_type, conversion, instruction);
            if (!result) return Result<void>::failure(result.error());
            return to_fp ? scalar_write(destination, result.value(), instruction)
                         : write_register(destination, result.value(), instruction);
        }
        case Op::Fcvt:
        {
            if (instruction.operands.size() != 2U || !destination_is_vector ||
                instruction.operands[1].kind != aarch64::OperandKind::Register)
                return Result<void>::failure(unsupported(instruction, "FCVT requires two scalar FP registers"));
            const auto destination_type = scalar_type_for_destination();
            const auto source = scalar_read(instruction.operands[1].reg, instruction);
            if (!source) return Result<void>::failure(source.error());
            const auto conversion = destination_type == ir::f64_type() ? ir::FpConversion::Fp32ToFp64
                                                                          : ir::FpConversion::Fp64ToFp32;
            const auto result = emit_fp_convert(source.value(), destination_type, conversion, instruction);
            return result ? scalar_write(instruction.operands[0].reg, result.value(), instruction)
                          : Result<void>::failure(result.error());
        }
        case Op::Frintn: case Op::Frintp: case Op::Frintm: case Op::Frintz:
        {
            if (instruction.operands.size() != 2U || !destination_is_vector)
                return Result<void>::failure(unsupported(instruction, "FRINT requires scalar FP operands"));
            const auto type = scalar_type_for_destination();
            const auto source = scalar_operand(instruction.operands[1], type, instruction);
            if (!source) return Result<void>::failure(source.error());
            const auto mode = op == Op::Frintn ? ir::RoundingMode::NearestEven
                              : op == Op::Frintp ? ir::RoundingMode::PlusInfinity
                              : op == Op::Frintm ? ir::RoundingMode::MinusInfinity
                                                 : ir::RoundingMode::TowardZero;
            const auto result = emit_fp_round(source.value(), type, mode, instruction);
            return result ? scalar_write(instruction.operands[0].reg, result.value(), instruction)
                          : Result<void>::failure(result.error());
        }
        case Op::Fmov:
        {
            if (instruction.operands.size() != 2U || instruction.operands[0].kind != aarch64::OperandKind::Register)
                return Result<void>::failure(unsupported(instruction, "FMOV requires destination and source"));
            const auto& destination = instruction.operands[0].reg;
            const auto& source = instruction.operands[1];
            if (destination.kind == aarch64::RegisterKind::Vector)
            {
                if (source.kind == aarch64::OperandKind::Register && source.reg.kind == aarch64::RegisterKind::Vector)
                {
                    if (destination.width == aarch64::RegisterWidth::Q128 || source.reg.width == aarch64::RegisterWidth::Q128)
                    {
                        const auto value = read_vector(source.reg, instruction);
                        return value ? write_vector(destination, value.value(), instruction)
                                     : Result<void>::failure(value.error());
                    }
                    const auto value = scalar_read(source.reg, instruction);
                    return value ? scalar_write(destination, value.value(), instruction)
                                 : Result<void>::failure(value.error());
                }
                const auto type = destination.width == aarch64::RegisterWidth::S32 ? ir::f32_type()
                                                                                    : ir::f64_type();
                ir::ValueId value = ir::invalid_value;
                if (source.kind == aarch64::OperandKind::Register && source.reg.kind == aarch64::RegisterKind::General)
                {
                    const auto raw = read_register(source.reg, instruction);
                    const auto narrowed = raw && source.reg.width == aarch64::RegisterWidth::X64 && type == ir::f32_type()
                                              ? cast(ir::Opcode::Truncate, raw.value(), ir::i32_type(), instruction)
                                              : raw;
                    if (!narrowed) return Result<void>::failure(narrowed.error());
                    const auto converted = bitcast(narrowed.value(), type, instruction);
                    if (!converted) return Result<void>::failure(converted.error());
                    value = converted.value();
                }
                else
                {
                    const auto immediate = scalar_operand(source, type, instruction);
                    if (!immediate) return Result<void>::failure(immediate.error());
                    value = immediate.value();
                }
                return scalar_write(destination, value, instruction);
            }
            if (destination.kind == aarch64::RegisterKind::General &&
                source.kind == aarch64::OperandKind::Register && source.reg.kind == aarch64::RegisterKind::Vector)
            {
                const auto scalar = scalar_read(source.reg, instruction);
                if (!scalar) return Result<void>::failure(scalar.error());
                const auto raw = bitcast(scalar.value(), destination.width == aarch64::RegisterWidth::W32 ? ir::i32_type() : ir::i64_type(), instruction);
                return raw ? write_register(destination, raw.value(), instruction) : Result<void>::failure(raw.error());
            }
            return Result<void>::failure(unsupported(instruction, "FMOV operand form is not supported"));
        }
        case Op::Dup:
        {
            if (instruction.operands.size() != 2U || !destination_is_vector)
                return Result<void>::failure(unsupported(instruction, "DUP requires vector destination and source"));
            const auto arrangement = vector_arrangement(instruction.operands[0], instruction);
            if (!arrangement || arrangement.value() == ir::VectorArrangement::Raw128)
                return Result<void>::failure(!arrangement ? arrangement.error() : unsupported(instruction, "DUP requires a lane arrangement"));
            const auto lane = vector_lane_operand(instruction.operands[1], arrangement.value(), instruction);
            if (!lane) return Result<void>::failure(lane.error());
            ir::Instruction broadcast;
            broadcast.opcode = ir::Opcode::VectorBroadcast;
            broadcast.result_type = ir::v128_type();
            broadcast.operands = {lane.value()};
            broadcast.arrangement = arrangement.value();
            broadcast.source = source_location(instruction);
            const auto result = emit_value(std::move(broadcast));
            return result ? write_vector(instruction.operands[0].reg, result.value(), instruction)
                          : Result<void>::failure(result.error());
        }
        case Op::Ins:
        {
            if (instruction.operands.size() != 2U || !destination_is_vector)
                return Result<void>::failure(unsupported(instruction, "INS requires vector destination and source"));
            const auto arrangement = vector_arrangement(instruction.operands[0], instruction);
            const auto destination = read_vector(instruction.operands[0].reg, instruction);
            const auto lane = arrangement ? vector_lane_operand(instruction.operands[1], arrangement.value(), instruction)
                                           : Result<ir::ValueId>::failure(arrangement.error());
            if (!arrangement || !destination || !lane)
                return Result<void>::failure(!arrangement ? arrangement.error() : !destination ? destination.error() : lane.error());
            ir::Instruction insert;
            insert.opcode = ir::Opcode::VectorInsertLane;
            insert.result_type = ir::v128_type();
            insert.operands = {destination.value(), lane.value()};
            insert.arrangement = arrangement.value();
            insert.lane_index = instruction.operands[0].vector_index < 0 ? 0U : static_cast<std::uint8_t>(instruction.operands[0].vector_index);
            insert.source = source_location(instruction);
            const auto result = emit_value(std::move(insert));
            return result ? write_vector(instruction.operands[0].reg, result.value(), instruction)
                          : Result<void>::failure(result.error());
        }
        case Op::Umov: case Op::Smov:
        {
            if (instruction.operands.size() != 2U || instruction.operands[0].reg.kind != aarch64::RegisterKind::General ||
                instruction.operands[1].kind != aarch64::OperandKind::Register)
                return Result<void>::failure(unsupported(instruction, "UMOV/SMOV requires general destination and vector source"));
            const auto source_vector = read_vector(instruction.operands[1].reg, instruction);
            const auto arrangement = vector_arrangement(instruction.operands[1], instruction);
            if (!source_vector || !arrangement) return Result<void>::failure(!source_vector ? source_vector.error() : arrangement.error());
            const auto lane = vector_extract(source_vector.value(), arrangement.value(),
                                             instruction.operands[1].vector_index < 0 ? 0U : static_cast<std::uint8_t>(instruction.operands[1].vector_index), instruction);
            if (!lane) return Result<void>::failure(lane.error());
            const auto lane_width = lane.value() == ir::invalid_value ? 0U : arrangement.value() == ir::VectorArrangement::B8 || arrangement.value() == ir::VectorArrangement::B16 ? 8U : arrangement.value() == ir::VectorArrangement::H4 || arrangement.value() == ir::VectorArrangement::H8 ? 16U : arrangement.value() == ir::VectorArrangement::S2 || arrangement.value() == ir::VectorArrangement::S4 ? 32U : 64U;
            const auto destination_width = instruction.operands[0].reg.width == aarch64::RegisterWidth::W32 ? 32U : 64U;
            auto result = lane;
            if (lane_width < destination_width)
                result = cast(op == Op::Smov ? ir::Opcode::SignExtend : ir::Opcode::ZeroExtend, lane.value(), type_for_width(instruction.operands[0].reg.width), instruction);
            else if (lane_width > destination_width)
                result = cast(ir::Opcode::Truncate, lane.value(), type_for_width(instruction.operands[0].reg.width), instruction);
            return result ? write_register(instruction.operands[0].reg, result.value(), instruction)
                          : Result<void>::failure(result.error());
        }
        case Op::Ext:
        {
            if (instruction.operands.size() < 3U || !destination_is_vector)
                return Result<void>::failure(unsupported(instruction, "EXT requires destination, two vectors, and byte index"));
            const auto arrangement = vector_arrangement(instruction.operands[0], instruction);
            const auto left = read_vector(instruction.operands[1].reg, instruction);
            const auto right = read_vector(instruction.operands[2].reg, instruction);
            if (!arrangement || !left || !right) return Result<void>::failure(!arrangement ? arrangement.error() : !left ? left.error() : right.error());
            ir::Instruction shuffle;
            shuffle.opcode = ir::Opcode::VectorShuffle;
            shuffle.result_type = ir::v128_type();
            shuffle.operands = {left.value(), right.value()};
            shuffle.arrangement = arrangement.value();
            shuffle.vector_index = 0U;
            shuffle.immediate = instruction.operands.size() > 3U ? instruction.operands[3].immediate : instruction.operands.back().immediate;
            shuffle.source = source_location(instruction);
            const auto result = emit_value(std::move(shuffle));
            return result ? write_vector(instruction.operands[0].reg, result.value(), instruction)
                          : Result<void>::failure(result.error());
        }
        case Op::Zip1: case Op::Zip2: case Op::Uzp1: case Op::Uzp2: case Op::Trn1: case Op::Trn2:
        {
            if (instruction.operands.size() != 3U || !destination_is_vector)
                return Result<void>::failure(unsupported(instruction, "vector permutation requires three vector registers"));
            const auto arrangement = vector_arrangement(instruction.operands[0], instruction);
            const auto left = read_vector(instruction.operands[1].reg, instruction);
            const auto right = read_vector(instruction.operands[2].reg, instruction);
            if (!arrangement || !left || !right) return Result<void>::failure(!arrangement ? arrangement.error() : !left ? left.error() : right.error());
            const auto shuffle_op = op == Op::Zip1 ? 1U : op == Op::Zip2 ? 2U : op == Op::Uzp1 ? 3U : op == Op::Uzp2 ? 4U : op == Op::Trn1 ? 5U : 6U;
            ir::Instruction shuffle;
            shuffle.opcode = ir::Opcode::VectorShuffle;
            shuffle.result_type = ir::v128_type();
            shuffle.operands = {left.value(), right.value()};
            shuffle.arrangement = arrangement.value();
            shuffle.vector_index = static_cast<std::uint8_t>(shuffle_op);
            shuffle.source = source_location(instruction);
            const auto result = emit_value(std::move(shuffle));
            return result ? write_vector(instruction.operands[0].reg, result.value(), instruction)
                          : Result<void>::failure(result.error());
        }
        case Op::Fcmeq: case Op::Fcmgt: case Op::Fcmge: case Op::Cmeq: case Op::Cmgt: case Op::Cmge: case Op::Cmhi: case Op::Cmhs:
        {
            if (instruction.operands.size() != 3U || !destination_is_vector)
                return Result<void>::failure(unsupported(instruction, "vector compare requires destination and two vectors"));
            const auto arrangement = vector_arrangement(instruction.operands[0], instruction);
            const auto left = read_vector(instruction.operands[1].reg, instruction);
            const auto right = read_vector(instruction.operands[2].reg, instruction);
            if (!arrangement || !left || !right) return Result<void>::failure(!arrangement ? arrangement.error() : !left ? left.error() : right.error());
            const auto compare = op == Op::Fcmeq ? ir::VectorCompareOperation::FpEqual
                              : op == Op::Fcmgt ? ir::VectorCompareOperation::FpGreaterThan
                              : op == Op::Fcmge ? ir::VectorCompareOperation::FpGreaterEqual
                              : op == Op::Cmeq ? ir::VectorCompareOperation::Equal
                              : op == Op::Cmgt ? ir::VectorCompareOperation::SignedGreaterThan
                              : op == Op::Cmge ? ir::VectorCompareOperation::SignedGreaterEqual
                              : op == Op::Cmhi ? ir::VectorCompareOperation::UnsignedHigher
                                                : ir::VectorCompareOperation::UnsignedHigherEqual;
            ir::Instruction vector;
            vector.opcode = ir::Opcode::VectorCompare;
            vector.result_type = ir::v128_type();
            vector.operands = {left.value(), right.value()};
            vector.arrangement = arrangement.value();
            vector.vector_compare = compare;
            vector.source = source_location(instruction);
            const auto result = emit_value(std::move(vector));
            return result ? write_vector(instruction.operands[0].reg, result.value(), instruction)
                          : Result<void>::failure(result.error());
        }
        case Op::Fmadd: case Op::Fmsub: case Op::Fnmadd: case Op::Fnmsub:
            return Result<void>::failure(unsupported(instruction, "fused FP multiply-add is intentionally unsupported in Milestone 8"));
        default:
            return Result<void>::failure(unsupported(instruction, "FP/SIMD operation is outside the Milestone 8 subset"));
        }
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

    [[nodiscard]] Result<void> lift_cmn(const DecodedInstruction& instruction)
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
        const auto result = binary(ir::Opcode::Add, left.value(), right.value(), type, instruction);
        if (!result)
        {
            return Result<void>::failure(result.error());
        }
        return emit_flags(left.value(), right.value(), result.value(), false, type, instruction);
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

    [[nodiscard]] Result<void> emit_logic_flags(ir::ValueId result, ir::Type type,
                                                const DecodedInstruction& instruction)
    {
        const auto zero = constant(type, 0U, instruction);
        const auto z = zero ? binary(ir::Opcode::CompareEqual, result, zero.value(), ir::i1_type(), instruction)
                            : Result<ir::ValueId>::failure(zero.error());
        const auto shift_amount = constant(type, type.bit_width() - 1U, instruction);
        const auto shifted = shift_amount
                                 ? binary(ir::Opcode::LogicalShiftRight, result, shift_amount.value(), type,
                                          instruction)
                                 : Result<ir::ValueId>::failure(shift_amount.error());
        if (!z || !shifted)
        {
            return Result<void>::failure(!z ? z.error() : shifted.error());
        }
        ir::Instruction truncate;
        truncate.opcode = ir::Opcode::Truncate;
        truncate.result_type = ir::i1_type();
        truncate.operands = {shifted.value()};
        truncate.source = source_location(instruction);
        const auto n = emit_value(std::move(truncate));
        if (!n)
        {
            return Result<void>::failure(n.error());
        }
        const auto false_value = constant(ir::i1_type(), 0U, instruction);
        if (!false_value)
        {
            return Result<void>::failure(false_value.error());
        }
        for (const auto& [flag, value] : {std::pair{ir::Flag::N, n.value()},
                                          std::pair{ir::Flag::Z, z.value()},
                                          std::pair{ir::Flag::C, false_value.value()},
                                          std::pair{ir::Flag::V, false_value.value()}})
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

    [[nodiscard]] Result<void> lift_logic(const DecodedInstruction& instruction, ir::Opcode opcode,
                                          bool sets_flags = false, bool invert_right = false)
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
        auto right_value = right.value();
        if (invert_right)
        {
            const auto inverted = unary(ir::Opcode::Not, right_value, type, instruction);
            if (!inverted)
            {
                return Result<void>::failure(inverted.error());
            }
            right_value = inverted.value();
        }
        const auto result = binary(opcode, left.value(), right_value, type, instruction);
        if (!result)
        {
            return Result<void>::failure(result.error());
        }
        if (sets_flags)
        {
            const auto flags = emit_logic_flags(result.value(), type, instruction);
            if (!flags)
            {
                return flags;
            }
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

    [[nodiscard]] Result<void> lift_movz(const DecodedInstruction& instruction, bool keep,
                                          bool negate = false)
    {
        if (instruction.operands.size() != 2U || instruction.operands[0].kind != aarch64::OperandKind::Register ||
            instruction.operands[1].kind != aarch64::OperandKind::Immediate)
        {
            return Result<void>::failure(unsupported(instruction, "expected register and 16-bit immediate"));
        }
        const auto type = type_for_width(instruction.operands[0].reg.width);
        const auto immediate = static_cast<std::int64_t>((instruction.opcode >> 5U) & 0xffffU);
        const auto shift = static_cast<std::uint8_t>(((instruction.opcode >> 21U) & 0x3U) * 16U);
        const auto max_shift = type == ir::i32_type() ? 16U : 48U;
        if (immediate < 0 || immediate > 0xffff ||
            (instruction.operands[1].shift_kind != aarch64::ShiftKind::None &&
             instruction.operands[1].shift_kind != aarch64::ShiftKind::Lsl) ||
            shift > max_shift || (shift % 16U) != 0U)
        {
            return Result<void>::failure(unsupported(instruction, "invalid MOVZ/MOVK immediate or shift"));
        }
        if (!keep)
        {
            auto value = static_cast<std::uint64_t>(immediate) << shift;
            if (negate)
            {
                value = ~value;
            }
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
            type, static_cast<std::uint64_t>(immediate) << shift, instruction);
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

    [[nodiscard]] Result<void> lift_test(const DecodedInstruction& instruction)
    {
        if (instruction.operands.size() != 2U || instruction.operands[0].kind != aarch64::OperandKind::Register)
        {
            return Result<void>::failure(unsupported(instruction, "expected two logical operands"));
        }
        const auto type = type_for_width(instruction.operands[0].reg.width);
        const auto left = operand_value(instruction.operands[0], type, instruction);
        const auto right = operand_value(instruction.operands[1], type, instruction);
        if (!left || !right)
        {
            return Result<void>::failure(!left ? left.error() : right.error());
        }
        const auto result = binary(ir::Opcode::And, left.value(), right.value(), type, instruction);
        if (!result)
        {
            return Result<void>::failure(result.error());
        }
        return emit_logic_flags(result.value(), type, instruction);
    }

    [[nodiscard]] Result<void> lift_neg(const DecodedInstruction& instruction, bool sets_flags)
    {
        if (instruction.operands.size() != 2U || instruction.operands[0].kind != aarch64::OperandKind::Register)
        {
            return Result<void>::failure(unsupported(instruction, "expected destination and source operand"));
        }
        const auto type = type_for_width(instruction.operands[0].reg.width);
        const auto source = operand_value(instruction.operands[1], type, instruction);
        const auto zero = constant(type, 0U, instruction);
        if (!source || !zero)
        {
            return Result<void>::failure(!source ? source.error() : zero.error());
        }
        const auto result = binary(ir::Opcode::Sub, zero.value(), source.value(), type, instruction);
        if (!result)
        {
            return Result<void>::failure(result.error());
        }
        if (sets_flags)
        {
            const auto flags = emit_flags(zero.value(), source.value(), result.value(), true, type, instruction);
            if (!flags)
            {
                return flags;
            }
        }
        return write_register(instruction.operands[0].reg, result.value(), instruction);
    }

    [[nodiscard]] Result<void> lift_conditional_select(const DecodedInstruction& instruction)
    {
        if (instruction.operands.empty() || instruction.operands[0].kind != aarch64::OperandKind::Register)
        {
            return Result<void>::failure(unsupported(instruction, "conditional select has no destination"));
        }
        const auto type = type_for_width(instruction.operands[0].reg.width);
        const auto condition = condition_value(instruction.condition.value_or(aarch64::ConditionCode::Al), instruction);
        if (!condition)
        {
            return Result<void>::failure(condition.error());
        }
        auto source_true = Result<ir::ValueId>::failure(
            unsupported(instruction, "conditional select has no true operand"));
        auto source_false = source_true;
        if (instruction.id == aarch64::InstructionId::Cset || instruction.id == aarch64::InstructionId::Csetm)
        {
            const auto zero = constant(type, 0U, instruction);
            const auto one = constant(type, instruction.id == aarch64::InstructionId::Csetm
                                                ? (type == ir::i32_type() ? 0xffffffffU
                                                                          : std::numeric_limits<std::uint64_t>::max())
                                                : 1U,
                                      instruction);
            if (!zero || !one)
            {
                return Result<void>::failure(!zero ? zero.error() : one.error());
            }
            source_true = one;
            source_false = zero;
        }
        else
        {
            if (instruction.operands.size() < 2U)
            {
                return Result<void>::failure(unsupported(instruction, "conditional select has no source operand"));
            }
            source_true = operand_value(instruction.operands[1], type, instruction);
            if (!source_true)
            {
                return Result<void>::failure(source_true.error());
            }
            if (instruction.id == aarch64::InstructionId::Cinc ||
                instruction.id == aarch64::InstructionId::Cinv ||
                instruction.id == aarch64::InstructionId::Cneg)
            {
                source_false = source_true;
            }
            else
            {
                if (instruction.operands.size() < 3U)
                {
                    return Result<void>::failure(unsupported(instruction, "conditional select has no false operand"));
                }
                source_false = operand_value(instruction.operands[2], type, instruction);
                if (!source_false)
                {
                    return Result<void>::failure(source_false.error());
                }
            }
        }

        if (instruction.id == aarch64::InstructionId::Csinc)
        {
            const auto one = constant(type, 1U, instruction);
            if (!one)
            {
                return Result<void>::failure(one.error());
            }
            source_false = binary(ir::Opcode::Add, source_false.value(), one.value(), type, instruction);
        }
        else if (instruction.id == aarch64::InstructionId::Csinv)
        {
            source_false = unary(ir::Opcode::Not, source_false.value(), type, instruction);
        }
        else if (instruction.id == aarch64::InstructionId::Csneg)
        {
            const auto zero = constant(type, 0U, instruction);
            if (!zero)
            {
                return Result<void>::failure(zero.error());
            }
            source_false = binary(ir::Opcode::Sub, zero.value(), source_false.value(), type, instruction);
        }
        else if (instruction.id == aarch64::InstructionId::Cinc)
        {
            const auto one = constant(type, 1U, instruction);
            if (!one)
            {
                return Result<void>::failure(one.error());
            }
            source_true = binary(ir::Opcode::Add, source_true.value(), one.value(), type, instruction);
        }
        else if (instruction.id == aarch64::InstructionId::Cinv)
        {
            source_true = unary(ir::Opcode::Not, source_true.value(), type, instruction);
        }
        else if (instruction.id == aarch64::InstructionId::Cneg)
        {
            const auto zero = constant(type, 0U, instruction);
            if (!zero)
            {
                return Result<void>::failure(zero.error());
            }
            source_true = binary(ir::Opcode::Sub, zero.value(), source_true.value(), type, instruction);
        }
        if (!source_false)
        {
            return Result<void>::failure(source_false.error());
        }
        const auto result = emit_value(ir::Instruction{ir::Opcode::Select, ir::invalid_value, type,
                                                        {condition.value(), source_true.value(),
                                                         source_false.value()}, {}, ir::Flag::N,
                                                        ir::ConditionCode::Al, 0, 0, 0,
                                                        source_location(instruction)});
        if (!result)
        {
            return Result<void>::failure(result.error());
        }
        return write_register(instruction.operands[0].reg, result.value(), instruction);
    }

    [[nodiscard]] Result<void> lift_multiply(const DecodedInstruction& instruction)
    {
        if (instruction.operands.size() < 3U || instruction.operands[0].kind != aarch64::OperandKind::Register ||
            instruction.operands[1].kind != aarch64::OperandKind::Register)
        {
            return Result<void>::failure(unsupported(instruction, "expected multiply register operands"));
        }
        const auto type = type_for_width(instruction.operands[0].reg.width);
        const auto left = operand_value(instruction.operands[1], type, instruction);
        const auto right = operand_value(instruction.operands[2], type, instruction);
        if (!left || !right)
        {
            return Result<void>::failure(!left ? left.error() : right.error());
        }
        const auto product = binary(ir::Opcode::Mul, left.value(), right.value(), type, instruction);
        if (!product)
        {
            return Result<void>::failure(product.error());
        }
        auto result = Result<ir::ValueId>::success(product.value());
        if (instruction.id == aarch64::InstructionId::Mneg)
        {
            const auto zero = constant(type, 0U, instruction);
            if (!zero)
            {
                return Result<void>::failure(zero.error());
            }
            result = binary(ir::Opcode::Sub, zero.value(), product.value(), type, instruction);
        }
        else if (instruction.id == aarch64::InstructionId::Madd || instruction.id == aarch64::InstructionId::Msub)
        {
            if (instruction.operands.size() != 4U)
            {
                return Result<void>::failure(unsupported(instruction, "MADD/MSUB requires an accumulator"));
            }
            const auto accumulator = operand_value(instruction.operands[3], type, instruction);
            if (!accumulator)
            {
                return Result<void>::failure(accumulator.error());
            }
            result = instruction.id == aarch64::InstructionId::Madd
                         ? binary(ir::Opcode::Add, product.value(), accumulator.value(), type, instruction)
                         : binary(ir::Opcode::Sub, accumulator.value(), product.value(), type, instruction);
            if (!result)
            {
                return Result<void>::failure(result.error());
            }
        }
        return write_register(instruction.operands[0].reg, result.value(), instruction);
    }

    [[nodiscard]] Result<void> lift_bitfield(const DecodedInstruction& instruction)
    {
        const bool encoded_bitfield = instruction.id == aarch64::InstructionId::Ubfm ||
                                      instruction.id == aarch64::InstructionId::Sbfm ||
                                      instruction.id == aarch64::InstructionId::Bfm;
        if (instruction.operands.size() < (encoded_bitfield ? 2U : 3U) ||
            instruction.operands[0].kind != aarch64::OperandKind::Register ||
            instruction.operands[1].kind != aarch64::OperandKind::Register ||
            (!encoded_bitfield && instruction.operands[2].kind != aarch64::OperandKind::Immediate))
        {
            return Result<void>::failure(unsupported(instruction, "expected bitfield register and immediate operands"));
        }
        const auto type = type_for_width(instruction.operands[0].reg.width);
        const auto width = static_cast<unsigned int>(type.bit_width());
        auto source = read_register(instruction.operands[1].reg, instruction);
        if (!source)
        {
            return Result<void>::failure(source.error());
        }
        if (type_for_width(instruction.operands[1].reg.width) != type)
        {
            const auto widened = cast(instruction.id == aarch64::InstructionId::Sbfm
                                          ? ir::Opcode::SignExtend
                                          : ir::Opcode::ZeroExtend,
                                      source.value(), type, instruction);
            if (!widened)
            {
                return Result<void>::failure(widened.error());
            }
            source = widened;
        }
        const auto immediate = (instruction.id == aarch64::InstructionId::Ubfm ||
                                instruction.id == aarch64::InstructionId::Sbfm ||
                                instruction.id == aarch64::InstructionId::Bfm)
                                   ? static_cast<unsigned int>((instruction.opcode >> 16U) & 0x3fU)
                                   : static_cast<unsigned int>(instruction.operands[2].immediate);
        if (immediate >= width)
        {
            return Result<void>::failure(unsupported(instruction, "bitfield shift is outside the register width"));
        }
        if (instruction.id == aarch64::InstructionId::Lsl ||
            instruction.id == aarch64::InstructionId::Lsr ||
            instruction.id == aarch64::InstructionId::Asr ||
            instruction.id == aarch64::InstructionId::Ror)
        {
            const auto shift = constant(type, immediate, instruction);
            if (!shift)
            {
                return Result<void>::failure(shift.error());
            }
            const auto opcode = instruction.id == aarch64::InstructionId::Lsl
                                    ? ir::Opcode::ShiftLeft
                                    : instruction.id == aarch64::InstructionId::Lsr
                                          ? ir::Opcode::LogicalShiftRight
                                          : instruction.id == aarch64::InstructionId::Asr
                                                ? ir::Opcode::ArithmeticShiftRight
                                                : ir::Opcode::RotateRight;
            const auto result = binary(opcode, source.value(), shift.value(), type, instruction);
            if (!result)
            {
                return Result<void>::failure(result.error());
            }
            return write_register(instruction.operands[0].reg, result.value(), instruction);
        }
        if (instruction.id != aarch64::InstructionId::Ubfm &&
            instruction.id != aarch64::InstructionId::Sbfm &&
            instruction.id != aarch64::InstructionId::Bfm &&
            (instruction.operands.size() != 4U || instruction.operands[3].kind != aarch64::OperandKind::Immediate))
        {
            return Result<void>::failure(unsupported(instruction, "bitfield operation requires immr and imms"));
        }
        const auto rotate_amount = immediate;
        const auto mask_amount = (instruction.id == aarch64::InstructionId::Ubfm ||
                                  instruction.id == aarch64::InstructionId::Sbfm ||
                                  instruction.id == aarch64::InstructionId::Bfm)
                                     ? static_cast<unsigned int>((instruction.opcode >> 10U) & 0x3fU)
                                     : static_cast<unsigned int>(instruction.operands[3].immediate);
        if (mask_amount >= width)
        {
            return Result<void>::failure(unsupported(instruction, "bitfield mask is outside the register width"));
        }
        const auto rotate = constant(type, rotate_amount, instruction);
        if (!rotate)
        {
            return Result<void>::failure(rotate.error());
        }
        const auto rotated = rotate_amount == 0U
                                 ? source
                                 : binary(ir::Opcode::RotateRight, source.value(), rotate.value(), type,
                                          instruction);
        if (!rotated)
        {
            return Result<void>::failure(rotated.error());
        }
        const auto length = mask_amount >= rotate_amount ? mask_amount - rotate_amount + 1U
                                                         : mask_amount + 1U;
        const auto mask = length == width ? (type == ir::i32_type() ? 0xffffffffU
                                                                      : std::numeric_limits<std::uint64_t>::max())
                                          : (std::uint64_t{1} << length) - 1U;
        const auto mask_value = constant(type, mask, instruction);
        if (!mask_value)
        {
            return Result<void>::failure(mask_value.error());
        }
        auto result = binary(ir::Opcode::And, rotated.value(), mask_value.value(), type, instruction);
        if (!result)
        {
            return Result<void>::failure(result.error());
        }
        if (instruction.id == aarch64::InstructionId::Sbfm)
        {
            const auto sign_shift = constant(type, length - 1U, instruction);
            const auto sign_bit = sign_shift
                                      ? binary(ir::Opcode::LogicalShiftRight, result.value(), sign_shift.value(), type,
                                               instruction)
                                      : Result<ir::ValueId>::failure(sign_shift.error());
            if (!sign_bit)
            {
                return Result<void>::failure(sign_bit.error());
            }
            const auto sign = cast(ir::Opcode::Truncate, sign_bit.value(), ir::i1_type(), instruction);
            const auto sign_mask = constant(type, type == ir::i32_type()
                                                       ? static_cast<std::uint64_t>(~static_cast<std::uint32_t>(mask))
                                                       : ~mask,
                                             instruction);
            const auto zero = constant(type, 0U, instruction);
            const auto extended = sign && sign_mask && zero
                                      ? emit_value(ir::Instruction{ir::Opcode::Select, ir::invalid_value, type,
                                                                   {sign.value(), sign_mask.value(), zero.value()}, {},
                                                                   ir::Flag::N, ir::ConditionCode::Al, 0, 0, 0,
                                                                   source_location(instruction)})
                                      : Result<ir::ValueId>::failure(!sign ? sign.error()
                                                                     : !sign_mask ? sign_mask.error() : zero.error());
            if (!extended)
            {
                return Result<void>::failure(extended.error());
            }
            result = binary(ir::Opcode::Or, result.value(), extended.value(), type, instruction);
        }
        if (instruction.id == aarch64::InstructionId::Bfm)
        {
            const auto old = read_register(instruction.operands[0].reg, instruction);
            const auto inverse_mask = unary(ir::Opcode::Not, mask_value.value(), type, instruction);
            if (!old || !inverse_mask)
            {
                return Result<void>::failure(!old ? old.error() : inverse_mask.error());
            }
            const auto preserved = binary(ir::Opcode::And, old.value(), inverse_mask.value(), type, instruction);
            result = preserved ? binary(ir::Opcode::Or, preserved.value(), result.value(), type, instruction)
                               : Result<ir::ValueId>::failure(preserved.error());
        }
        if (!result)
        {
            return Result<void>::failure(result.error());
        }
        return write_register(instruction.operands[0].reg, result.value(), instruction);
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
            !operand.memory.base.valid() || operand.memory.base.width != aarch64::RegisterWidth::X64)
        {
            return Result<ir::ValueId>::failure(
                unsupported(instruction, "memory operand does not have an X/SP base"));
        }
        const auto base = read_register(operand.memory.base, instruction);
        if (!base)
        {
            return base;
        }
        if (operand.memory.addressing == aarch64::MemoryAddressingMode::PostIndex)
        {
            return Result<ir::ValueId>::success(base.value());
        }
        ir::Instruction address_add;
        address_add.opcode = ir::Opcode::GuestAddressAdd;
        address_add.result_type = ir::i64_type();
        address_add.operands = {base.value()};
        address_add.immediate = operand.memory.displacement;
        address_add.source = source_location(instruction);
        return emit_value(std::move(address_add));
    }

    [[nodiscard]] Result<ir::ValueId> memory_index_value(const aarch64::MemoryOperand& memory,
                                                         const DecodedInstruction& instruction)
    {
        if (!memory.index.valid())
        {
            return Result<ir::ValueId>::failure(
                unsupported(instruction, "register-offset address is missing its index register"));
        }
        aarch64::Operand index;
        index.kind = aarch64::OperandKind::Register;
        index.reg = memory.index;
        index.shift = memory.shift;
        index.extension = memory.extension;
        index.shift_kind = memory.shift == 0U ? aarch64::ShiftKind::None : aarch64::ShiftKind::Lsl;
        return operand_value(index, ir::i64_type(), instruction);
    }

    [[nodiscard]] Result<ir::ValueId> address_for_memory(const aarch64::Operand& operand,
                                                         const DecodedInstruction& instruction)
    {
        const auto base = read_register(operand.memory.base, instruction);
        if (!base)
        {
            return base;
        }
        auto address = base.value();
        if (operand.memory.addressing != aarch64::MemoryAddressingMode::PostIndex)
        {
            if (operand.memory.index.valid())
            {
                const auto index = memory_index_value(operand.memory, instruction);
                if (!index)
                {
                    return index;
                }
                ir::Instruction add;
                add.opcode = ir::Opcode::GuestAddressAddValue;
                add.result_type = ir::i64_type();
                add.operands = {address, index.value()};
                add.address_offset_signed = operand.memory.extension == aarch64::ExtensionKind::Sxtb ||
                                            operand.memory.extension == aarch64::ExtensionKind::Sxth ||
                                            operand.memory.extension == aarch64::ExtensionKind::Sxtw ||
                                            operand.memory.extension == aarch64::ExtensionKind::Sxtx;
                add.source = source_location(instruction);
                const auto indexed = emit_value(std::move(add));
                if (!indexed)
                {
                    return indexed;
                }
                address = indexed.value();
            }
            if (operand.memory.displacement != 0)
            {
                ir::Instruction add;
                add.opcode = ir::Opcode::GuestAddressAdd;
                add.result_type = ir::i64_type();
                add.operands = {address};
                add.immediate = operand.memory.displacement;
                add.source = source_location(instruction);
                const auto displaced = emit_value(std::move(add));
                if (!displaced)
                {
                    return displaced;
                }
                address = displaced.value();
            }
        }
        return Result<ir::ValueId>::success(address);
    }

    [[nodiscard]] Result<void> writeback_memory(const aarch64::Operand& operand, ir::ValueId base,
                                                bool before, const DecodedInstruction& instruction)
    {
        if (!operand.memory.writeback ||
            (before && operand.memory.addressing != aarch64::MemoryAddressingMode::PreIndex) ||
            (!before && operand.memory.addressing != aarch64::MemoryAddressingMode::PostIndex))
        {
            return Result<void>::success();
        }
        ir::Instruction add;
        add.opcode = ir::Opcode::GuestAddressAdd;
        add.result_type = ir::i64_type();
        add.operands = {base};
        add.immediate = operand.memory.displacement;
        add.source = source_location(instruction);
        const auto updated = emit_value(std::move(add));
        if (!updated)
        {
            return Result<void>::failure(updated.error());
        }
        return write_register(operand.memory.base, updated.value(), instruction);
    }

    [[nodiscard]] Result<void> lift_vector_memory(const DecodedInstruction& instruction, bool store)
    {
        if (instruction.operands.size() != 2U || instruction.operands[0].kind != aarch64::OperandKind::Register ||
            instruction.operands[1].kind != aarch64::OperandKind::Memory ||
            !is_vector_register(instruction.operands[0].reg))
        {
            return Result<void>::failure(unsupported(instruction, "expected S/D/Q register and memory operand"));
        }
        const auto& data_reg = instruction.operands[0].reg;
        const auto size = data_reg.width == aarch64::RegisterWidth::S32 ? 4U
                          : data_reg.width == aarch64::RegisterWidth::D64 ? 8U
                                                                          : data_reg.width == aarch64::RegisterWidth::Q128 ? 16U : 0U;
        if (size == 0U)
        {
            return Result<void>::failure(unsupported(instruction, "only S/D/Q memory operations are supported"));
        }
        const auto& memory_operand = instruction.operands[1];
        const auto address = address_for_memory(memory_operand, instruction);
        const auto base = read_register(memory_operand.memory.base, instruction);
        if (!address || !base)
        {
            return Result<void>::failure(!address ? address.error() : base.error());
        }
        const auto pre_writeback = writeback_memory(memory_operand, base.value(), true, instruction);
        if (!pre_writeback) return pre_writeback;

        if (data_reg.width == aarch64::RegisterWidth::Q128)
        {
            if (store)
            {
                const auto value = read_vector(data_reg, instruction);
                if (!value) return Result<void>::failure(value.error());
                ir::Instruction memory;
                memory.opcode = ir::Opcode::GuestStoreVector;
                memory.result_type = ir::void_type();
                memory.operands = {address.value(), value.value()};
                memory.memory_size = 16U;
                memory.source = source_location(instruction);
                const auto emitted = emit_void(std::move(memory));
                if (!emitted) return emitted;
            }
            else
            {
                ir::Instruction memory;
                memory.opcode = ir::Opcode::GuestLoadVector;
                memory.result_type = ir::v128_type();
                memory.operands = {address.value()};
                memory.memory_size = 16U;
                memory.source = source_location(instruction);
                const auto value = emit_value(std::move(memory));
                if (!value) return Result<void>::failure(value.error());
                const auto written = write_vector(data_reg, value.value(), instruction);
                if (!written) return written;
            }
        }
        else
        {
            const auto arrangement = data_reg.width == aarch64::RegisterWidth::S32 ? ir::VectorArrangement::S2
                                                                                     : ir::VectorArrangement::D2;
            const auto raw_type = size == 4U ? ir::i32_type() : ir::i64_type();
            if (store)
            {
                const auto value = read_vector(data_reg, instruction);
                const auto raw = value ? vector_extract(value.value(), arrangement, 0U, instruction)
                                       : Result<ir::ValueId>::failure(value.error());
                if (!raw) return Result<void>::failure(!value ? value.error() : raw.error());
                ir::Instruction memory;
                memory.opcode = ir::Opcode::GuestStore;
                memory.result_type = ir::void_type();
                memory.operands = {address.value(), raw.value()};
                memory.memory_size = static_cast<std::uint8_t>(size);
                memory.source = source_location(instruction);
                const auto emitted = emit_void(std::move(memory));
                if (!emitted) return emitted;
            }
            else
            {
                ir::Instruction memory;
                memory.opcode = ir::Opcode::GuestLoad;
                memory.result_type = raw_type;
                memory.operands = {address.value()};
                memory.memory_size = static_cast<std::uint8_t>(size);
                memory.source = source_location(instruction);
                const auto raw = emit_value(std::move(memory));
                const auto zero = raw ? zero_vector(instruction) : Result<ir::ValueId>::failure(raw.error());
                if (!raw || !zero) return Result<void>::failure(!raw ? raw.error() : zero.error());
                ir::Instruction insert;
                insert.opcode = ir::Opcode::VectorInsertLane;
                insert.result_type = ir::v128_type();
                insert.operands = {zero.value(), raw.value()};
                insert.arrangement = arrangement;
                insert.lane_index = 0U;
                insert.source = source_location(instruction);
                const auto vector = emit_value(std::move(insert));
                if (!vector) return Result<void>::failure(vector.error());
                const auto written = write_vector(data_reg, vector.value(), instruction);
                if (!written) return written;
            }
        }
        return writeback_memory(memory_operand, base.value(), false, instruction);
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
        if (data_reg.kind == aarch64::RegisterKind::Vector)
        {
            return lift_vector_memory(instruction, store);
        }
        if (data_reg.width != aarch64::RegisterWidth::W32 && data_reg.width != aarch64::RegisterWidth::X64)
        {
            return Result<void>::failure(unsupported(instruction, "memory data register must be W or X"));
        }
        const auto& memory_operand = instruction.operands[1];
        const auto address = address_for_memory(memory_operand, instruction);
        if (!address)
        {
            return Result<void>::failure(address.error());
        }
        const bool byte = instruction.id == aarch64::InstructionId::Ldrb ||
                          instruction.id == aarch64::InstructionId::Ldrsb ||
                          instruction.id == aarch64::InstructionId::Strb;
        const bool half = instruction.id == aarch64::InstructionId::Ldrh ||
                          instruction.id == aarch64::InstructionId::Ldrsh ||
                          instruction.id == aarch64::InstructionId::Strh;
        const auto size = byte ? 1U
                               : half ? 2U
                                      : instruction.id == aarch64::InstructionId::Ldrsw
                                            ? 4U
                                            : data_reg.width == aarch64::RegisterWidth::W32 ? 4U : 8U;
        const auto raw_type = size == 1U ? ir::i8_type() : size == 2U ? ir::i16_type()
                                      : size == 4U ? ir::i32_type() : ir::i64_type();
        const bool signed_load = instruction.id == aarch64::InstructionId::Ldrsb ||
                                 instruction.id == aarch64::InstructionId::Ldrsh ||
                                 instruction.id == aarch64::InstructionId::Ldrsw;
        const auto base = read_register(memory_operand.memory.base, instruction);
        if (!base)
        {
            return Result<void>::failure(base.error());
        }
        const auto pre_writeback = writeback_memory(memory_operand, base.value(), true, instruction);
        if (!pre_writeback)
        {
            return pre_writeback;
        }
        if (store)
        {
            const auto value = read_register(data_reg, instruction);
            if (!value)
            {
                return Result<void>::failure(value.error());
            }
            auto stored = value.value();
            if (raw_type != type_for_width(data_reg.width))
            {
                const auto narrowed = cast(ir::Opcode::Truncate, stored, raw_type, instruction);
                if (!narrowed)
                {
                    return Result<void>::failure(narrowed.error());
                }
                stored = narrowed.value();
            }
            ir::Instruction store_instruction;
            store_instruction.opcode = ir::Opcode::GuestStore;
            store_instruction.operands = {address.value(), stored};
            store_instruction.memory_size = static_cast<std::uint8_t>(size);
            store_instruction.result_type = ir::void_type();
            store_instruction.source = source_location(instruction);
            const auto emitted = emit_void(std::move(store_instruction));
            if (!emitted)
            {
                return emitted;
            }
        }
        else
        {
            ir::Instruction load;
            load.opcode = ir::Opcode::GuestLoad;
            load.result_type = raw_type;
            load.operands = {address.value()};
            load.memory_size = static_cast<std::uint8_t>(size);
            load.source = source_location(instruction);
            const auto value = emit_value(std::move(load));
            if (!value)
            {
                return Result<void>::failure(value.error());
            }
            auto loaded = value.value();
            const auto destination_type = type_for_width(data_reg.width);
            if (raw_type != destination_type)
            {
                const auto widened = cast(signed_load ? ir::Opcode::SignExtend : ir::Opcode::ZeroExtend,
                                          loaded, destination_type, instruction);
                if (!widened)
                {
                    return Result<void>::failure(widened.error());
                }
                loaded = widened.value();
            }
            const auto written = write_register(data_reg, loaded, instruction);
            if (!written)
            {
                return written;
            }
        }
        return writeback_memory(memory_operand, base.value(), false, instruction);
    }

    [[nodiscard]] Result<void> lift_pair(const DecodedInstruction& instruction, bool store)
    {
        if (instruction.operands.size() != 3U ||
            instruction.operands[0].kind != aarch64::OperandKind::Register ||
            instruction.operands[1].kind != aarch64::OperandKind::Register ||
            instruction.operands[2].kind != aarch64::OperandKind::Memory)
        {
            return Result<void>::failure(unsupported(instruction, "expected two registers and memory operand"));
        }
        const auto& first = instruction.operands[0].reg;
        const auto& second = instruction.operands[1].reg;
        if (first.kind == aarch64::RegisterKind::Vector || second.kind == aarch64::RegisterKind::Vector)
        {
            if (!is_vector_register(first) || !is_vector_register(second) || first.width != second.width)
            {
                return Result<void>::failure(unsupported(instruction, "vector pair registers must have matching S/D/Q widths"));
            }
            const auto size = first.width == aarch64::RegisterWidth::S32 ? 4U
                              : first.width == aarch64::RegisterWidth::D64 ? 8U
                                                                            : first.width == aarch64::RegisterWidth::Q128 ? 16U : 0U;
            if (size == 0U) return Result<void>::failure(unsupported(instruction, "only S/D/Q vector pairs are supported"));
            const auto& memory_operand = instruction.operands[2];
            const auto address = address_for_memory(memory_operand, instruction);
            const auto base = read_register(memory_operand.memory.base, instruction);
            if (!address || !base) return Result<void>::failure(!address ? address.error() : base.error());
            const auto pre_writeback = writeback_memory(memory_operand, base.value(), true, instruction);
            if (!pre_writeback) return pre_writeback;
            auto access = [&](const aarch64::Register& reg, ir::ValueId address_value) -> Result<void> {
                if (reg.width == aarch64::RegisterWidth::Q128)
                {
                    if (store)
                    {
                        const auto value = read_vector(reg, instruction);
                        if (!value) return Result<void>::failure(value.error());
                        ir::Instruction memory;
                        memory.opcode = ir::Opcode::GuestStoreVector;
                        memory.result_type = ir::void_type();
                        memory.operands = {address_value, value.value()};
                        memory.memory_size = 16U;
                        memory.source = source_location(instruction);
                        return emit_void(std::move(memory));
                    }
                    ir::Instruction memory;
                    memory.opcode = ir::Opcode::GuestLoadVector;
                    memory.result_type = ir::v128_type();
                    memory.operands = {address_value};
                    memory.memory_size = 16U;
                    memory.source = source_location(instruction);
                    const auto value = emit_value(std::move(memory));
                    if (!value) return Result<void>::failure(value.error());
                    return write_vector(reg, value.value(), instruction);
                }
                const auto arrangement = reg.width == aarch64::RegisterWidth::S32 ? ir::VectorArrangement::S2
                                                                                     : ir::VectorArrangement::D2;
                const auto raw_type = reg.width == aarch64::RegisterWidth::S32 ? ir::i32_type() : ir::i64_type();
                if (store)
                {
                    const auto vector = read_vector(reg, instruction);
                    const auto value = vector ? vector_extract(vector.value(), arrangement, 0U, instruction)
                                               : Result<ir::ValueId>::failure(vector.error());
                    if (!value) return Result<void>::failure(!vector ? vector.error() : value.error());
                    ir::Instruction memory;
                    memory.opcode = ir::Opcode::GuestStore;
                    memory.result_type = ir::void_type();
                    memory.operands = {address_value, value.value()};
                    memory.memory_size = static_cast<std::uint8_t>(size);
                    memory.source = source_location(instruction);
                    return emit_void(std::move(memory));
                }
                ir::Instruction memory;
                memory.opcode = ir::Opcode::GuestLoad;
                memory.result_type = raw_type;
                memory.operands = {address_value};
                memory.memory_size = static_cast<std::uint8_t>(size);
                memory.source = source_location(instruction);
                const auto raw = emit_value(std::move(memory));
                const auto zero = raw ? zero_vector(instruction) : Result<ir::ValueId>::failure(raw.error());
                if (!raw || !zero) return Result<void>::failure(!raw ? raw.error() : zero.error());
                ir::Instruction insert;
                insert.opcode = ir::Opcode::VectorInsertLane;
                insert.result_type = ir::v128_type();
                insert.operands = {zero.value(), raw.value()};
                insert.arrangement = arrangement;
                insert.lane_index = 0U;
                insert.source = source_location(instruction);
                const auto vector = emit_value(std::move(insert));
                if (!vector) return Result<void>::failure(vector.error());
                return write_vector(reg, vector.value(), instruction);
            };
            const auto first_access = access(first, address.value());
            if (!first_access) return first_access;
            ir::Instruction next;
            next.opcode = ir::Opcode::GuestAddressAdd;
            next.result_type = ir::i64_type();
            next.operands = {address.value()};
            next.immediate = static_cast<std::int64_t>(size);
            next.source = source_location(instruction);
            const auto next_address = emit_value(std::move(next));
            if (!next_address) return Result<void>::failure(next_address.error());
            const auto second_access = access(second, next_address.value());
            if (!second_access) return second_access;
            return writeback_memory(memory_operand, base.value(), false, instruction);
        }
        if ((first.width != aarch64::RegisterWidth::W32 && first.width != aarch64::RegisterWidth::X64) ||
            first.width != second.width)
        {
            return Result<void>::failure(unsupported(instruction, "pair registers must have matching W/X widths"));
        }
        const auto& memory_operand = instruction.operands[2];
        const auto address = address_for_memory(memory_operand, instruction);
        if (!address)
        {
            return Result<void>::failure(address.error());
        }
        const auto base = read_register(memory_operand.memory.base, instruction);
        if (!base)
        {
            return Result<void>::failure(base.error());
        }
        const auto pre_writeback = writeback_memory(memory_operand, base.value(), true, instruction);
        if (!pre_writeback)
        {
            return pre_writeback;
        }
        const auto size = first.width == aarch64::RegisterWidth::W32 ? 4U : 8U;
        const auto type = type_for_width(first.width);
        auto emit_access = [&](const aarch64::Register& reg, ir::ValueId access_address,
                               bool is_store) -> Result<void> {
            if (is_store)
            {
                const auto value = read_register(reg, instruction);
                if (!value)
                {
                    return Result<void>::failure(value.error());
                }
                ir::Instruction store_instruction;
                store_instruction.opcode = ir::Opcode::GuestStore;
                store_instruction.result_type = ir::void_type();
                store_instruction.operands = {access_address, value.value()};
                store_instruction.memory_size = static_cast<std::uint8_t>(size);
                store_instruction.source = source_location(instruction);
                return emit_void(std::move(store_instruction));
            }
            ir::Instruction load;
            load.opcode = ir::Opcode::GuestLoad;
            load.result_type = type;
            load.operands = {access_address};
            load.memory_size = static_cast<std::uint8_t>(size);
            load.source = source_location(instruction);
            const auto value = emit_value(std::move(load));
            if (!value)
            {
                return Result<void>::failure(value.error());
            }
            return write_register(reg, value.value(), instruction);
        };

        const auto first_access = emit_access(first, address.value(), store);
        if (!first_access)
        {
            return first_access;
        }
        ir::Instruction second_address;
        second_address.opcode = ir::Opcode::GuestAddressAdd;
        second_address.result_type = ir::i64_type();
        second_address.operands = {address.value()};
        second_address.immediate = static_cast<std::int64_t>(size);
        second_address.source = source_location(instruction);
        const auto next_address = emit_value(std::move(second_address));
        if (!next_address)
        {
            return Result<void>::failure(next_address.error());
        }
        const auto second_access = emit_access(second, next_address.value(), store);
        if (!second_access)
        {
            return second_access;
        }
        return writeback_memory(memory_operand, base.value(), false, instruction);
    }

    [[nodiscard]] Result<void> lift_literal(const DecodedInstruction& instruction)
    {
        if (instruction.operands.size() < 1U || instruction.operands[0].kind != aarch64::OperandKind::Register ||
            !instruction.pc_relative_value)
        {
            return Result<void>::failure(unsupported(instruction, "literal load has no validated target"));
        }
        const auto& destination = instruction.operands[0].reg;
        const auto destination_type = type_for_width(destination.width);
        const bool sign_extend_word = ((instruction.opcode >> 30U) & 0x3U) == 2U;
        const auto size = sign_extend_word ? 4U : destination.width == aarch64::RegisterWidth::W32 ? 4U : 8U;
        const auto raw_type = size == 4U ? ir::i32_type() : ir::i64_type();
        const auto address = constant(ir::i64_type(), instruction.pc_relative_value.value(), instruction);
        if (!address)
        {
            return Result<void>::failure(address.error());
        }
        ir::Instruction load;
        load.opcode = ir::Opcode::GuestLoad;
        load.result_type = raw_type;
        load.operands = {address.value()};
        load.memory_size = static_cast<std::uint8_t>(size);
        load.source = source_location(instruction);
        const auto loaded = emit_value(std::move(load));
        if (!loaded)
        {
            return Result<void>::failure(loaded.error());
        }
        auto value = loaded.value();
        if (raw_type != destination_type)
        {
            const auto widened = cast(sign_extend_word ? ir::Opcode::SignExtend : ir::Opcode::ZeroExtend,
                                      value, destination_type, instruction);
            if (!widened)
            {
                return Result<void>::failure(widened.error());
            }
            value = widened.value();
        }
        return write_register(destination, value, instruction);
    }

    [[nodiscard]] Result<void> lift_link(const DecodedInstruction& instruction)
    {
        const auto return_address = checked_add_u64(instruction.address, 4U);
        if (!return_address)
        {
            return Result<void>::failure(return_address.error());
        }
        const aarch64::Register link{aarch64::RegisterKind::General, aarch64::RegisterWidth::X64,
                                     30U, false, false};
        const auto value = constant(ir::i64_type(), return_address.value(), instruction);
        if (!value)
        {
            return Result<void>::failure(value.error());
        }
        return write_register(link, value.value(), instruction);
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
                                         const DecodedInstruction& instruction,
                                         std::optional<ir::ValueId> target_override = std::nullopt)
    {
        const auto& flow = instruction.control_flow;
        ir::Terminator terminator;
        terminator.source = source_location(instruction);
        if (flow.kind == aarch64::ControlFlowKind::Return)
        {
            const auto return_register = flow.return_register.value_or(aarch64::Register{
                aarch64::RegisterKind::General, aarch64::RegisterWidth::X64, 30U, false, false});
            const auto target = read_register(return_register, instruction);
            if (!target)
            {
                return Result<void>::failure(target.error());
            }
            terminator.kind = ir::TerminatorKind::Return;
            terminator.target_value = target.value();
            return builder_.set_terminator(std::move(terminator));
        }
        if (flow.kind == aarch64::ControlFlowKind::IndirectBranch)
        {
            if (!flow.register_target)
            {
                return Result<void>::failure(unsupported(instruction, "indirect branch has no target register"));
            }
            const auto target = read_register(flow.register_target.value(), instruction);
            if (!target)
            {
                return Result<void>::failure(target.error());
            }
            terminator.kind = ir::TerminatorKind::IndirectBranch;
            terminator.target_value = target.value();
            return builder_.set_terminator(std::move(terminator));
        }
        if (flow.kind == aarch64::ControlFlowKind::IndirectCall)
        {
            if (!flow.register_target)
            {
                return Result<void>::failure(unsupported(instruction, "indirect call has no target register"));
            }
            const auto target = target_override.value_or(ir::invalid_value) != ir::invalid_value
                                    ? Result<ir::ValueId>::success(target_override.value())
                                    : read_register(flow.register_target.value(), instruction);
            if (!target)
            {
                return Result<void>::failure(target.error());
            }
            terminator.kind = ir::TerminatorKind::IndirectCall;
            terminator.target_value = target.value();
            return builder_.set_terminator(std::move(terminator));
        }
        if (flow.kind == aarch64::ControlFlowKind::DirectCall)
        {
            if (!flow.target)
            {
                return Result<void>::failure(unsupported(instruction, "direct call has no target address"));
            }
            const auto target = constant(ir::i64_type(), flow.target.value(), instruction);
            if (!target)
            {
                return Result<void>::failure(target.error());
            }
            terminator.kind = ir::TerminatorKind::DirectCall;
            terminator.target_value = target.value();
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
            else if (instruction.id == aarch64::InstructionId::Tbz ||
                     instruction.id == aarch64::InstructionId::Tbnz)
            {
                if (instruction.operands.empty() || instruction.operands[0].kind != aarch64::OperandKind::Register)
                {
                    return Result<void>::failure(unsupported(instruction, "test-and-branch has no register operand"));
                }
                const auto type = type_for_width(instruction.operands[0].reg.width);
                const auto value = read_register(instruction.operands[0].reg, instruction);
                const auto bit = constant(type, ((instruction.opcode >> 19U) & 0x1fU) |
                                                       (((instruction.opcode >> 31U) & 0x1U) << 5U),
                                           instruction);
                const auto one = constant(type, 1U, instruction);
                if (!value || !bit || !one)
                {
                    return Result<void>::failure(!value ? value.error() : !bit ? bit.error() : one.error());
                }
                const auto shifted = binary(ir::Opcode::LogicalShiftRight, value.value(), bit.value(), type, instruction);
                const auto tested = shifted ? binary(ir::Opcode::And, shifted.value(), one.value(), type, instruction)
                                            : Result<ir::ValueId>::failure(shifted.error());
                if (!tested)
                {
                    return Result<void>::failure(tested.error());
                }
                const auto zero = constant(type, 0U, instruction);
                const auto branch_value = zero
                                              ? binary(instruction.id == aarch64::InstructionId::Tbz
                                                           ? ir::Opcode::CompareEqual
                                                           : ir::Opcode::CompareNotEqual,
                                                       tested.value(), zero.value(), ir::i1_type(), instruction)
                                              : Result<ir::ValueId>::failure(zero.error());
                if (!branch_value)
                {
                    return Result<void>::failure(branch_value.error());
                }
                condition = branch_value.value();
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
        return Result<void>::failure(unsupported(instruction, "control-flow form is outside the documented lifting subset"));
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
                std::optional<ir::ValueId> indirect_call_target;
                switch (instruction.id)
                {
                case aarch64::InstructionId::B:
                case aarch64::InstructionId::BCond:
                case aarch64::InstructionId::Cbz:
                case aarch64::InstructionId::Cbnz:
                case aarch64::InstructionId::Tbz:
                case aarch64::InstructionId::Tbnz:
                    break;
                case aarch64::InstructionId::Bl:
                    if (instruction.control_flow.target &&
                        instruction.control_flow.target.value() == instruction.address)
                    {
                        return Result<void>::failure(unsupported(
                            instruction, "self-recursive direct calls require a runtime dispatcher"));
                    }
                    {
                        const auto link = lift_link(instruction);
                        if (!link)
                        {
                            return link;
                        }
                    }
                    break;
                case aarch64::InstructionId::Blr:
                {
                    if (!instruction.control_flow.register_target)
                    {
                        return Result<void>::failure(unsupported(
                            instruction, "indirect call has no target register"));
                    }
                    const auto target = read_register(
                        instruction.control_flow.register_target.value(), instruction);
                    if (!target)
                    {
                        return Result<void>::failure(target.error());
                    }
                    indirect_call_target = target.value();
                    {
                        const auto link = lift_link(instruction);
                        if (!link)
                        {
                            return link;
                        }
                    }
                    break;
                }
                case aarch64::InstructionId::Ret:
                case aarch64::InstructionId::Br:
                    break;
                default:
                    if (instruction.control_flow.kind != aarch64::ControlFlowKind::Return)
                    {
                        return Result<void>::failure(unsupported(
                            instruction, "terminating instruction is not supported by the standalone lifter"));
                    }
                    break;
                }
                const auto end = terminate(block, instruction, indirect_call_target);
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
        if (instruction.id == aarch64::InstructionId::FpSimd)
        {
            return lift_fp_simd(instruction);
        }
        if (!instruction.operands.empty() && instruction.operands[0].kind == aarch64::OperandKind::Register &&
            instruction.operands[0].reg.kind == aarch64::RegisterKind::Vector)
        {
            switch (instruction.id)
            {
            case aarch64::InstructionId::Add:
            case aarch64::InstructionId::Sub:
            case aarch64::InstructionId::Mul:
            case aarch64::InstructionId::And:
            case aarch64::InstructionId::Ands:
            case aarch64::InstructionId::Orr:
            case aarch64::InstructionId::Eor:
            case aarch64::InstructionId::Bic:
            case aarch64::InstructionId::Bics:
                return lift_vector_common(instruction);
            default:
                break;
            }
        }
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
        case aarch64::InstructionId::Movn:
            return lift_movz(instruction, false, true);
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
        case aarch64::InstructionId::Cmn:
            return lift_cmn(instruction);
        case aarch64::InstructionId::Tst:
            return lift_test(instruction);
        case aarch64::InstructionId::And:
            return lift_logic(instruction, ir::Opcode::And);
        case aarch64::InstructionId::Ands:
            return lift_logic(instruction, ir::Opcode::And, true);
        case aarch64::InstructionId::Orr:
            return lift_logic(instruction, ir::Opcode::Or);
        case aarch64::InstructionId::Orn:
            return lift_logic(instruction, ir::Opcode::Or, false, true);
        case aarch64::InstructionId::Eor:
            return lift_logic(instruction, ir::Opcode::Xor);
        case aarch64::InstructionId::Eon:
            return lift_logic(instruction, ir::Opcode::Xor, false, true);
        case aarch64::InstructionId::Bic:
            return lift_logic(instruction, ir::Opcode::And, false, true);
        case aarch64::InstructionId::Bics:
            return lift_logic(instruction, ir::Opcode::And, true, true);
        case aarch64::InstructionId::Neg:
            return lift_neg(instruction, false);
        case aarch64::InstructionId::Negs:
            return lift_neg(instruction, true);
        case aarch64::InstructionId::Mvn:
            if (instruction.operands.size() != 2U || instruction.operands[0].kind != aarch64::OperandKind::Register)
            {
                return Result<void>::failure(unsupported(instruction, "expected destination and source operand"));
            }
            {
                const auto type = type_for_width(instruction.operands[0].reg.width);
                const auto source = operand_value(instruction.operands[1], type, instruction);
                if (!source)
                {
                    return Result<void>::failure(source.error());
                }
                const auto result = unary(ir::Opcode::Not, source.value(), type, instruction);
                return result ? write_register(instruction.operands[0].reg, result.value(), instruction)
                              : Result<void>::failure(result.error());
            }
        case aarch64::InstructionId::Csel:
        case aarch64::InstructionId::Csinc:
        case aarch64::InstructionId::Csinv:
        case aarch64::InstructionId::Csneg:
        case aarch64::InstructionId::Cset:
        case aarch64::InstructionId::Csetm:
        case aarch64::InstructionId::Cinc:
        case aarch64::InstructionId::Cinv:
        case aarch64::InstructionId::Cneg:
            return lift_conditional_select(instruction);
        case aarch64::InstructionId::Lsl:
        case aarch64::InstructionId::Lsr:
        case aarch64::InstructionId::Asr:
        case aarch64::InstructionId::Ror:
        case aarch64::InstructionId::Ubfm:
        case aarch64::InstructionId::Sbfm:
        case aarch64::InstructionId::Bfm:
            return lift_bitfield(instruction);
        case aarch64::InstructionId::Mul:
        case aarch64::InstructionId::Madd:
        case aarch64::InstructionId::Msub:
        case aarch64::InstructionId::Mneg:
            return lift_multiply(instruction);
        case aarch64::InstructionId::Adr:
        case aarch64::InstructionId::Adrp:
            return lift_pc_relative(instruction);
        case aarch64::InstructionId::Ldr:
        case aarch64::InstructionId::Ldur:
        case aarch64::InstructionId::Ldrb:
        case aarch64::InstructionId::Ldrh:
        case aarch64::InstructionId::Ldrsb:
        case aarch64::InstructionId::Ldrsh:
        case aarch64::InstructionId::Ldrsw:
            return lift_memory(instruction, false);
        case aarch64::InstructionId::Str:
        case aarch64::InstructionId::Stur:
        case aarch64::InstructionId::Strb:
        case aarch64::InstructionId::Strh:
            return lift_memory(instruction, true);
        case aarch64::InstructionId::Ldp:
            return lift_pair(instruction, false);
        case aarch64::InstructionId::Stp:
            return lift_pair(instruction, true);
        case aarch64::InstructionId::LdrLiteral:
            return lift_literal(instruction);
        case aarch64::InstructionId::Bl:
        case aarch64::InstructionId::Blr:
            return lift_link(instruction);
        case aarch64::InstructionId::Br:
        case aarch64::InstructionId::Ret:
            return Result<void>::success();
        default:
            return Result<void>::failure(unsupported(instruction, "instruction is outside the documented lifting subset"));
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

bool is_instruction_liftable(aarch64::InstructionId id) noexcept
{
    switch (id)
    {
    case aarch64::InstructionId::Nop:
    case aarch64::InstructionId::Add:
    case aarch64::InstructionId::Adds:
    case aarch64::InstructionId::Sub:
    case aarch64::InstructionId::Subs:
    case aarch64::InstructionId::And:
    case aarch64::InstructionId::Ands:
    case aarch64::InstructionId::Orr:
    case aarch64::InstructionId::Orn:
    case aarch64::InstructionId::Eor:
    case aarch64::InstructionId::Eon:
    case aarch64::InstructionId::Bic:
    case aarch64::InstructionId::Bics:
    case aarch64::InstructionId::Mov:
    case aarch64::InstructionId::Mvn:
    case aarch64::InstructionId::Cmp:
    case aarch64::InstructionId::Cmn:
    case aarch64::InstructionId::Tst:
    case aarch64::InstructionId::Neg:
    case aarch64::InstructionId::Negs:
    case aarch64::InstructionId::Csel:
    case aarch64::InstructionId::Csinc:
    case aarch64::InstructionId::Csinv:
    case aarch64::InstructionId::Csneg:
    case aarch64::InstructionId::Cset:
    case aarch64::InstructionId::Csetm:
    case aarch64::InstructionId::Cinc:
    case aarch64::InstructionId::Cinv:
    case aarch64::InstructionId::Cneg:
    case aarch64::InstructionId::Movz:
    case aarch64::InstructionId::Movk:
    case aarch64::InstructionId::Movn:
    case aarch64::InstructionId::Lsl:
    case aarch64::InstructionId::Lsr:
    case aarch64::InstructionId::Asr:
    case aarch64::InstructionId::Ror:
    case aarch64::InstructionId::Ubfm:
    case aarch64::InstructionId::Sbfm:
    case aarch64::InstructionId::Bfm:
    case aarch64::InstructionId::Mul:
    case aarch64::InstructionId::Madd:
    case aarch64::InstructionId::Msub:
    case aarch64::InstructionId::Mneg:
    case aarch64::InstructionId::Adr:
    case aarch64::InstructionId::Adrp:
    case aarch64::InstructionId::Ldr:
    case aarch64::InstructionId::Ldrb:
    case aarch64::InstructionId::Ldrh:
    case aarch64::InstructionId::Ldrsb:
    case aarch64::InstructionId::Ldrsh:
    case aarch64::InstructionId::Ldrsw:
    case aarch64::InstructionId::Str:
    case aarch64::InstructionId::Strb:
    case aarch64::InstructionId::Strh:
    case aarch64::InstructionId::Ldp:
    case aarch64::InstructionId::Stp:
    case aarch64::InstructionId::Ldur:
    case aarch64::InstructionId::Stur:
    case aarch64::InstructionId::LdrLiteral:
    case aarch64::InstructionId::B:
    case aarch64::InstructionId::Bl:
    case aarch64::InstructionId::BCond:
    case aarch64::InstructionId::Br:
    case aarch64::InstructionId::Blr:
    case aarch64::InstructionId::Ret:
    case aarch64::InstructionId::Cbz:
    case aarch64::InstructionId::Cbnz:
    case aarch64::InstructionId::Tbz:
    case aarch64::InstructionId::Tbnz:
        return true;
    case aarch64::InstructionId::Unknown:
    case aarch64::InstructionId::Ccmp:
    case aarch64::InstructionId::Ccmn:
    case aarch64::InstructionId::Udiv:
    case aarch64::InstructionId::Sdiv:
    case aarch64::InstructionId::FpSimd:
    case aarch64::InstructionId::Ldxr:
    case aarch64::InstructionId::Stxr:
    case aarch64::InstructionId::Ldaxr:
    case aarch64::InstructionId::Stlxr:
    case aarch64::InstructionId::Dmb:
    case aarch64::InstructionId::Dsb:
    case aarch64::InstructionId::Isb:
    case aarch64::InstructionId::Svc:
    case aarch64::InstructionId::Brk:
    case aarch64::InstructionId::Hlt:
    case aarch64::InstructionId::Hvc:
    case aarch64::InstructionId::Smc:
    case aarch64::InstructionId::Eret:
        return id == aarch64::InstructionId::FpSimd;
    }
    return false;
}

bool is_instruction_liftable(const aarch64::DecodedInstruction& instruction) noexcept
{
    if (!instruction.normalized)
    {
        return false;
    }
    if (instruction.id != aarch64::InstructionId::FpSimd)
    {
        return is_instruction_liftable(instruction.id);
    }
    switch (instruction.simd_operation)
    {
    case aarch64::SimdOperation::Fmov:
    case aarch64::SimdOperation::Fadd:
    case aarch64::SimdOperation::Fsub:
    case aarch64::SimdOperation::Fmul:
    case aarch64::SimdOperation::Fdiv:
    case aarch64::SimdOperation::Fneg:
    case aarch64::SimdOperation::Fabs:
    case aarch64::SimdOperation::Fsqrt:
    case aarch64::SimdOperation::Fmin:
    case aarch64::SimdOperation::Fmax:
    case aarch64::SimdOperation::Fcmp:
    case aarch64::SimdOperation::Fcmpe:
    case aarch64::SimdOperation::Fcsel:
    case aarch64::SimdOperation::Scvtf:
    case aarch64::SimdOperation::Ucvtf:
    case aarch64::SimdOperation::Fcvtzs:
    case aarch64::SimdOperation::Fcvtzu:
    case aarch64::SimdOperation::Fcvt:
    case aarch64::SimdOperation::Frintn:
    case aarch64::SimdOperation::Frintp:
    case aarch64::SimdOperation::Frintm:
    case aarch64::SimdOperation::Frintz:
    case aarch64::SimdOperation::Dup:
    case aarch64::SimdOperation::Ins:
    case aarch64::SimdOperation::Umov:
    case aarch64::SimdOperation::Smov:
    case aarch64::SimdOperation::Ext:
    case aarch64::SimdOperation::Zip1:
    case aarch64::SimdOperation::Zip2:
    case aarch64::SimdOperation::Uzp1:
    case aarch64::SimdOperation::Uzp2:
    case aarch64::SimdOperation::Trn1:
    case aarch64::SimdOperation::Trn2:
    case aarch64::SimdOperation::Fcmeq:
    case aarch64::SimdOperation::Fcmgt:
    case aarch64::SimdOperation::Fcmge:
    case aarch64::SimdOperation::Cmeq:
    case aarch64::SimdOperation::Cmgt:
    case aarch64::SimdOperation::Cmge:
    case aarch64::SimdOperation::Cmhi:
    case aarch64::SimdOperation::Cmhs:
        return true;
    case aarch64::SimdOperation::None:
    case aarch64::SimdOperation::Fmadd:
    case aarch64::SimdOperation::Fmsub:
    case aarch64::SimdOperation::Fnmadd:
    case aarch64::SimdOperation::Fnmsub:
        return false;
    }
    return false;
}

} // namespace switchrecomp::lifter
