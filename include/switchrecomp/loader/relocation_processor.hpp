#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/format/elf_rela.hpp"
#include "switchrecomp/loader/symbol_resolver.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace switchrecomp::loader
{

struct RelocationProcessorOptions
{
    // Relocations are loader-time writes. Keeping this explicit makes it
    // impossible to accidentally turn normal guest writes into privileged ones.
    bool use_loader_write = true;
};

struct AppliedRelocation
{
    std::size_t relocation_index = 0U;
    format::Relocation relocation{};
    std::uint64_t value = 0U;
};

struct UnresolvedRelocation
{
    std::size_t relocation_index = 0U;
    format::Relocation relocation{};
    format::ImportSymbol symbol{};
};

struct RelocationPlan
{
    std::size_t relocation_count = 0U;
    std::vector<AppliedRelocation> applied;
    std::vector<UnresolvedRelocation> unresolved;
};

// Classify and validate every relocation without changing GuestMemory. A
// valid undefined global/weak symbol is retained as an unresolved import
// boundary; all other resolution and structural failures remain errors.
[[nodiscard]] Result<RelocationPlan> plan_relocations(
    const memory::GuestMemory& guest_memory, std::span<const format::Relocation> relocations,
    const SymbolResolver& resolver, const RelocationProcessorOptions& options = {});

// Apply only the resolved writes from a previously validated plan.
[[nodiscard]] Result<void> apply_relocation_plan(
    memory::GuestMemory& guest_memory, const RelocationPlan& plan,
    const RelocationProcessorOptions& options = {});

// Validate every relocation and stage its result before committing any bytes.
// Consequently a parse, resolution, arithmetic, or target-range failure leaves
// GuestMemory unchanged.
[[nodiscard]] Result<void> apply_relocations(
    memory::GuestMemory& guest_memory, std::span<const format::Relocation> relocations,
    const SymbolResolver& resolver, const RelocationProcessorOptions& options = {});

} // namespace switchrecomp::loader
