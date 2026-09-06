#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/format/elf_dynamic.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
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

enum class AArch64RelocationType
{
    None,
    Abs64,
    Relative,
    GlobDat,
    JumpSlot,
    Unknown,
};

[[nodiscard]] AArch64RelocationType aarch64_relocation_type(std::uint32_t raw_type) noexcept;
[[nodiscard]] std::string_view aarch64_relocation_type_name(
    AArch64RelocationType type) noexcept;

struct Relocation
{
    std::uint64_t offset;
    memory::GuestAddress target_address;
    std::uint32_t raw_type;
    AArch64RelocationType type;
    std::uint32_t symbol_index;
    std::int64_t addend;
};

// Convert binary Elf64_Rela fields into the project-owned semantic form. Unknown
// numeric types remain representable and are rejected by the application layer.
[[nodiscard]] Result<std::vector<Relocation>> make_relocations(
    std::span<const RelaEntry> entries);

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
