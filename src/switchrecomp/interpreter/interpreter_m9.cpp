#include "switchrecomp/interpreter/interpreter.hpp"

namespace switchrecomp::interpreter
{

// The M9 entry point shares the verified, cursor-based interpreter. Keeping a
// single execution state machine is important here: M9 semantic operations
// consume exactly one IR operation, and they must observe the same slice,
// provenance, and continuation rules as the earlier IR operations.
Result<runtime::ExecutionResult> execute(const ir::Function& function,
                                         runtime::CpuState& cpu,
                                         runtime::RuntimeContext& runtime,
                                         const runtime::ExecutionOptions& options)
{
    return execute_legacy(function, cpu, runtime, options);
}

} // namespace switchrecomp::interpreter
