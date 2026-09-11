#include "switchrecomp/interpreter/m9_semantics.hpp"

namespace switchrecomp::interpreter
{
namespace
{
[[nodiscard]] Result<void> runtime_failure(const runtime::RuntimeContext& runtime)
{
    return Result<void>::failure(runtime.has_error
        ? runtime.last_error
        : make_error(ErrorCode::InterpreterError, "runtime helper failed without a diagnostic"));
}
}

bool is_m9_opcode(ir::Opcode opcode) noexcept
{
    switch (opcode)
    {
    case ir::Opcode::AtomicLoad:
    case ir::Opcode::AtomicStore:
    case ir::Opcode::ExclusiveLoad:
    case ir::Opcode::ExclusiveStore:
    case ir::Opcode::ClearExclusive:
    case ir::Opcode::MemoryBarrier:
    case ir::Opcode::ReadSystemRegister:
    case ir::Opcode::WriteSystemRegister:
        return true;
    default:
        return false;
    }
}

Result<void> execute_m9_instruction(const ir::Instruction& instruction,
                                    runtime::RuntimeContext& runtime,
                                    const M9ReadValue& read,
                                    const M9StoreValue& store)
{
    if (!is_m9_opcode(instruction.opcode))
    {
        return Result<void>::failure(make_error(
            ErrorCode::UnsupportedInstruction, "interpreter received a non-Milestone 9 opcode"));
    }

    switch (instruction.opcode)
    {
    case ir::Opcode::AtomicLoad:
    case ir::Opcode::ExclusiveLoad:
    {
        const auto address = read(instruction.operands[0]);
        if (!address) return Result<void>::failure(address.error());
        std::uint64_t loaded = 0U;
        const auto rc = instruction.opcode == ir::Opcode::AtomicLoad
            ? runtime::switchrecomp_runtime_atomic_load(
                  &runtime, address.value(), instruction.memory_size,
                  static_cast<std::uint8_t>(instruction.memory_order), &loaded)
            : runtime::switchrecomp_runtime_exclusive_load(
                  &runtime, address.value(), instruction.memory_size,
                  static_cast<std::uint8_t>(instruction.memory_order), &loaded);
        if (rc != 0U) return runtime_failure(runtime);
        return store(loaded, InterpreterValueProvenance{
            InterpreterValueProvenance::Kind::GuestLoad, address.value(), 0});
    }
    case ir::Opcode::AtomicStore:
    {
        const auto address = read(instruction.operands[0]);
        const auto value = read(instruction.operands[1]);
        if (!address || !value)
        {
            return Result<void>::failure(!address ? address.error() : value.error());
        }
        if (runtime::switchrecomp_runtime_atomic_store(
                &runtime, address.value(), instruction.memory_size, value.value(),
                static_cast<std::uint8_t>(instruction.memory_order)) != 0U)
        {
            return runtime_failure(runtime);
        }
        return Result<void>::success();
    }
    case ir::Opcode::ExclusiveStore:
    {
        const auto address = read(instruction.operands[0]);
        const auto value = read(instruction.operands[1]);
        if (!address || !value)
        {
            return Result<void>::failure(!address ? address.error() : value.error());
        }
        std::uint32_t status = 1U;
        if (runtime::switchrecomp_runtime_exclusive_store(
                &runtime, address.value(), instruction.memory_size, value.value(),
                static_cast<std::uint8_t>(instruction.memory_order), &status) != 0U)
        {
            return runtime_failure(runtime);
        }
        return store(status, {});
    }
    case ir::Opcode::ClearExclusive:
        if (runtime::switchrecomp_runtime_clear_exclusive(&runtime) != 0U)
            return runtime_failure(runtime);
        return Result<void>::success();
    case ir::Opcode::MemoryBarrier:
        if (runtime::switchrecomp_runtime_memory_barrier(
                &runtime, static_cast<std::uint8_t>(instruction.barrier_kind),
                static_cast<std::uint8_t>(instruction.barrier_option)) != 0U)
        {
            return runtime_failure(runtime);
        }
        return Result<void>::success();
    case ir::Opcode::ReadSystemRegister:
    {
        std::uint64_t value = 0U;
        if (runtime::switchrecomp_runtime_read_system_register(
                &runtime, static_cast<std::uint8_t>(instruction.system_register), &value) != 0U)
        {
            return runtime_failure(runtime);
        }
        return store(value, {});
    }
    case ir::Opcode::WriteSystemRegister:
    {
        const auto value = read(instruction.operands[0]);
        if (!value) return Result<void>::failure(value.error());
        if (runtime::switchrecomp_runtime_write_system_register(
                &runtime, static_cast<std::uint8_t>(instruction.system_register), value.value()) != 0U)
        {
            return runtime_failure(runtime);
        }
        return Result<void>::success();
    }
    default:
        return Result<void>::failure(make_error(
            ErrorCode::UnsupportedInstruction, "interpreter received a non-Milestone 9 opcode"));
    }
}

} // namespace switchrecomp::interpreter
