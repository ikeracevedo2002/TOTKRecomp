#include "switchrecomp/lifter/lifter.hpp"

namespace switchrecomp::lifter
{

[[nodiscard]] Result<ir::Function> lift_function_m9_injected(
    const analysis::ControlFlowGraph& cfg, const LiftOptions& options);

namespace
{
[[nodiscard]] bool needs_m9_injection(const analysis::ControlFlowGraph& cfg) noexcept
{
    for (const auto& [_, block] : cfg.blocks)
    {
        for (const auto& instruction : block.instructions)
        {
            switch (instruction.id)
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
            case aarch64::InstructionId::Mrs: case aarch64::InstructionId::Msr:
                return true;
            default:
                break;
            }
        }
    }
    return false;
}
} // namespace

Result<ir::Function> lift_function(const analysis::ControlFlowGraph& cfg,
                                   const LiftOptions& options)
{
    if (!needs_m9_injection(cfg))
        return lift_function_legacy(cfg, options);
    return lift_function_m9_injected(cfg, options);
}

} // namespace switchrecomp::lifter
