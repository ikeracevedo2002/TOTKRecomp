#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/memory/guest_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace switchrecomp::format
{

inline constexpr std::size_t elf64_dyn_size = 16U;
inline constexpr std::size_t elf64_rela_size = 24U;
inline constexpr std::size_t elf64_rel_size = 16U;
inline constexpr std::size_t elf64_sym_size = 24U;

enum class DynamicTag : std::int64_t
{
    DT_NULL = 0,
    DT_NEEDED = 1,
    DT_PLTRELSZ = 2,
    DT_PLTGOT = 3,
    DT_HASH = 4,
    DT_STRTAB = 5,
    DT_SYMTAB = 6,
    DT_RELA = 7,
    DT_RELASZ = 8,
    DT_RELAENT = 9,
    DT_STRSZ = 10,
    DT_SYMENT = 11,
    DT_INIT = 12,
    DT_FINI = 13,
    DT_SONAME = 14,
    DT_RPATH = 15,
    DT_SYMBOLIC = 16,
    DT_REL = 17,
    DT_RELSZ = 18,
    DT_RELENT = 19,
    DT_PLTREL = 20,
    DT_DEBUG = 21,
    DT_TEXTREL = 22,
    DT_JMPREL = 23,
    DT_BIND_NOW = 24,
    DT_INIT_ARRAY = 25,
    DT_FINI_ARRAY = 26,
    DT_INIT_ARRAYSZ = 27,
    DT_FINI_ARRAYSZ = 28,
    DT_RUNPATH = 29,
    DT_FLAGS = 30,
    DT_ENCODING = 32,
    DT_PREINIT_ARRAY = 32,
    DT_PREINIT_ARRAYSZ = 33,
    DT_SYMTAB_SHNDX = 34,

    DT_GNU_HASH = 0x6ffffef5,
    DT_TLSDESC_PLT = 0x6ffffef6,
    DT_TLSDESC_GOT = 0x6ffffef7,
    DT_GNU_CONFLICT = 0x6ffffef8,
    DT_GNU_LIBLIST = 0x6ffffef9,
    DT_CONFIG = 0x6ffffefa,
    DT_DEPAUDIT = 0x6ffffefb,
    DT_AUDIT = 0x6ffffefc,
    DT_PLTPAD = 0x6ffffefd,
    DT_MOVETAB = 0x6ffffefe,
    DT_SYMINFO = 0x6ffffeff,
    DT_GNU_PRELINKED = 0x6ffffdf5,
    DT_GNU_CONFLICTSZ = 0x6ffffdf6,
    DT_GNU_LIBLISTSZ = 0x6ffffdf7,
    DT_CHECKSUM = 0x6ffffdf8,
    DT_PLTPADSZ = 0x6ffffdf9,
    DT_MOVEENT = 0x6ffffdfa,
    DT_MOVESZ = 0x6ffffdfb,
    DT_FEATURE_1 = 0x6ffffdfc,
    DT_POSFLAG_1 = 0x6ffffdfd,
    DT_SYMINSZ = 0x6ffffdfe,
    DT_SYMINENT = 0x6ffffdff,
    DT_VERSYM = 0x6ffffff0,
    DT_RELACOUNT = 0x6ffffff9,
    DT_RELCOUNT = 0x6ffffffa,
    DT_FLAGS_1 = 0x6ffffffb,
    DT_VERDEF = 0x6ffffffc,
    DT_VERDEFNUM = 0x6ffffffd,
    DT_VERNEED = 0x6ffffffe,
    DT_VERNEEDNUM = 0x6fffffff,
};

[[nodiscard]] std::string_view dynamic_tag_name(DynamicTag tag) noexcept;

struct DynamicEntry
{
    // d_tag is an ELF64 signed word. Keeping the raw signed value preserves
    // processor-specific and future tags without narrowing them.
    std::int64_t tag;
    std::uint64_t value;
};

struct DynamicPointer
{
    // NSO dynamic pointer values are module-relative offsets before relocation.
    // Both domains remain visible so callers cannot mistake one for the other.
    std::uint64_t module_offset;
    memory::GuestAddress address;
};

struct DynamicInfo
{
    memory::GuestAddress module_base;
    memory::GuestAddress address;
    std::size_t entry_count;
    std::vector<DynamicEntry> entries;

    std::vector<std::uint64_t> needed;
    std::optional<std::uint64_t> soname;
    std::optional<std::uint64_t> rpath;
    std::optional<std::uint64_t> runpath;
    std::optional<std::uint64_t> symbolic;
    std::optional<std::uint64_t> bind_now;
    std::optional<std::uint64_t> textrel;
    std::optional<std::uint64_t> flags;
    std::optional<std::uint64_t> flags_1;

    std::optional<DynamicPointer> pltgot;
    std::optional<DynamicPointer> hash;
    std::optional<DynamicPointer> gnu_hash;
    std::optional<DynamicPointer> strtab;
    std::optional<DynamicPointer> symtab;
    std::optional<DynamicPointer> rela;
    std::optional<DynamicPointer> init;
    std::optional<DynamicPointer> fini;
    std::optional<DynamicPointer> rel;
    std::optional<DynamicPointer> debug;
    std::optional<DynamicPointer> jmprel;
    std::optional<DynamicPointer> init_array;
    std::optional<DynamicPointer> fini_array;
    std::optional<DynamicPointer> preinit_array;

    std::optional<std::uint64_t> pltrelsz;
    std::optional<std::uint64_t> strsz;
    std::optional<std::uint64_t> syment;
    std::optional<std::uint64_t> relasz;
    std::optional<std::uint64_t> relaent;
    std::optional<std::uint64_t> relsz;
    std::optional<std::uint64_t> relent;
    std::optional<std::uint64_t> plt_rel_type;
    std::optional<std::uint64_t> init_array_size;
    std::optional<std::uint64_t> fini_array_size;
    std::optional<std::uint64_t> preinit_array_size;
    std::optional<std::uint64_t> relacount;
    std::optional<std::uint64_t> relcount;

    std::optional<std::size_t> rela_count;
    std::optional<std::size_t> jmprel_count;
    std::optional<std::size_t> rel_count;
};

inline constexpr std::size_t dynamic_default_max_entries = 65536U;
inline constexpr std::size_t dynamic_default_max_relocations = 1024U * 1024U;
inline constexpr std::uint64_t dynamic_default_max_string_table_size =
    std::uint64_t{256U} * std::uint64_t{1024U} * std::uint64_t{1024U};

struct DynamicParseLimits
{
    std::size_t max_dynamic_entries = dynamic_default_max_entries;
    std::size_t max_relocations = dynamic_default_max_relocations;
    std::uint64_t max_string_table_size = dynamic_default_max_string_table_size;
};

// Parse an ELF64-style dynamic table from guest memory. dynamic_address is the
// already resolved address from MOD0; pointer values inside the table are
// interpreted as offsets relative to module_base, as used by Switch NSOs.
[[nodiscard]] Result<DynamicInfo> parse_dynamic(
    const memory::GuestMemory& guest_memory, memory::GuestAddress module_base,
    memory::GuestAddress dynamic_address, const DynamicParseLimits& limits = {});

} // namespace switchrecomp::format
