#include "switchrecomp/lift/aarch64_lifter.hpp"

#include "switchrecomp/common/checked_arithmetic.hpp"
#include "switchrecomp/ir/builder.hpp"
#include "switchrecomp/ir/verifier.hpp"

#include <iomanip>
#include <map>
#include <limits>
#include <new>
#include <sstream>
#include <string>
#include <utility>

namespace switchrecomp::lift
{

namespace
{

using aarch64::DecodedInstruction;
using aarch64::InstructionId;
using aarch64::MemoryAddressingMode;
using aarch64::Operand;
using aarch64::OperandKind;
using aarch64::Register;
using aarch64::RegisterKind;
using aarch64::RegisterWidth;
using ir::BlockId;
using ir::IrBasicBlock;
using ir::IrBuilder;
using ir::IrFunction;
using ir::IrInstruction;
using ir::IrOpcode;
using ir::IrSourceLocation;
using ir::IrType;
using ir::ValueId;

[[nodiscard]] std::string hex(std::uint64_t value, unsigned int width)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::setw(static_cast<int>(width)) << std::setfill('0') << value;
    return output.str();
}

[[nodiscard]] IrSourceLocation source_of(const DecodedInstruction& instruction)
{
    return IrSourceLocation{instruction.address, instruction.opcode, instruction.id};
}

[[nodiscard]] Result<IrType> register_type(const Register& reg, const DecodedInstruction& instruction)
{
    if (reg.kind != RegisterKind::General ||
        (reg.width != RegisterWidth::W32 && reg.width != RegisterWidth::X64))
    {
        return Result<IrType>::failure(make_error(
            ErrorCode::InvalidRegisterWidth,
            "instruction " + std::string(aarch64::instruction_id_name(instruction.id)) + " at " +
                hex(instruction.address, 16) + " uses a non-scalar register"));
    }
    return Result<IrType>::success(reg.width == RegisterWidth::W32 ? IrType::I32 : IrType::I64);
}

[[nodiscard]] Result<void> unsupported(const DecodedInstruction& instruction, ErrorCode code,
                                       std::string reason)
{
    return Result<void>::failure(make_error(
        code, "unsupported semantic operation at guest " + hex(instruction.address, 16) +
                 " source opcode " + hex(instruction.opcode, 8) + " (" +
                 std::string(aarch64::instruction_id_name(instruction.id)) + "): " +
                 std::move(reason)));
}

[[nodiscard]] Result<void> append(IrBuilder& builder, BlockId block, IrInstruction instruction)
{
    return builder.append(block, std::move(instruction));
}

[[nodiscard]] Result<ValueId> append_value(IrBuilder& builder, BlockId block, IrInstruction instruction)
{
    const auto id = builder.create_value();
    instruction.result = id;
    const auto added = append(builder, block, std::move(instruction));
    if (!added)
    {
        return Result<ValueId>::failure(added.error());
    }
    return Result<ValueId>::success(id);
}

[[nodiscard]] Result<ValueId> read_register(IrBuilder& builder, BlockId block, const Register& reg,
                                             const DecodedInstruction& source)
{
    const auto type = register_type(reg, source);
    if (!type)
    {
        return Result<ValueId>::failure(type.error());
    }
    return append_value(builder, block,
                        IrInstruction{IrOpcode::ReadRegister, type.value(), {}, {}, 0U, reg, 0U,
                                      false, {}, {}, {}, source_of(source)});
}

[[nodiscard]] Result<ValueId> constant(IrBuilder& builder, BlockId block, IrType type,
                                        std::uint64_t value, const DecodedInstruction& source)
{
    return append_value(builder, block,
                        IrInstruction{IrOpcode::Constant, type, {}, {}, value, {}, 0U, false,
                                      {}, {}, {}, source_of(source)});
}

[[nodiscard]] Result<void> write_register(IrBuilder& builder, BlockId block, const Register& reg,
                                          ValueId value, const DecodedInstruction& source)
{
    const auto type = register_type(reg, source);
    if (!type)
    {
        return Result<void>::failure(type.error());
    }
    return append(builder, block,
                  IrInstruction{IrOpcode::WriteRegister, type.value(), {}, {value}, 0U, reg, 0U,
                                false, {}, {}, {}, source_of(source)});
}

[[nodiscard]] Result<void> lift_mov(IrBuilder& builder, BlockId block,
                                    const DecodedInstruction& instruction)
{
    if (instruction.operands.size() != 2U || instruction.operands[0].kind != OperandKind::Register)
    {
        return unsupported(instruction, ErrorCode::InvalidOperandCount,
                            "mov requires destination and source operands");
    }
    const auto destination = instruction.operands[0].reg;
    const auto type = register_type(destination, instruction);
    if (!type)
    {
        return Result<void>::failure(type.error());
    }
    Result<ValueId> value = Result<ValueId>::failure(
        make_error(ErrorCode::UnsupportedOperandForm, "uninitialized mov source"));
    if (instruction.operands[1].kind == OperandKind::Register)
    {
        const auto source_type = register_type(instruction.operands[1].reg, instruction);
        if (!source_type || source_type.value() != type.value())
        {
            return unsupported(instruction, ErrorCode::UnsupportedOperandForm,
                                "mov register widths must match");
        }
        value = read_register(builder, block, instruction.operands[1].reg, instruction);
    }
    else if (instruction.operands[1].kind == OperandKind::Immediate)
    {
        value = constant(builder, block, type.value(),
                         static_cast<std::uint64_t>(instruction.operands[1].immediate), instruction);
    }
    else
    {
        return unsupported(instruction, ErrorCode::UnsupportedOperandForm,
                            "mov source form is not supported");
    }
    if (!value)
    {
        return Result<void>::failure(value.error());
    }
    return write_register(builder, block, destination, value.value(), instruction);
}

[[nodiscard]] Result<void> lift_binary(IrBuilder& builder, BlockId block,
                                       const DecodedInstruction& instruction, IrOpcode opcode)
{
    if (instruction.operands.size() != 3U || instruction.operands[0].kind != OperandKind::Register ||
        instruction.operands[1].kind != OperandKind::Register)
    {
        return unsupported(instruction, ErrorCode::InvalidOperandCount,
                            "binary instruction requires destination, left and right operands");
    }
    const auto type = register_type(instruction.operands[0].reg, instruction);
    if (!type)
    {
        return Result<void>::failure(type.error());
    }
    const auto left_type = register_type(instruction.operands[1].reg, instruction);
    if (!left_type || left_type.value() != type.value())
    {
        return unsupported(instruction, ErrorCode::UnsupportedOperandForm,
                            "binary register widths must match");
    }
    const auto left = read_register(builder, block, instruction.operands[1].reg, instruction);
    if (!left)
    {
        return Result<void>::failure(left.error());
    }
    Result<ValueId> right = Result<ValueId>::failure(
        make_error(ErrorCode::UnsupportedOperandForm, "uninitialized binary source"));
    if (instruction.operands[2].kind == OperandKind::Register)
    {
        const auto right_type = register_type(instruction.operands[2].reg, instruction);
        if (!right_type || right_type.value() != type.value())
        {
            return unsupported(instruction, ErrorCode::UnsupportedOperandForm,
                                "binary register widths must match");
        }
        right = read_register(builder, block, instruction.operands[2].reg, instruction);
    }
    else if (instruction.operands[2].kind == OperandKind::Immediate &&
             (opcode == IrOpcode::Add || opcode == IrOpcode::Sub))
    {
        right = constant(builder, block, type.value(),
                         static_cast<std::uint64_t>(instruction.operands[2].immediate), instruction);
    }
    else
    {
        return unsupported(instruction, ErrorCode::UnsupportedOperandForm,
                            "only register RHS is supported for logical operations and only immediate "
                            "RHS is supported for add/sub");
    }
    if (!right)
    {
        return Result<void>::failure(right.error());
    }
    const auto result = append_value(builder, block,
                                     IrInstruction{opcode, type.value(), {},
                                                   {left.value(), right.value()}, 0U, {}, 0U, false,
                                                   {}, {}, {}, source_of(instruction)});
    if (!result)
    {
        return Result<void>::failure(result.error());
    }
    return write_register(builder, block, instruction.operands[0].reg, result.value(), instruction);
}

[[nodiscard]] Result<void> lift_mov_wide(IrBuilder& builder, BlockId block,
                                         const DecodedInstruction& instruction, bool keep)
{
    if (instruction.operands.size() != 2U || instruction.operands[0].kind != OperandKind::Register ||
        instruction.operands[1].kind != OperandKind::Immediate)
    {
        return unsupported(instruction, ErrorCode::UnsupportedOperandForm,
                            "movz/movk requires register and immediate operands");
    }
    const auto type = register_type(instruction.operands[0].reg, instruction);
    if (!type)
    {
        return Result<void>::failure(type.error());
    }
    const unsigned int shift = ((instruction.opcode >> 21U) & 0x3U) * 16U;
    if (type.value() == IrType::I32 && shift >= 32U)
    {
        return unsupported(instruction, ErrorCode::UnsupportedOperandForm,
                            "movz/movk shift does not fit a W register");
    }
    const auto immediate = static_cast<std::uint64_t>(instruction.operands[1].immediate) & 0xffffU;
    const auto part = constant(builder, block, type.value(), immediate << shift, instruction);
    if (!part)
    {
        return Result<void>::failure(part.error());
    }
    ValueId value = part.value();
    if (keep)
    {
        const auto old = read_register(builder, block, instruction.operands[0].reg, instruction);
        if (!old)
        {
            return Result<void>::failure(old.error());
        }
        const auto full_mask = type.value() == IrType::I32 ? std::uint64_t{0xffffffffU}
                                                            : std::numeric_limits<std::uint64_t>::max();
        const auto field_mask = (std::uint64_t{0xffffU} << shift) & full_mask;
        const auto preserved = constant(builder, block, type.value(), full_mask & ~field_mask, instruction);
        if (!preserved)
        {
            return Result<void>::failure(preserved.error());
        }
        const auto kept = append_value(builder, block,
                                       IrInstruction{IrOpcode::And, type.value(), {},
                                                     {old.value(), preserved.value()}, 0U, {}, 0U,
                                                     false, {}, {}, {}, source_of(instruction)});
        if (!kept)
        {
            return Result<void>::failure(kept.error());
        }
        const auto combined = append_value(builder, block,
                                           IrInstruction{IrOpcode::Or, type.value(), {},
                                                         {kept.value(), part.value()}, 0U, {}, 0U,
                                                         false, {}, {}, {}, source_of(instruction)});
        if (!combined)
        {
            return Result<void>::failure(combined.error());
        }
        value = combined.value();
    }
    return write_register(builder, block, instruction.operands[0].reg, value, instruction);
}

[[nodiscard]] Result<void> lift_memory(IrBuilder& builder, BlockId block,
                                       const DecodedInstruction& instruction, bool is_load)
{
    if (instruction.operands.size() != 2U || instruction.operands[0].kind != OperandKind::Register ||
        instruction.operands[1].kind != OperandKind::Memory)
    {
        return unsupported(instruction, ErrorCode::InvalidOperandCount,
                            "memory instruction requires a data register and memory operand");
    }
    const auto& memory = instruction.operands[1].memory;
    if (memory.addressing != MemoryAddressingMode::Base || memory.writeback ||
        (memory.index.kind != RegisterKind::System && memory.index.valid()) || memory.shift != 0U)
    {
        return unsupported(instruction, ErrorCode::UnsupportedAddressingMode,
                            "only base plus immediate displacement without writeback is supported");
    }
    const auto address = read_register(builder, block, memory.base, instruction);
    if (!address)
    {
        return Result<void>::failure(address.error());
    }
    const auto address_type = register_type(memory.base, instruction);
    if (!address_type || address_type.value() != IrType::I64)
    {
        return unsupported(instruction, ErrorCode::UnsupportedOperandForm,
                            "memory base must be an X register or SP");
    }
    const auto data_type = register_type(instruction.operands[0].reg, instruction);
    if (!data_type)
    {
        return Result<void>::failure(data_type.error());
    }
    const auto access_size = data_type.value() == IrType::I32 ? std::uint8_t{4U} :
                             data_type.value() == IrType::I64 ? std::uint8_t{8U} : std::uint8_t{0U};
    if (access_size == 0U)
    {
        return unsupported(instruction, ErrorCode::InvalidRegisterWidth,
                            "only 32-bit and 64-bit scalar memory accesses are supported");
    }
    const auto displacement = constant(builder, block, IrType::I64,
                                       static_cast<std::uint64_t>(memory.displacement), instruction);
    if (!displacement)
    {
        return Result<void>::failure(displacement.error());
    }
    const auto effective = append_value(builder, block,
                                        IrInstruction{IrOpcode::Add, IrType::I64, {},
                                                      {address.value(), displacement.value()}, 0U, {}, 0U,
                                                      false, {}, {}, {}, source_of(instruction)});
    if (!effective)
    {
        return Result<void>::failure(effective.error());
    }
    if (is_load)
    {
        const auto loaded = append_value(builder, block,
                                         IrInstruction{IrOpcode::GuestLoad, data_type.value(), {},
                                                       {effective.value()}, 0U, {}, access_size, false,
                                                       {}, {}, {}, source_of(instruction)});
        if (!loaded)
        {
            return Result<void>::failure(loaded.error());
        }
        return write_register(builder, block, instruction.operands[0].reg, loaded.value(), instruction);
    }
    const auto value = read_register(builder, block, instruction.operands[0].reg, instruction);
    if (!value)
    {
        return Result<void>::failure(value.error());
    }
    return append(builder, block,
                  IrInstruction{IrOpcode::GuestStore, data_type.value(), {},
                                {effective.value(), value.value()}, 0U, {}, access_size, false,
                                {}, {}, {}, source_of(instruction)});
}

[[nodiscard]] Result<BlockId> target_block(const std::map<aarch64::GuestAddress, BlockId>& blocks,
                                           const DecodedInstruction& instruction,
                                           aarch64::GuestAddress target)
{
    const auto found = blocks.find(target);
    if (found == blocks.end())
    {
        return Result<BlockId>::failure(make_error(
            ErrorCode::InvalidBranchTarget,
            "lifted branch at guest " + hex(instruction.address, 16) + " targets " +
                hex(target, 16) + ", which is not an IR block"));
    }
    return Result<BlockId>::success(found->second);
}

[[nodiscard]] Result<void> lift_terminator(IrBuilder& builder, BlockId block,
                                            const DecodedInstruction& instruction,
                                            const std::map<aarch64::GuestAddress, BlockId>& blocks)
{
    if (instruction.id == InstructionId::Ret)
    {
        return append(builder, block,
                      IrInstruction{IrOpcode::Return, IrType::I1, {}, {}, 0U, {}, 0U, false,
                                    {}, {}, {}, source_of(instruction)});
    }
    if (instruction.id == InstructionId::B)
    {
        if (!instruction.control_flow.target)
        {
            return unsupported(instruction, ErrorCode::InvalidBranchTarget,
                                "direct branch has no target");
        }
        const auto target = target_block(blocks, instruction, *instruction.control_flow.target);
        if (!target)
        {
            return Result<void>::failure(target.error());
        }
        return append(builder, block,
                      IrInstruction{IrOpcode::Branch, IrType::I1, {}, {}, 0U, {}, 0U, false,
                                    target.value(), {}, {}, source_of(instruction)});
    }
    if (instruction.id == InstructionId::Cbz || instruction.id == InstructionId::Cbnz)
    {
        if (instruction.operands.size() != 2U ||
            instruction.operands[0].kind != OperandKind::Register ||
            instruction.operands[1].kind != OperandKind::Immediate ||
            !instruction.control_flow.target)
        {
            return unsupported(instruction, ErrorCode::InvalidOperandCount,
                                "cbz/cbnz requires one register and a direct target");
        }
        const auto value = read_register(builder, block, instruction.operands[0].reg, instruction);
        if (!value)
        {
            return Result<void>::failure(value.error());
        }
        const auto type = register_type(instruction.operands[0].reg, instruction);
        if (!type)
        {
            return Result<void>::failure(type.error());
        }
        const auto zero = constant(builder, block, type.value(), 0U, instruction);
        if (!zero)
        {
            return Result<void>::failure(zero.error());
        }
        const auto comparison = append_value(
            builder, block,
            IrInstruction{instruction.id == InstructionId::Cbz ? IrOpcode::CompareEqual
                                                                : IrOpcode::CompareNotEqual,
                          IrType::I1, {}, {value.value(), zero.value()}, 0U, {}, 0U, false, {}, {},
                          {}, source_of(instruction)});
        if (!comparison)
        {
            return Result<void>::failure(comparison.error());
        }
        const auto taken = target_block(blocks, instruction, *instruction.control_flow.target);
        if (!taken)
        {
            return Result<void>::failure(taken.error());
        }
        const auto fallthrough = checked_add_u64(instruction.address, 4U);
        if (!fallthrough)
        {
            return Result<void>::failure(fallthrough.error());
        }
        const auto not_taken = target_block(blocks, instruction, fallthrough.value());
        if (!not_taken)
        {
            return Result<void>::failure(not_taken.error());
        }
        return append(builder, block,
                      IrInstruction{IrOpcode::ConditionalBranch, IrType::I1, {}, {comparison.value()},
                                    0U, {}, 0U, false, {}, taken.value(), not_taken.value(),
                                    source_of(instruction)});
    }
    return unsupported(instruction, ErrorCode::UnsupportedInstruction,
                       "instruction is not a supported standalone terminator");
}

[[nodiscard]] Result<void> lift_instruction(IrBuilder& builder, BlockId block,
                                            const DecodedInstruction& instruction,
                                            const std::map<aarch64::GuestAddress, BlockId>& blocks)
{
    switch (instruction.id)
    {
    case InstructionId::Nop:
        return append(builder, block,
                      IrInstruction{IrOpcode::Nop, IrType::I1, {}, {}, 0U, {}, 0U, false,
                                    {}, {}, {}, source_of(instruction)});
    case InstructionId::Mov:
        return lift_mov(builder, block, instruction);
    case InstructionId::Movz:
        return lift_mov_wide(builder, block, instruction, false);
    case InstructionId::Movk:
        return lift_mov_wide(builder, block, instruction, true);
    case InstructionId::Add:
        return lift_binary(builder, block, instruction, IrOpcode::Add);
    case InstructionId::Sub:
        return lift_binary(builder, block, instruction, IrOpcode::Sub);
    case InstructionId::And:
        return lift_binary(builder, block, instruction, IrOpcode::And);
    case InstructionId::Orr:
        return lift_binary(builder, block, instruction, IrOpcode::Or);
    case InstructionId::Eor:
        return lift_binary(builder, block, instruction, IrOpcode::Xor);
    case InstructionId::Ldr:
    case InstructionId::Ldur:
        return lift_memory(builder, block, instruction, true);
    case InstructionId::Str:
    case InstructionId::Stur:
        return lift_memory(builder, block, instruction, false);
    case InstructionId::B:
    case InstructionId::Cbz:
    case InstructionId::Cbnz:
    case InstructionId::Ret:
        return lift_terminator(builder, block, instruction, blocks);
    default:
        return unsupported(instruction, ErrorCode::UnsupportedInstruction,
                            "instruction is outside the Milestone 6 semantic subset");
    }
}

} // namespace

