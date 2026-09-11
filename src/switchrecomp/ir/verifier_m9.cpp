#include "switchrecomp/ir/verifier.hpp"

#include <cstddef>
#include <string>

namespace switchrecomp::ir
{
namespace
{
[[nodiscard]] Result<void> invalid(std::string message)
{
    return Result<void>::failure(make_error(ErrorCode::IrVerificationFailed, std::move(message)));
}

[[nodiscard]] bool valid_width(std::uint8_t width) noexcept
{
    return width == 1U || width == 2U || width == 4U || width == 8U;
}

[[nodiscard]] Type type_for_width(std::uint8_t width) noexcept
{
    switch (width)
    {
    case 1U: return i8_type();
    case 2U: return i16_type();
    case 4U: return i32_type();
    case 8U: return i64_type();
    default: return void_type();
    }
}

[[nodiscard]] bool valid_load_order(MemoryOrder order) noexcept
{
    return order == MemoryOrder::Relaxed || order == MemoryOrder::Acquire ||
           order == MemoryOrder::SequentiallyConsistent;
}

[[nodiscard]] bool valid_store_order(MemoryOrder order) noexcept
{
    return order == MemoryOrder::Relaxed || order == MemoryOrder::Release ||
           order == MemoryOrder::SequentiallyConsistent;
}

[[nodiscard]] bool valid_barrier_kind(BarrierKind kind) noexcept
{
    return kind == BarrierKind::Dmb || kind == BarrierKind::Dsb || kind == BarrierKind::Isb;
}

[[nodiscard]] bool valid_barrier_option(BarrierOption option) noexcept
{
    return static_cast<std::uint8_t>(option) <= static_cast<std::uint8_t>(BarrierOption::Oshld);
}

[[nodiscard]] bool valid_system_register(SystemRegister reg) noexcept
{
    return reg == SystemRegister::TpidrEl0 || reg == SystemRegister::TpidrroEl0;
}

[[nodiscard]] Result<Type> operand_type(const Function& function, const Instruction& instruction,
                                        std::size_t index)
{
    if (index >= instruction.operands.size())
        return Result<Type>::failure(make_error(ErrorCode::IrVerificationFailed,
            "Milestone 9 instruction is missing an operand"));
    const auto* definition = function.value(instruction.operands[index]);
    if (definition == nullptr)
        return Result<Type>::failure(make_error(ErrorCode::IrVerificationFailed,
            "Milestone 9 instruction references an invalid operand"));
    return Result<Type>::success(definition->type);
}

[[nodiscard]] Result<void> verify_m9_instruction(const Function& function,
                                                 const Instruction& instruction)
{
    const auto require_address = [&]() -> Result<void> {
        const auto type = operand_type(function, instruction, 0U);
        if (!type || type.value() != i64_type())
            return invalid("atomic/exclusive guest address must be i64");
        return Result<void>::success();
    };

    switch (instruction.opcode)
    {
    case Opcode::AtomicLoad:
    case Opcode::ExclusiveLoad:
    {
        if (instruction.operands.size() != 1U)
            return invalid("atomic/exclusive load requires exactly one address operand");
        if (!valid_width(instruction.memory_size))
            return invalid("atomic/exclusive load uses an invalid access width");
        const auto address = require_address(); if (!address) return address;
        if (instruction.result == invalid_value || instruction.result_type != type_for_width(instruction.memory_size))
            return invalid("atomic/exclusive load result type must match its access width");
        if (!valid_load_order(instruction.memory_order))
            return invalid("atomic/exclusive load has an invalid memory order");
        return Result<void>::success();
    }
    case Opcode::AtomicStore:
    {
        if (instruction.operands.size() != 2U || instruction.result != invalid_value ||
            !instruction.result_type.is_void())
            return invalid("atomic store requires address/value operands and no result");
        if (!valid_width(instruction.memory_size))
            return invalid("atomic store uses an invalid access width");
        const auto address = require_address(); if (!address) return address;
        const auto value = operand_type(function, instruction, 1U);
        if (!value || value.value() != type_for_width(instruction.memory_size))
            return invalid("atomic store value type must match its access width");
        if (!valid_store_order(instruction.memory_order))
            return invalid("atomic store has an invalid memory order");
        return Result<void>::success();
    }
    case Opcode::ExclusiveStore:
    {
        if (instruction.operands.size() != 2U)
            return invalid("exclusive store requires address and value operands");
        if (!valid_width(instruction.memory_size))
            return invalid("exclusive store uses an invalid access width");
        const auto address = require_address(); if (!address) return address;
        const auto value = operand_type(function, instruction, 1U);
        if (!value || value.value() != type_for_width(instruction.memory_size))
            return invalid("exclusive store value type must match its access width");
        if (instruction.result == invalid_value || instruction.result_type != i32_type())
            return invalid("exclusive store status result must be i32");
        if (!valid_store_order(instruction.memory_order))
            return invalid("exclusive store has an invalid memory order");
        return Result<void>::success();
    }
    case Opcode::ClearExclusive:
        if (!instruction.operands.empty() || instruction.result != invalid_value ||
            !instruction.result_type.is_void())
            return invalid("clear_exclusive must have no operands or result");
        return Result<void>::success();
    case Opcode::MemoryBarrier:
        if (!instruction.operands.empty() || instruction.result != invalid_value ||
            !instruction.result_type.is_void())
            return invalid("memory_barrier must have no value operands or result");
        if (!valid_barrier_kind(instruction.barrier_kind) ||
            !valid_barrier_option(instruction.barrier_option))
            return invalid("memory_barrier contains invalid normalized barrier data");
        return Result<void>::success();
    case Opcode::ReadSystemRegister:
        if (!instruction.operands.empty() || instruction.result == invalid_value ||
            instruction.result_type != i64_type())
            return invalid("system-register read must produce one i64 result");
        if (!valid_system_register(instruction.system_register))
            return invalid("system-register read references an unsupported register");
        return Result<void>::success();
    case Opcode::WriteSystemRegister:
    {
        if (instruction.operands.size() != 1U || instruction.result != invalid_value ||
            !instruction.result_type.is_void())
            return invalid("system-register write requires one operand and no result");
        const auto value = operand_type(function, instruction, 0U);
        if (!value || value.value() != i64_type())
            return invalid("system-register write operand must be i64");
        if (instruction.system_register != SystemRegister::TpidrEl0)
            return invalid("only TPIDR_EL0 is writable in Milestone 9");
        return Result<void>::success();
    }
    default:
        return Result<void>::success();
    }
}
} // namespace

Result<void> verify(const Function& function)
{
    const auto legacy = verify_legacy(function);
    if (!legacy) return legacy;
    for (const auto& block : function.blocks())
        for (const auto& instruction : block.instructions)
        {
            const auto checked = verify_m9_instruction(function, instruction);
            if (!checked) return checked;
        }
    return Result<void>::success();
}

} // namespace switchrecomp::ir
