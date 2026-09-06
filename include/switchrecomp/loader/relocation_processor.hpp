#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/format/elf_rela.hpp"
#include "switchrecomp/loader/symbol_resolver.hpp"

#include <span>

namespace switchrecomp::loader
{

struct RelocationProcessorOptions
{
    // Relocations are loader-time writes. Keeping this explicit makes it
    // impossible to accidentally turn normal guest writes into privileged ones.
    bool use_loader_write = true;
};

// Validate every relocation and stage its result before committing any bytes.
// Consequently a parse, resolution, arithmetic, or target-range failure leaves
// GuestMemory unchanged.
[[nodiscard]] Result<void> apply_relocations(
    memory::GuestMemory& guest_memory, std::span<const format::Relocation> relocations,
    const SymbolResolver& resolver, const RelocationProcessorOptions& options = {});

} // namespace switchrecomp::loader
