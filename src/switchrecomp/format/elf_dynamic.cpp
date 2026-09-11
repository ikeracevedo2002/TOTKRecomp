#include "switchrecomp/format/elf_dynamic.hpp"

#include "switchrecomp/common/binary_reader.hpp"
#include "switchrecomp/common/checked_arithmetic.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>
#include <set>
#include <string>
#include <string_view>
#include <stdexcept>

namespace switchrecomp::format
{

namespace
{

using memory::GuestAddress;
using memory::GuestMemory;

[[nodiscard]] std::string tag_text(std::int64_t raw_tag)
{
    const auto tag = static_cast<DynamicTag>(raw_tag);
    const auto name = dynamic_tag_name(tag);
    if (name != "unknown")
    {
        return std::string(name);
    }
    return "tag " + std::to_string(raw_tag);
}

template <typename T>
[[nodiscard]] Result<T> read_failure(const Error& source, std::string_view field)
{
    return Result<T>::failure(make_error(
        source.code, "failed to read dynamic " + std::string(field) + ": " + source.message));
}

[[nodiscard]] Result<std::uint64_t> read_u64(const GuestMemory& guest_memory,
                                             GuestAddress address, std::string_view field)
{
    std::array<std::byte, sizeof(std::uint64_t)> bytes{};
    const auto read = guest_memory.read(address, bytes);
    if (!read)
    {
        return read_failure<std::uint64_t>(read.error(), field);
    }
    const BinaryReader reader(bytes);
    const auto value = reader.read_u64_le(0U);
    if (!value)
    {
        return read_failure<std::uint64_t>(value.error(), field);
    }
    return value;
}

[[nodiscard]] Result<void> require_readable(const GuestMemory& guest_memory, GuestAddress address,
                                            std::uint64_t size, std::string_view field)
{
    if (size == 0U)
    {
        return Result<void>::success();
    }
    const auto permissions = guest_memory.permissions_at(address, size);
    if (!permissions)
    {
        return Result<void>::failure(make_error(
            permissions.error().code,
            "dynamic " + std::string(field) + " range is not mapped: " +
                permissions.error().message));
    }
    if (!memory::has_permission(permissions.value(), memory::GuestMemoryPermissions::Read))
    {
        return Result<void>::failure(make_error(
            ErrorCode::PermissionDenied,
            "dynamic " + std::string(field) + " range is mapped without read permission"));
    }
    return Result<void>::success();
}

[[nodiscard]] bool is_singleton_tag(DynamicTag tag) noexcept
{
    switch (tag)
    {
    case DynamicTag::DT_NULL:
    case DynamicTag::DT_NEEDED:
        return false;
    default:
        return dynamic_tag_name(tag) != "unknown";
    }
}

[[nodiscard]] Result<void> incomplete_pair(std::string_view name)
{
    return Result<void>::failure(make_error(
        ErrorCode::InvalidFormat,
        "dynamic metadata has an incomplete " + std::string(name) + " pointer/size pair"));
}

[[nodiscard]] Result<void> validate_table(const GuestMemory& guest_memory,
                                           const std::optional<DynamicPointer>& address,
                                           const std::optional<std::uint64_t>& size,
                                           const std::optional<std::uint64_t>& entry_size,
                                           std::uint64_t expected_entry_size,
                                           std::size_t max_entries, std::string_view name,
                                           std::optional<std::size_t>& count)
{
    if (!address && !size && !entry_size)
    {
        return Result<void>::success();
    }
    if (!address || !size || !entry_size)
    {
        return incomplete_pair(name);
    }
    if (entry_size.value() != expected_entry_size)
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidFormat,
            "dynamic " + std::string(name) + " entry size is " +
                std::to_string(entry_size.value()) + ", expected " +
                std::to_string(expected_entry_size)));
    }
    if (size.value() % entry_size.value() != 0U)
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidFormat,
            "dynamic " + std::string(name) + " size is not divisible by its entry size"));
    }

    const auto raw_count = size.value() / entry_size.value();
    if (raw_count > static_cast<std::uint64_t>(max_entries))
    {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceLimit,
            "dynamic " + std::string(name) + " entry count exceeds the configured limit"));
    }
    count = static_cast<std::size_t>(raw_count);
    if (size.value() != 0U)
    {
        const auto readable =
            require_readable(guest_memory, address->address, size.value(), name);
        if (!readable)
        {
            return readable;
        }
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> validate_sized_pointer(
    const GuestMemory& guest_memory, const std::optional<DynamicPointer>& address,
    const std::optional<std::uint64_t>& size, std::string_view name)
{
    if (!address && !size)
    {
        return Result<void>::success();
    }
    if (!address || !size)
    {
        return incomplete_pair(name);
    }
    if (size.value() != 0U)
    {
        return require_readable(guest_memory, address->address, size.value(), name);
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> validate_string_offsets(const DynamicInfo& dynamic)
{
    if (dynamic.needed.empty() && !dynamic.soname && !dynamic.rpath && !dynamic.runpath)
    {
        return Result<void>::success();
    }
    if (!dynamic.strtab || !dynamic.strsz)
    {
        return incomplete_pair("string table");
    }

    const auto check = [&](std::uint64_t offset, std::string_view field) -> Result<void>
    {
        if (offset >= dynamic.strsz.value())
        {
            return Result<void>::failure(make_error(
                ErrorCode::OutOfBounds,
                "dynamic " + std::string(field) + " string offset " +
                    std::to_string(offset) + " is outside DT_STRSZ"));
        }
        return Result<void>::success();
    };

    for (const auto offset : dynamic.needed)
    {
        const auto valid = check(offset, "DT_NEEDED");
        if (!valid)
        {
            return valid;
        }
    }
    if (dynamic.soname)
    {
        const auto valid = check(dynamic.soname.value(), "DT_SONAME");
        if (!valid)
        {
            return valid;
        }
    }
    if (dynamic.rpath)
    {
        const auto valid = check(dynamic.rpath.value(), "DT_RPATH");
        if (!valid)
        {
            return valid;
        }
    }
    if (dynamic.runpath)
    {
        const auto valid = check(dynamic.runpath.value(), "DT_RUNPATH");
        if (!valid)
        {
            return valid;
        }
    }
    return Result<void>::success();
}

} // namespace

std::string_view dynamic_tag_name(DynamicTag tag) noexcept
{
    switch (tag)
    {
    case DynamicTag::DT_NULL:
        return "DT_NULL";
    case DynamicTag::DT_NEEDED:
        return "DT_NEEDED";
    case DynamicTag::DT_PLTRELSZ:
        return "DT_PLTRELSZ";
    case DynamicTag::DT_PLTGOT:
        return "DT_PLTGOT";
    case DynamicTag::DT_HASH:
        return "DT_HASH";
    case DynamicTag::DT_STRTAB:
        return "DT_STRTAB";
    case DynamicTag::DT_SYMTAB:
        return "DT_SYMTAB";
    case DynamicTag::DT_RELA:
        return "DT_RELA";
    case DynamicTag::DT_RELASZ:
        return "DT_RELASZ";
    case DynamicTag::DT_RELAENT:
        return "DT_RELAENT";
    case DynamicTag::DT_STRSZ:
        return "DT_STRSZ";
    case DynamicTag::DT_SYMENT:
        return "DT_SYMENT";
    case DynamicTag::DT_INIT:
        return "DT_INIT";
    case DynamicTag::DT_FINI:
        return "DT_FINI";
    case DynamicTag::DT_SONAME:
        return "DT_SONAME";
    case DynamicTag::DT_RPATH:
        return "DT_RPATH";
    case DynamicTag::DT_SYMBOLIC:
        return "DT_SYMBOLIC";
    case DynamicTag::DT_REL:
        return "DT_REL";
    case DynamicTag::DT_RELSZ:
        return "DT_RELSZ";
    case DynamicTag::DT_RELENT:
        return "DT_RELENT";
    case DynamicTag::DT_PLTREL:
        return "DT_PLTREL";
    case DynamicTag::DT_DEBUG:
        return "DT_DEBUG";
    case DynamicTag::DT_TEXTREL:
        return "DT_TEXTREL";
    case DynamicTag::DT_JMPREL:
        return "DT_JMPREL";
    case DynamicTag::DT_BIND_NOW:
        return "DT_BIND_NOW";
    case DynamicTag::DT_INIT_ARRAY:
        return "DT_INIT_ARRAY";
    case DynamicTag::DT_FINI_ARRAY:
        return "DT_FINI_ARRAY";
    case DynamicTag::DT_INIT_ARRAYSZ:
        return "DT_INIT_ARRAYSZ";
    case DynamicTag::DT_FINI_ARRAYSZ:
        return "DT_FINI_ARRAYSZ";
    case DynamicTag::DT_RUNPATH:
        return "DT_RUNPATH";
    case DynamicTag::DT_FLAGS:
        return "DT_FLAGS";
    case DynamicTag::DT_PREINIT_ARRAY:
        return "DT_PREINIT_ARRAY";
    case DynamicTag::DT_PREINIT_ARRAYSZ:
        return "DT_PREINIT_ARRAYSZ";
    case DynamicTag::DT_SYMTAB_SHNDX:
        return "DT_SYMTAB_SHNDX";
    case DynamicTag::DT_GNU_HASH:
        return "DT_GNU_HASH";
    case DynamicTag::DT_TLSDESC_PLT:
        return "DT_TLSDESC_PLT";
    case DynamicTag::DT_TLSDESC_GOT:
        return "DT_TLSDESC_GOT";
    case DynamicTag::DT_GNU_CONFLICT:
        return "DT_GNU_CONFLICT";
    case DynamicTag::DT_GNU_LIBLIST:
        return "DT_GNU_LIBLIST";
    case DynamicTag::DT_GNU_PRELINKED:
        return "DT_GNU_PRELINKED";
    case DynamicTag::DT_VERSYM:
        return "DT_VERSYM";
    case DynamicTag::DT_RELACOUNT:
        return "DT_RELACOUNT";
    case DynamicTag::DT_RELCOUNT:
        return "DT_RELCOUNT";
    case DynamicTag::DT_FLAGS_1:
        return "DT_FLAGS_1";
    case DynamicTag::DT_VERDEF:
        return "DT_VERDEF";
    case DynamicTag::DT_VERDEFNUM:
        return "DT_VERDEFNUM";
    case DynamicTag::DT_VERNEED:
        return "DT_VERNEED";
    case DynamicTag::DT_VERNEEDNUM:
        return "DT_VERNEEDNUM";
    default:
        return "unknown";
    }
}

Result<DynamicInfo> parse_dynamic(const GuestMemory& guest_memory, GuestAddress module_base,
                                  GuestAddress dynamic_address,
                                  const DynamicParseLimits& limits)
{
    try
    {
        DynamicInfo result{};
        result.module_base = module_base;
        result.address = dynamic_address;
        result.entry_count = 0U;
        result.entries.reserve(std::min(limits.max_dynamic_entries, std::size_t{256U}));
        std::set<std::int64_t> seen_singletons;

        const auto assign_pointer = [&](std::optional<DynamicPointer>& destination,
                                        std::uint64_t module_offset,
                                        std::string_view field) -> Result<void>
        {
            const auto address = checked_add_u64(module_base, module_offset);
            if (!address)
            {
                return Result<void>::failure(make_error(
                    address.error().code,
                    "dynamic " + std::string(field) + " address overflows module base"));
            }
            destination = DynamicPointer{module_offset, address.value()};
            return Result<void>::success();
        };

        while (true)
        {
            if (result.entry_count >= limits.max_dynamic_entries)
            {
                return Result<DynamicInfo>::failure(make_error(
                    ErrorCode::ResourceLimit,
                    "dynamic table is missing DT_NULL before the configured entry limit"));
            }

            const auto offset = checked_mul_u64(static_cast<std::uint64_t>(result.entry_count),
                                                static_cast<std::uint64_t>(elf64_dyn_size));
            if (!offset)
            {
                return Result<DynamicInfo>::failure(make_error(
                    offset.error().code, "dynamic table entry address overflows"));
            }
            const auto entry_address = checked_add_u64(dynamic_address, offset.value());
            if (!entry_address)
            {
                return Result<DynamicInfo>::failure(make_error(
                    entry_address.error().code,
                    "dynamic table entry address overflows dynamic base"));
            }

            const auto raw_tag = read_u64(guest_memory, entry_address.value(), "tag");
            const auto value = checked_add_u64(entry_address.value(), 8U);
            if (!raw_tag)
            {
                return Result<DynamicInfo>::failure(raw_tag.error());
            }
            if (!value)
            {
                return Result<DynamicInfo>::failure(make_error(
                    value.error().code, "dynamic table value address overflows"));
            }
            const auto raw_value = read_u64(guest_memory, value.value(), "value");
            if (!raw_value)
            {
                return Result<DynamicInfo>::failure(raw_value.error());
            }

            const auto tag = std::bit_cast<std::int64_t>(raw_tag.value());
            result.entries.push_back(DynamicEntry{tag, raw_value.value()});
            ++result.entry_count;

            if (static_cast<DynamicTag>(tag) == DynamicTag::DT_NULL)
            {
                break;
            }

            const auto tag_kind = static_cast<DynamicTag>(tag);
            if (is_singleton_tag(tag_kind) &&
                !seen_singletons.insert(tag).second)
            {
                return Result<DynamicInfo>::failure(make_error(
                    ErrorCode::InvalidFormat,
                    "duplicate singleton dynamic tag " + tag_text(tag)));
            }

            Result<void> assigned = Result<void>::success();
            switch (tag_kind)
            {
            case DynamicTag::DT_NEEDED:
                result.needed.push_back(raw_value.value());
                break;
            case DynamicTag::DT_PLTRELSZ:
                result.pltrelsz = raw_value.value();
                break;
            case DynamicTag::DT_PLTGOT:
                assigned = assign_pointer(result.pltgot, raw_value.value(), "DT_PLTGOT");
                break;
            case DynamicTag::DT_HASH:
                assigned = assign_pointer(result.hash, raw_value.value(), "DT_HASH");
                break;
            case DynamicTag::DT_GNU_HASH:
                assigned = assign_pointer(result.gnu_hash, raw_value.value(), "DT_GNU_HASH");
                break;
            case DynamicTag::DT_STRTAB:
                assigned = assign_pointer(result.strtab, raw_value.value(), "DT_STRTAB");
                break;
            case DynamicTag::DT_SYMTAB:
                assigned = assign_pointer(result.symtab, raw_value.value(), "DT_SYMTAB");
                break;
            case DynamicTag::DT_RELA:
                assigned = assign_pointer(result.rela, raw_value.value(), "DT_RELA");
                break;
            case DynamicTag::DT_RELASZ:
                result.relasz = raw_value.value();
                break;
            case DynamicTag::DT_RELAENT:
                result.relaent = raw_value.value();
                break;
            case DynamicTag::DT_STRSZ:
                result.strsz = raw_value.value();
                break;
            case DynamicTag::DT_SYMENT:
                result.syment = raw_value.value();
                break;
            case DynamicTag::DT_INIT:
                assigned = assign_pointer(result.init, raw_value.value(), "DT_INIT");
                break;
            case DynamicTag::DT_FINI:
                assigned = assign_pointer(result.fini, raw_value.value(), "DT_FINI");
                break;
            case DynamicTag::DT_SONAME:
                result.soname = raw_value.value();
                break;
            case DynamicTag::DT_RPATH:
                result.rpath = raw_value.value();
                break;
            case DynamicTag::DT_SYMBOLIC:
                result.symbolic = raw_value.value();
                break;
            case DynamicTag::DT_REL:
                assigned = assign_pointer(result.rel, raw_value.value(), "DT_REL");
                break;
            case DynamicTag::DT_RELSZ:
                result.relsz = raw_value.value();
                break;
            case DynamicTag::DT_RELENT:
                result.relent = raw_value.value();
                break;
            case DynamicTag::DT_PLTREL:
                result.plt_rel_type = raw_value.value();
                break;
            case DynamicTag::DT_DEBUG:
                assigned = assign_pointer(result.debug, raw_value.value(), "DT_DEBUG");
                break;
            case DynamicTag::DT_TEXTREL:
                result.textrel = raw_value.value();
                break;
            case DynamicTag::DT_JMPREL:
                assigned = assign_pointer(result.jmprel, raw_value.value(), "DT_JMPREL");
                break;
            case DynamicTag::DT_BIND_NOW:
                result.bind_now = raw_value.value();
                break;
            case DynamicTag::DT_INIT_ARRAY:
                assigned = assign_pointer(result.init_array, raw_value.value(), "DT_INIT_ARRAY");
                break;
            case DynamicTag::DT_FINI_ARRAY:
                assigned = assign_pointer(result.fini_array, raw_value.value(), "DT_FINI_ARRAY");
                break;
            case DynamicTag::DT_INIT_ARRAYSZ:
                result.init_array_size = raw_value.value();
                break;
            case DynamicTag::DT_FINI_ARRAYSZ:
                result.fini_array_size = raw_value.value();
                break;
            case DynamicTag::DT_RUNPATH:
                result.runpath = raw_value.value();
                break;
            case DynamicTag::DT_FLAGS:
                result.flags = raw_value.value();
                break;
            case DynamicTag::DT_PREINIT_ARRAY:
                assigned = assign_pointer(result.preinit_array, raw_value.value(),
                                          "DT_PREINIT_ARRAY");
                break;
            case DynamicTag::DT_PREINIT_ARRAYSZ:
                result.preinit_array_size = raw_value.value();
                break;
            case DynamicTag::DT_RELACOUNT:
                result.relacount = raw_value.value();
                break;
            case DynamicTag::DT_RELCOUNT:
                result.relcount = raw_value.value();
                break;
            case DynamicTag::DT_FLAGS_1:
                result.flags_1 = raw_value.value();
                break;
            default:
                // Unknown and currently unmodeled tags stay in entries for
                // forward-compatible inspection.
                break;
            }
            if (!assigned)
            {
                return Result<DynamicInfo>::failure(assigned.error());
            }
        }

        if (result.strsz && result.strsz.value() > limits.max_string_table_size)
        {
            return Result<DynamicInfo>::failure(make_error(
                ErrorCode::ResourceLimit,
                "dynamic string table exceeds the configured size limit"));
        }
        const auto strtab_valid = validate_sized_pointer(
            guest_memory, result.strtab, result.strsz, "DT_STRTAB");
        if (!strtab_valid)
        {
            return Result<DynamicInfo>::failure(strtab_valid.error());
        }
        const auto strings_valid = validate_string_offsets(result);
        if (!strings_valid)
        {
            return Result<DynamicInfo>::failure(strings_valid.error());
        }

        if (result.symtab.has_value() != result.syment.has_value())
        {
            return Result<DynamicInfo>::failure(incomplete_pair("symbol table").error());
        }
        if (result.syment && result.syment.value() != elf64_sym_size)
        {
            return Result<DynamicInfo>::failure(make_error(
                ErrorCode::InvalidFormat,
                "dynamic DT_SYMENT is " + std::to_string(result.syment.value()) +
                    ", expected " + std::to_string(elf64_sym_size)));
        }
        if (result.symtab)
        {
            const auto readable = require_readable(guest_memory, result.symtab->address,
                                                   elf64_sym_size, "DT_SYMTAB");
            if (!readable)
            {
                return Result<DynamicInfo>::failure(readable.error());
            }
        }

        const auto rela_valid = validate_table(guest_memory, result.rela, result.relasz,
                                               result.relaent, elf64_rela_size,
                                               limits.max_relocations, "RELA", result.rela_count);
        if (!rela_valid)
        {
            return Result<DynamicInfo>::failure(rela_valid.error());
        }
        const auto rel_valid = validate_table(guest_memory, result.rel, result.relsz,
                                              result.relent, elf64_rel_size,
                                              limits.max_relocations, "REL", result.rel_count);
        if (!rel_valid)
        {
            return Result<DynamicInfo>::failure(rel_valid.error());
        }

        const bool has_jmprel = result.jmprel || result.pltrelsz || result.plt_rel_type;
        if (has_jmprel)
        {
            if (!result.jmprel || !result.pltrelsz || !result.plt_rel_type)
            {
                return Result<DynamicInfo>::failure(incomplete_pair("JMPREL").error());
            }
            if (result.plt_rel_type.value() != static_cast<std::uint64_t>(DynamicTag::DT_RELA))
            {
                return Result<DynamicInfo>::failure(make_error(
                    ErrorCode::Unsupported,
                    "unsupported DT_PLTREL encoding " +
                        std::to_string(result.plt_rel_type.value()) + "; only DT_RELA is supported"));
            }
            if (result.pltrelsz.value() % elf64_rela_size != 0U)
            {
                return Result<DynamicInfo>::failure(make_error(
                    ErrorCode::InvalidFormat, "JMPREL size is not divisible by Elf64_Rela size"));
            }
            const auto count = result.pltrelsz.value() / elf64_rela_size;
            if (count > static_cast<std::uint64_t>(limits.max_relocations))
            {
                return Result<DynamicInfo>::failure(make_error(
                    ErrorCode::ResourceLimit,
                    "JMPREL entry count exceeds the configured relocation limit"));
            }
            result.jmprel_count = static_cast<std::size_t>(count);
            if (result.pltrelsz.value() != 0U)
            {
                const auto readable = require_readable(guest_memory, result.jmprel->address,
                                                       result.pltrelsz.value(), "JMPREL");
                if (!readable)
                {
                    return Result<DynamicInfo>::failure(readable.error());
                }
            }
        }

        return Result<DynamicInfo>::success(std::move(result));
    }
    catch (const std::bad_alloc&)
    {
        return Result<DynamicInfo>::failure(
            make_error(ErrorCode::ResourceLimit, "dynamic metadata allocation failed"));
    }
    catch (const std::length_error&)
    {
        return Result<DynamicInfo>::failure(
            make_error(ErrorCode::ResourceLimit, "dynamic metadata exceeds host container limits"));
    }
}

} // namespace switchrecomp::format
