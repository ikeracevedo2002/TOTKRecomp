#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/memory/guest_memory.hpp"
#include "switchrecomp/runtime/context.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace switchrecomp::runtime
{

// This is the generated-function ABI used by the current LLVM backend. The
// target remains a guest address until this registry performs the controlled
// dispatch boundary.
using GuestFunction = std::uint32_t (*)(CpuState*, RuntimeContext*);

struct FunctionRegistration
{
    memory::GuestAddress entry = 0U;
    memory::GuestAddress range_begin = 0U;
    memory::GuestAddress range_end = 0U;
    std::string module;
    GuestFunction target = nullptr;
};

class GuestFunctionRegistry
{
  public:
    explicit GuestFunctionRegistry(const memory::GuestMemory& memory) noexcept : memory_(&memory) {}

    GuestFunctionRegistry(const GuestFunctionRegistry&) = delete;
    GuestFunctionRegistry& operator=(const GuestFunctionRegistry&) = delete;

    [[nodiscard]] Result<void> add(FunctionRegistration registration);
    [[nodiscard]] Result<void> freeze();
    [[nodiscard]] Result<GuestFunction> lookup(memory::GuestAddress target) const;
    [[nodiscard]] Result<std::uint32_t> dispatch(memory::GuestAddress target,
                                                 CpuState& cpu,
                                                 RuntimeContext& runtime) const;

    [[nodiscard]] bool frozen() const noexcept { return frozen_; }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

  private:
    const memory::GuestMemory* memory_ = nullptr;
    std::vector<FunctionRegistration> entries_;
    bool frozen_ = false;
};

} // namespace switchrecomp::runtime
