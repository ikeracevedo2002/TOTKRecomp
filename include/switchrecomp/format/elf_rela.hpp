#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/format/elf_dynamic.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace switchrecomp::format
{

struct RelaEntry
{
    // r_offset is retained in the module-relative address domain used by NSO.
    std::uint64_t offset;
    memory::GuestAddress target_address;
    std::uint64_t info;
    std::int64_t addend;

    [[nodiscard]] std::uint32_t symbol_index() const noexcept
    {
        return static_cast<std::uint32_t>(info >> 32U);
    }

    [[nodiscard]] std::uint32_t relocation_type() const noexcept
    {
        return static_cast<std::uint32_t>(info & 0xffffffffU);
    }
};

// Parse the RELA table described by DT_RELA/DT_RELASZ/DT_RELAENT. This only
// reads and validates metadata; it never writes the relocation result back to
// GuestMemory.
[[nodiscard]] Result<std::vector<RelaEntry>> parse_rela_table(
    const memory::GuestMemory& guest_memory, const DynamicInfo& dynamic,
    const DynamicParseLimits& limits = {});

// Parse the PLT RELA table described by DT_JMPREL/DT_PLTRELSZ/DT_PLTREL.
// REL encodings are deliberately rejected until a future milestone supports
// them explicitly.
[[nodiscard]] Result<std::vector<RelaEntry>> parse_jmprel_table(
    const memory::GuestMemory& guest_memory, const DynamicInfo& dynamic,
    const DynamicParseLimits& limits = {});

} // namespace switchrecomp::format
