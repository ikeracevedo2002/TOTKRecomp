#pragma once

#include "switchrecomp/format/elf_dynamic.hpp"
#include "switchrecomp/format/mod0.hpp"

#include <optional>

namespace switchrecomp::format
{

struct ModuleMetadata
{
    // This is the address of the loaded text/module origin used by Switch NSO
    // relative metadata. It is a guest address, never a host pointer.
    memory::GuestAddress module_base;
    std::optional<Mod0Info> mod0;
    std::optional<DynamicInfo> dynamic;
};

struct ModuleMetadataParseOptions
{
    Mod0ParseOptions mod0;
    DynamicParseLimits dynamic;
};

// Discover MOD0 from the ModuleStart slot and parse its dynamic table. A valid
// guest image without MOD0 returns success with both optionals empty.
[[nodiscard]] Result<ModuleMetadata> parse_module_metadata(
    const memory::GuestMemory& guest_memory, memory::GuestAddress module_base,
    const ModuleMetadataParseOptions& options = {});

} // namespace switchrecomp::format
