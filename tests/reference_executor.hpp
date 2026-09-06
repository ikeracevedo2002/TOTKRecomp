#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/memory/guest_memory.hpp"
#include "switchrecomp/runtime/cpu_state.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace switchrecomp::test_reference
{

struct ReferenceExecutionResult
{
    runtime::CpuState state;
    std::size_t executed_instructions = 0U;
};

// Deliberately independent of Capstone, CFG and Semantic IR.  This tiny raw
// decoder is test-only and covers the synthetic scalar subset used for the
// differential fixtures; it is not a runtime emulator.
class AArch64ReferenceExecutor
{
  public:
    [[nodiscard]] Result<ReferenceExecutionResult> execute(
        std::span<const std::uint32_t> words, memory::GuestAddress base,
        runtime::CpuState initial_state, memory::GuestMemory& memory,
        std::size_t max_instructions = 1'000U) const;
};

} // namespace switchrecomp::test_reference
