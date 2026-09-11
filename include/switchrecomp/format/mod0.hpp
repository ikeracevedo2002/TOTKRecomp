#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/memory/guest_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace switchrecomp::format
{

inline constexpr std::size_t module_start_size = 0x08U;
inline constexpr std::size_t mod0_header_size = 0x1cU;
inline constexpr std::size_t mod0_extended_header_size = 0x34U;
inline constexpr std::uint32_t mod0_magic = 0x30444f4dU; // "MOD0" in little-endian order.

// The first words of a loaded Switch module's text image. The second word is a
// module-relative offset to the MOD0 header; it is not a file offset or pointer.
struct ModuleStart
{
    std::uint32_t reserved;
    std::uint32_t mod0_offset;
};

struct Mod0Range
{
    std::int32_t start_offset;
    std::int32_t end_offset;
    memory::GuestAddress start_address;
    memory::GuestAddress end_address;
};

struct Mod0Info
{
    memory::GuestAddress address;

    std::int32_t dynamic_offset;
    std::int32_t bss_start_offset;
    std::int32_t bss_end_offset;
    std::int32_t exception_info_start_offset;
    std::int32_t exception_info_end_offset;
    std::int32_t module_object_offset;

    memory::GuestAddress dynamic_address;
    memory::GuestAddress bss_start_address;
    memory::GuestAddress bss_end_address;
    memory::GuestAddress exception_info_start_address;
    memory::GuestAddress exception_info_end_address;
    memory::GuestAddress module_object_address;

    // These fields were added by later Switch system versions. They are
    // optional because older MOD0 headers are exactly 0x1c bytes long.
    std::optional<Mod0Range> relro;
    std::optional<Mod0Range> nx_debug_link;
    std::optional<Mod0Range> gnu_build_id_note;
};

struct Mod0ParseOptions
{
    // Read the 19.0+ extension only when the complete extended header is
    // readable in one GuestMemory mapping. No scan is performed if it is not.
    bool parse_extended_fields = false;
};

// Resolve the ModuleStart slot at the beginning of the loaded text image. A
// valid NSO may have no MOD0 metadata, represented by an empty optional.
[[nodiscard]] Result<std::optional<memory::GuestAddress>> discover_mod0(
    const memory::GuestMemory& guest_memory, memory::GuestAddress module_base);

// Parse a MOD0 header at an already discovered guest address. All relative
// offsets are interpreted relative to the MOD0 header and retain signedness.
[[nodiscard]] Result<Mod0Info> parse_mod0(
    const memory::GuestMemory& guest_memory, memory::GuestAddress mod0_address,
    const Mod0ParseOptions& options = {});

} // namespace switchrecomp::format
