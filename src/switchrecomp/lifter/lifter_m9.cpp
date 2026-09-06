#include "switchrecomp/lifter/lifter.hpp"

#include "switchrecomp/ir/verifier.hpp"

#include <map>
#include <utility>
#include <vector>

namespace switchrecomp::lifter
{
namespace
{
[[nodiscard]] bool is_m9_supported(aarch64::InstructionId id) noexcept
{
    switch (id)
    {
    case aarch64::InstructionId::Ldxr: case aarch64::InstructionId::Ldxrb:
    case aarch64::InstructionId::Ldxrh: case aarch64::InstructionId::Ldaxr:
    case aarch64::InstructionId::Ldaxrb: case aarch64::InstructionId::Ldaxrh:
    case aarch64::InstructionId::Stxr: case aarch64::InstructionId::Stxrb:
    case aarch64::InstructionId::Stxrh: case aarch64::InstructionId::Stlxr:
    case aarch64::InstructionId::Stlxrb: case aarch64::InstructionId::Stlxrh:
    case aarch64::InstructionId::Ldar: case aarch64::InstructionId::Ldarb:
    case aarch64::InstructionId::Ldarh: case aarch64::InstructionId::Stlr:
    case aarch64::InstructionId::Stlrb: case aarch64::InstructionId::Stlrh:
    case aarch64::InstructionId::Clrex: case aarch64::InstructionId::Dmb:
    case aarch64::InstructionId::Dsb: case aarch64::InstructionId::Isb:
        return true;
    case aarch64::InstructionId::Mrs:
    case aarch64::InstructionId::Msr:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] ir::MemoryOrder order_for(aarch64::AtomicMemoryOrder order) noexcept
{
    switch (order)
    {
    case aarch64::AtomicMemoryOrder::Acquire: return ir::MemoryOrder::Acquire;
    case aarch64::AtomicMemoryOrder::Release: return ir::MemoryOrder::Release;
    case aarch64::AtomicMemoryOrder::Relaxed: return ir::MemoryOrder::Relaxed;
    }
    return ir::MemoryOrder::Relaxed;
}

[[nodiscard]] ir::BarrierOption barrier_option_for(aarch64::BarrierOption option) noexcept
{
    switch (option)
    {
    case aarch64::BarrierOption::Sy: return ir::BarrierOption::Sy;
    case aarch64::BarrierOption::St: return ir::BarrierOption::St;
    case aarch64::BarrierOption::Ld: return ir::BarrierOption::Ld;
    case aarch64::BarrierOption::Ish: return ir::BarrierOption::Ish;
    case aarch64::BarrierOption::Ishst: return ir::BarrierOption::Ishst;
    case aarch64::BarrierOption::Ishld: return ir::BarrierOption::Ishld;
    case aarch64::BarrierOption::Nsh: return ir::BarrierOption::Nsh;
    case aarch64::BarrierOption::Nshst: return ir::BarrierOption::Nshst;
    case aarch64::BarrierOption::Nshld: return ir::BarrierOption::Nshld;
    case aarch64::BarrierOption::Osh: return ir::BarrierOption::Osh;
    case aarch64::BarrierOption::Oshst: return ir::BarrierOption::Oshst;
    case aarch64::BarrierOption::Oshld: return ir::BarrierOption::Oshld;
    case aarch64::BarrierOption::Invalid: return ir::BarrierOption::Sy;
    }
    return ir::BarrierOption::Sy;
}

[[nodiscard]] ir::GuestRegister register_for(const aarch64::Register& reg)
{
    return ir::GuestRegister{
        reg.width == aarch64::RegisterWidth::W32 ? ir::RegisterWidth::W32 : ir::RegisterWidth::X64,
        reg.index, reg.is_stack_pointer, reg.is_zero};
}

[[nodiscard]] ir::Type type_for_size(std::uint8_t size)
{
    switch (size)
    {
    case 1U: return ir::i8_type();
    case 2U: return ir::i16_type();
    case 4U: return ir::i32_type();
    default: return ir::i64_type();
    }
}

[[nodiscard]] const aarch64::Operand* memory_operand(const aarch64::DecodedInstruction& instruction)
{
    for (const auto& operand : instruction.operands)
        if (operand.kind == aarch64::OperandKind::Memory) return &operand;
    return nullptr;
}

[[nodiscard]] const aarch64::Operand* first_register_operand(const aarch64::DecodedInstruction& instruction,
                                                             std::size_t ordinal)
{
    std::size_t current = 0U;
    for (const auto& operand : instruction.operands)
    {
        if (operand.kind != aarch64::OperandKind::Register) continue;
        if (current++ == ordinal) return &operand;
    }
    return nullptr;
}

class Injector
{
  public:
    Injector(ir::Function& function, ir::BasicBlock& block, std::vector<ir::Instruction>& output,
             const aarch64::DecodedInstruction& guest)
        : function_(function), block_(block), output_(output), guest_(guest) {}

    [[nodiscard]] Result<void> inject()
    {
        switch (guest_.id)
        {
        case aarch64::InstructionId::Ldxr: case aarch64::InstructionId::Ldxrb:
        case aarch64::InstructionId::Ldxrh: case aarch64::InstructionId::Ldaxr:
        case aarch64::InstructionId::Ldaxrb: case aarch64::InstructionId::Ldaxrh:
            return exclusive_load();
        case aarch64::InstructionId::Stxr: case aarch64::InstructionId::Stxrb:
        case aarch64::InstructionId::Stxrh: case aarch64::InstructionId::Stlxr:
        case aarch64::InstructionId::Stlxrb: case aarch64::InstructionId::Stlxrh:
            return exclusive_store();
        case aarch64::InstructionId::Ldar: case aarch64::InstructionId::Ldarb:
        case aarch64::InstructionId::Ldarh:
            return atomic_load();
        case aarch64::InstructionId::Stlr: case aarch64::InstructionId::Stlrb:
        case aarch64::InstructionId::Stlrh:
            return atomic_store();
        case aarch64::InstructionId::Clrex:
        {
            ir::Instruction op; op.opcode = ir::Opcode::ClearExclusive; op.result_type = ir::void_type();
            op.source = source(); output_.push_back(std::move(op)); return Result<void>::success();
        }
        case aarch64::InstructionId::Dmb: case aarch64::InstructionId::Dsb:
        case aarch64::InstructionId::Isb:
        {
            if (guest_.barrier_option == aarch64::BarrierOption::Invalid)
                return Result<void>::failure(make_error(ErrorCode::UnsupportedOperandForm,
                    "AArch64 barrier has an unsupported option"));
            ir::Instruction op; op.opcode = ir::Opcode::MemoryBarrier; op.result_type = ir::void_type();
            op.barrier_kind = guest_.id == aarch64::InstructionId::Dmb ? ir::BarrierKind::Dmb
                            : guest_.id == aarch64::InstructionId::Dsb ? ir::BarrierKind::Dsb : ir::BarrierKind::Isb;
            op.barrier_option = barrier_option_for(guest_.barrier_option); op.source = source();
            output_.push_back(std::move(op)); return Result<void>::success();
        }
        case aarch64::InstructionId::Mrs: return read_system_register();
        case aarch64::InstructionId::Msr: return write_system_register();
        default:
            return Result<void>::failure(make_error(ErrorCode::UnsupportedInstruction,
                "Milestone 9 injector received an unsupported instruction"));
        }
    }

  private:
    [[nodiscard]] ir::SourceLocation source() const
    {
        return ir::SourceLocation{guest_.address, guest_.opcode, guest_.disassembly};
    }

    [[nodiscard]] Result<ir::ValueId> emit_value(ir::Instruction op)
    {
        op.source = source();
        const auto index = static_cast<std::uint32_t>(output_.size());
        const auto id = function_.add_value(ir::ValueKind::Instruction, op.result_type, block_.id, index);
        op.result = id;
        output_.push_back(std::move(op));
        return Result<ir::ValueId>::success(id);
    }

    void emit_void(ir::Instruction op)
    {
        op.result = ir::invalid_value; op.result_type = ir::void_type(); op.source = source();
        output_.push_back(std::move(op));
    }

    [[nodiscard]] Result<ir::ValueId> read_register(const aarch64::Register& reg)
    {
        if (reg.kind != aarch64::RegisterKind::General ||
            (reg.width != aarch64::RegisterWidth::W32 && reg.width != aarch64::RegisterWidth::X64))
            return Result<ir::ValueId>::failure(make_error(ErrorCode::UnsupportedOperandForm,
                "atomic instruction requires a W/X register"));
        ir::Instruction op; op.opcode = ir::Opcode::ReadRegister;
        op.result_type = reg.width == aarch64::RegisterWidth::W32 ? ir::i32_type() : ir::i64_type();
        op.reg = register_for(reg); return emit_value(std::move(op));
    }

    void write_register(const aarch64::Register& reg, ir::ValueId value)
    {
        ir::Instruction op; op.opcode = ir::Opcode::WriteRegister; op.operands = {value};
        op.reg = register_for(reg); emit_void(std::move(op));
    }

    [[nodiscard]] Result<ir::ValueId> address()
    {
        const auto* mem = memory_operand(guest_);
        if (mem == nullptr || mem->memory.base.kind != aarch64::RegisterKind::General ||
            mem->memory.base.width != aarch64::RegisterWidth::X64 ||
            mem->memory.index.valid() || mem->memory.displacement != 0 || mem->memory.writeback)
            return Result<ir::ValueId>::failure(make_error(ErrorCode::UnsupportedOperandForm,
                "Milestone 9 atomic memory form requires [Xn] without writeback"));
        return read_register(mem->memory.base);
    }

    [[nodiscard]] Result<ir::ValueId> narrow_value(ir::ValueId value, ir::Type source_type,
                                                    std::uint8_t width)
    {
        const auto target = type_for_size(width);
        if (source_type == target) return Result<ir::ValueId>::success(value);
        ir::Instruction cast; cast.opcode = ir::Opcode::Truncate; cast.result_type = target;
        cast.operands = {value}; return emit_value(std::move(cast));
    }

    [[nodiscard]] Result<ir::ValueId> widen_load(ir::ValueId value, std::uint8_t width,
                                                 const aarch64::Register& destination)
    {
        const auto target = destination.width == aarch64::RegisterWidth::W32 ? ir::i32_type() : ir::i64_type();
        const auto source_type = type_for_size(width);
        if (source_type == target) return Result<ir::ValueId>::success(value);
        ir::Instruction cast; cast.opcode = ir::Opcode::ZeroExtend; cast.result_type = target;
        cast.operands = {value}; return emit_value(std::move(cast));
    }

    [[nodiscard]] Result<void> exclusive_load()
    {
        const auto* destination = first_register_operand(guest_, 0U);
        if (destination == nullptr || guest_.atomic_width == 0U)
            return Result<void>::failure(make_error(ErrorCode::UnsupportedOperandForm,
                "exclusive load has no valid destination or width"));
        const auto addr = address(); if (!addr) return Result<void>::failure(addr.error());
        ir::Instruction op; op.opcode = ir::Opcode::ExclusiveLoad; op.result_type = type_for_size(guest_.atomic_width);
        op.operands = {addr.value()}; op.memory_size = guest_.atomic_width; op.memory_order = order_for(guest_.memory_order);
        const auto loaded = emit_value(std::move(op)); if (!loaded) return Result<void>::failure(loaded.error());
        const auto widened = widen_load(loaded.value(), guest_.atomic_width, destination->reg);
        if (!widened) return Result<void>::failure(widened.error());
        write_register(destination->reg, widened.value()); return Result<void>::success();
    }

    [[nodiscard]] Result<void> exclusive_store()
    {
        const auto* status = first_register_operand(guest_, 0U);
        const auto* value_reg = first_register_operand(guest_, 1U);
        if (status == nullptr || value_reg == nullptr || status->reg.width != aarch64::RegisterWidth::W32 ||
            guest_.atomic_width == 0U)
            return Result<void>::failure(make_error(ErrorCode::UnsupportedOperandForm,
                "exclusive store requires Ws, Wt/Xt, [Xn]"));
        const auto addr = address(); const auto value = read_register(value_reg->reg);
        if (!addr || !value) return Result<void>::failure(!addr ? addr.error() : value.error());
        const auto source_type = value_reg->reg.width == aarch64::RegisterWidth::W32 ? ir::i32_type() : ir::i64_type();
        const auto narrowed = narrow_value(value.value(), source_type, guest_.atomic_width);
        if (!narrowed) return Result<void>::failure(narrowed.error());
        ir::Instruction op; op.opcode = ir::Opcode::ExclusiveStore; op.result_type = ir::i32_type();
        op.operands = {addr.value(), narrowed.value()}; op.memory_size = guest_.atomic_width;
        op.memory_order = order_for(guest_.memory_order);
        const auto stored = emit_value(std::move(op)); if (!stored) return Result<void>::failure(stored.error());
        write_register(status->reg, stored.value()); return Result<void>::success();
    }

    [[nodiscard]] Result<void> atomic_load()
    {
        const auto* destination = first_register_operand(guest_, 0U);
        if (destination == nullptr || guest_.atomic_width == 0U)
            return Result<void>::failure(make_error(ErrorCode::UnsupportedOperandForm,
                "acquire load has no valid destination or width"));
        const auto addr = address(); if (!addr) return Result<void>::failure(addr.error());
        ir::Instruction op; op.opcode = ir::Opcode::AtomicLoad; op.result_type = type_for_size(guest_.atomic_width);
        op.operands = {addr.value()}; op.memory_size = guest_.atomic_width; op.memory_order = ir::MemoryOrder::Acquire;
        const auto loaded = emit_value(std::move(op)); if (!loaded) return Result<void>::failure(loaded.error());
        const auto widened = widen_load(loaded.value(), guest_.atomic_width, destination->reg);
        if (!widened) return Result<void>::failure(widened.error());
        write_register(destination->reg, widened.value()); return Result<void>::success();
    }

    [[nodiscard]] Result<void> atomic_store()
    {
        const auto* value_reg = first_register_operand(guest_, 0U);
        if (value_reg == nullptr || guest_.atomic_width == 0U)
            return Result<void>::failure(make_error(ErrorCode::UnsupportedOperandForm,
                "release store has no valid source or width"));
        const auto addr = address(); const auto value = read_register(value_reg->reg);
        if (!addr || !value) return Result<void>::failure(!addr ? addr.error() : value.error());
        const auto source_type = value_reg->reg.width == aarch64::RegisterWidth::W32 ? ir::i32_type() : ir::i64_type();
        const auto narrowed = narrow_value(value.value(), source_type, guest_.atomic_width);
        if (!narrowed) return Result<void>::failure(narrowed.error());
        ir::Instruction op; op.opcode = ir::Opcode::AtomicStore; op.operands = {addr.value(), narrowed.value()};
        op.memory_size = guest_.atomic_width; op.memory_order = ir::MemoryOrder::Release; emit_void(std::move(op));
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> read_system_register()
    {
        const auto* destination = first_register_operand(guest_, 0U);
        if (destination == nullptr || destination->reg.width != aarch64::RegisterWidth::X64 ||
            guest_.system_register == aarch64::SystemRegister::None)
            return Result<void>::failure(make_error(ErrorCode::UnsupportedSystemRegister,
                "MRS uses an unsupported system register"));
        ir::Instruction op; op.opcode = ir::Opcode::ReadSystemRegister; op.result_type = ir::i64_type();
        op.system_register = guest_.system_register == aarch64::SystemRegister::TpidrEl0
                                 ? ir::SystemRegister::TpidrEl0 : ir::SystemRegister::TpidrroEl0;
        const auto value = emit_value(std::move(op)); if (!value) return Result<void>::failure(value.error());
        write_register(destination->reg, value.value()); return Result<void>::success();
    }

    [[nodiscard]] Result<void> write_system_register()
    {
        const auto* source_reg = first_register_operand(guest_, 0U);
        if (source_reg == nullptr || source_reg->reg.width != aarch64::RegisterWidth::X64 ||
            guest_.system_register != aarch64::SystemRegister::TpidrEl0)
            return Result<void>::failure(make_error(ErrorCode::UnsupportedSystemRegister,
                "MSR supports only TPIDR_EL0 in Milestone 9"));
        const auto value = read_register(source_reg->reg); if (!value) return Result<void>::failure(value.error());
        ir::Instruction op; op.opcode = ir::Opcode::WriteSystemRegister; op.operands = {value.value()};
        op.system_register = ir::SystemRegister::TpidrEl0; emit_void(std::move(op));
        return Result<void>::success();
    }

    ir::Function& function_;
    ir::BasicBlock& block_;
    std::vector<ir::Instruction>& output_;
    const aarch64::DecodedInstruction& guest_;
};

[[nodiscard]] std::map<aarch64::GuestAddress, const aarch64::DecodedInstruction*>
index_concurrency(const analysis::ControlFlowGraph& cfg)
{
    std::map<aarch64::GuestAddress, const aarch64::DecodedInstruction*> result;
    for (const auto& [_, block] : cfg.blocks)
        for (const auto& instruction : block.instructions)
            if (is_m9_supported(instruction.id)) result.emplace(instruction.address, &instruction);
    return result;
}
} // namespace

Result<ir::Function> lift_function(const analysis::ControlFlowGraph& cfg, const LiftOptions& options)
{
    auto sanitized = cfg;
    for (auto& [_, block] : sanitized.blocks)
    {
        for (auto& instruction : block.instructions)
        {
            if (is_m9_supported(instruction.id))
            {
                instruction.id = aarch64::InstructionId::Nop;
                instruction.operands.clear();
                instruction.normalized = true;
            }
        }
    }
    auto legacy_options = options;
    legacy_options.verify_result = false;
    auto lifted = lift_function_legacy(sanitized, legacy_options);
    if (!lifted) return lifted;

    auto concurrency = index_concurrency(cfg);
    auto& function = lifted.value();
    for (auto& block : function.blocks())
    {
        std::vector<ir::Instruction> output;
        output.reserve(block.instructions.size() * 2U);
        for (auto& instruction : block.instructions)
        {
            const auto found = concurrency.find(instruction.source.guest_pc);
            if (instruction.opcode == ir::Opcode::Nop && found != concurrency.end())
            {
                Injector injector(function, block, output, *found->second);
                const auto injected = injector.inject();
                if (!injected) return Result<ir::Function>::failure(injected.error());
            }
            else
            {
                output.push_back(std::move(instruction));
            }
        }
        block.instructions = std::move(output);
        for (std::uint32_t index = 0U; index < block.instructions.size(); ++index)
        {
            const auto result = block.instructions[index].result;
            if (result != ir::invalid_value && result < function.values().size())
            {
                function.values()[result].defining_block = block.id;
                function.values()[result].instruction_index = index;
            }
        }
    }

    if (options.verify_result)
    {
        const auto verified = ir::verify(function);
        if (!verified) return Result<ir::Function>::failure(verified.error());
    }
    return lifted;
}

bool is_instruction_liftable(aarch64::InstructionId id) noexcept
{
    if (is_m9_supported(id)) return true;
    switch (id)
    {
    case aarch64::InstructionId::Ldxp: case aarch64::InstructionId::Ldaxp:
    case aarch64::InstructionId::Stxp: case aarch64::InstructionId::Stlxp:
        return false;
    default:
        return is_instruction_liftable_legacy(id);
    }
}

bool is_instruction_liftable(const aarch64::DecodedInstruction& instruction) noexcept
{
    if (!instruction.normalized) return false;
    if (instruction.id == aarch64::InstructionId::Mrs || instruction.id == aarch64::InstructionId::Msr)
    {
        return instruction.system_register == aarch64::SystemRegister::TpidrEl0 ||
               (instruction.id == aarch64::InstructionId::Mrs &&
                instruction.system_register == aarch64::SystemRegister::TpidrroEl0);
    }
    if ((instruction.id == aarch64::InstructionId::Dmb || instruction.id == aarch64::InstructionId::Dsb ||
         instruction.id == aarch64::InstructionId::Isb) &&
        instruction.barrier_option == aarch64::BarrierOption::Invalid)
        return false;
    if (is_m9_supported(instruction.id)) return true;
    return is_instruction_liftable_legacy(instruction);
}

} // namespace switchrecomp::lifter
