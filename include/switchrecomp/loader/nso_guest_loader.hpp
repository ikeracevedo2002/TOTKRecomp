#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/format/nso.hpp"
#include "switchrecomp/memory/guest_memory.hpp"

namespace switchrecomp::loader
{

struct NsoGuestLoadOptions
{
    memory::GuestAddress module_base = 0U;
};

// Load the already materialized NSO image into a logical, host-backed guest
// address space. This does not parse or apply MOD0 metadata or relocations.
[[nodiscard]] Result<void> load_nso(const format::NsoImage& image,
                                    memory::GuestMemory& guest_memory,
                                    const NsoGuestLoadOptions& options = {});

} // namespace switchrecomp::loader