Result<ir::IrFunction> lift_aarch64(const analysis::ControlFlowGraph& graph,
                                    const LiftOptions& options)
{
    const auto valid = analysis::validate_control_flow_graph(graph);
    if (!valid)
    {
        return Result<IrFunction>::failure(make_error(
            ErrorCode::InvalidCfg, "cannot lift an invalid CFG: " + valid.error().message));
    }
    IrFunction function;
    const auto entry_hex = hex(graph.entry, 16);
    function.name = options.function_name.empty()
                        ? "switchrecomp_guest_fn_" + entry_hex.substr(2U)
                        : options.function_name;
    function.entry = graph.entry;
    IrBuilder builder(function);
    std::map<aarch64::GuestAddress, BlockId> block_ids;
    for (const auto& [address, block] : graph.blocks)
    {
        const auto created = builder.create_block(block.start);
        if (!created)
        {
            return Result<IrFunction>::failure(created.error());
        }
        block_ids.emplace(address, created.value());
        if (address == graph.entry)
        {
            function.entry_block = created.value();
        }
    }
    for (const auto& [address, block] : graph.blocks)
    {
        const auto id = block_ids.at(address);
        for (const auto& instruction : block.instructions)
        {
            const auto lifted = lift_instruction(builder, id, instruction, block_ids);
            if (!lifted)
            {
                return Result<IrFunction>::failure(lifted.error());
            }
        }
        // The CFG may end a block at a leader boundary after a normal
        // fallthrough instruction.  Make that edge explicit in the semantic
        // IR; an IR block must never rely on implicit fallthrough.
        if (!block.instructions.empty() &&
            block.instructions.back().control_flow.kind == aarch64::ControlFlowKind::Fallthrough)
        {
            if (block.successors.size() != 1U ||
                block.successors.front().kind != analysis::EdgeKind::Fallthrough ||
                !block.successors.front().internal)
            {
                return Result<IrFunction>::failure(make_error(
                    ErrorCode::InvalidCfg,
                    "fallthrough block at guest " + hex(address, 16) +
                        " does not have one internal successor"));
            }
            const auto target = target_block(block_ids, block.instructions.back(),
                                             block.successors.front().target);
            if (!target)
            {
                return Result<IrFunction>::failure(target.error());
            }
            const auto& source = block.instructions.back();
            const auto edge = append(builder, id,
                                     IrInstruction{IrOpcode::Branch, IrType::I1, {}, {}, 0U, {}, 0U,
                                                   false, target.value(), {}, {}, source_of(source)});
            if (!edge)
            {
                return Result<IrFunction>::failure(edge.error());
            }
        }
    }
    const auto verified = ir::verify_function(function);
    if (!verified)
    {
        return Result<IrFunction>::failure(verified.error());
    }
    return Result<IrFunction>::success(std::move(function));
}

} // namespace switchrecomp::lift
