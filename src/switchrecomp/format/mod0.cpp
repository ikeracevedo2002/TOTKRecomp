#include "switchrecomp/format/mod0.hpp"

#include "switchrecomp/common/binary_reader.hpp"
#include "switchrecomp/common/checked_arithmetic.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace switchrecomp::format
{

namespace
{

using memory::GuestAddress;
using memory::GuestMemory;

template <typename T>
[[nodiscard]] Result<T> read_failure(const Error& source, std::string_view field)
{
    return Result<T>::failure(make_error(
        source.code, "failed to read MOD0 " + std::string(field) + ": " + source.message));
}

[[nodiscard]] Result<std::uint32_t> read_u32(const GuestMemory& guest_memory,
                                             GuestAddress address, std::string_view field)
{
    std::array<std::byte, sizeof(std::uint32_t)> bytes{};
    const auto read = guest_memory.read(address, bytes);
    if (!read)
    {
        return read_failure<std::uint32_t>(read.error(), field);
    }
    const BinaryReader reader(bytes);
    const auto value = reader.read_u32_le(0U);
    if (!value)
    {
        return read_failure<std::uint32_t>(value.error(), field);
    }
    return value;
}

[[nodiscard]] Result<std::int32_t> read_i32(const BinaryReader& reader, std::size_t offset,
                                            std::string_view field)
{
    const auto value = reader.read_u32_le(offset);
    if (!value)
    {
        return read_failure<std::int32_t>(value.error(), field);
    }
    return Result<std::int32_t>::success(
        std::bit_cast<std::int32_t>(value.value()));
}

[[nodiscard]] Result<GuestAddress> resolve_offset(GuestAddress base, std::int32_t offset,
                                                   std::string_view field)
{
    const auto resolved = checked_add_signed_u64(base, static_cast<std::int64_t>(offset));
    if (!resolved)
    {
        return Result<GuestAddress>::failure(make_error(
            resolved.error().code,
            "MOD0 " + std::string(field) + " offset " + std::to_string(offset) +
                " cannot be resolved from " + std::to_string(base) + ": " +
                resolved.error().message));
    }
    return resolved;
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
            "MOD0 " + std::string(field) + " range is not mapped: " +
                permissions.error().message));
    }
    if (!memory::has_permission(permissions.value(), memory::GuestMemoryPermissions::Read))
    {
        return Result<void>::failure(make_error(
            ErrorCode::PermissionDenied,
            "MOD0 " + std::string(field) + " range is mapped without read permission"));
    }
    return Result<void>::success();
}

[[nodiscard]] bool is_region_boundary(const GuestMemory& guest_memory, GuestAddress address)
{
    if (guest_memory.region_at(address))
    {
        return true;
    }

    for (const auto& region : guest_memory.regions())
    {
        if (region.size != 0U && region.end() == address)
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] Result<void> validate_address(const GuestMemory& guest_memory, GuestAddress address,
                                            std::string_view field)
{
    if (!is_region_boundary(guest_memory, address))
    {
        return Result<void>::failure(make_error(
            ErrorCode::UnmappedMemory,
            "MOD0 " + std::string(field) + " address " + std::to_string(address) +
                " is outside guest memory"));
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> validate_range(const GuestMemory& guest_memory, GuestAddress start,
                                          GuestAddress end, std::string_view field)
{
    if (end < start)
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidFormat,
            "MOD0 " + std::string(field) + " end address precedes its start address"));
    }

    const auto start_valid = validate_address(guest_memory, start, field);
    if (!start_valid)
    {
        return start_valid;
    }
    const auto end_valid = validate_address(guest_memory, end, field);
    if (!end_valid)
    {
        return end_valid;
    }
    return require_readable(guest_memory, start, end - start, field);
}

} // namespace

Result<std::optional<GuestAddress>> discover_mod0(const GuestMemory& guest_memory,
                                                  GuestAddress module_base)
{
    const auto magic_offset_address = checked_add_u64(module_base, 4U);
    if (!magic_offset_address)
    {
        return Result<std::optional<GuestAddress>>::failure(make_error(
            magic_offset_address.error().code,
            "MOD0 discovery cannot read ModuleStart.magic_offset: module base overflows"));
    }

    const auto reserved = read_u32(guest_memory, module_base, "ModuleStart.reserved");
    const auto magic_offset =
        read_u32(guest_memory, magic_offset_address.value(), "ModuleStart.magic_offset");
    if (!reserved)
    {
        return Result<std::optional<GuestAddress>>::failure(reserved.error());
    }
    if (!magic_offset)
    {
        return Result<std::optional<GuestAddress>>::failure(magic_offset.error());
    }

    // A zero slot is the only unambiguous no-MOD0 representation. Also accept
    // a header at the slot itself for synthetic/custom modules, but do not scan.
    if (magic_offset.value() == 0U)
    {
        if (reserved.value() == mod0_magic)
        {
            return Result<std::optional<GuestAddress>>::success(module_base);
        }
        return Result<std::optional<GuestAddress>>::success(std::nullopt);
    }

    const auto mod0_address = checked_add_u64(module_base, magic_offset.value());
    if (!mod0_address)
    {
        return Result<std::optional<GuestAddress>>::failure(make_error(
            mod0_address.error().code,
            "MOD0 discovery address overflows: module base plus magic offset"));
    }

    const auto magic = read_u32(guest_memory, mod0_address.value(), "signature");
    if (!magic)
    {
        return Result<std::optional<GuestAddress>>::failure(magic.error());
    }
    if (magic.value() != mod0_magic)
    {
        return Result<std::optional<GuestAddress>>::failure(make_error(
            ErrorCode::InvalidFormat,
            "MOD0 magic mismatch at discovered address; expected MOD0"));
    }
    return Result<std::optional<GuestAddress>>::success(mod0_address.value());
}

Result<Mod0Info> parse_mod0(const GuestMemory& guest_memory, GuestAddress mod0_address,
                            const Mod0ParseOptions& options)
{
    std::array<std::byte, mod0_header_size> bytes{};
    const auto read = guest_memory.read(mod0_address, bytes);
    if (!read)
    {
        return Result<Mod0Info>::failure(make_error(
            read.error().code, "failed to read MOD0 header: " + read.error().message));
    }

    const BinaryReader reader(bytes);
    const auto signature = reader.read_u32_le(0U);
    if (!signature)
    {
        return Result<Mod0Info>::failure(signature.error());
    }
    if (signature.value() != mod0_magic)
    {
        return Result<Mod0Info>::failure(
            make_error(ErrorCode::InvalidFormat, "MOD0 magic mismatch; expected MOD0"));
    }

    const auto dynamic = read_i32(reader, 0x04U, "dynamic offset");
    const auto bss_start = read_i32(reader, 0x08U, "BSS start offset");
    const auto bss_end = read_i32(reader, 0x0cU, "BSS end offset");
    const auto exception_start = read_i32(reader, 0x10U, "exception info start offset");
    const auto exception_end = read_i32(reader, 0x14U, "exception info end offset");
    const auto module_object = read_i32(reader, 0x18U, "module object offset");
    if (!dynamic || !bss_start || !bss_end || !exception_start || !exception_end ||
        !module_object)
    {
        const Error* error = !dynamic       ? &dynamic.error()
                             : !bss_start   ? &bss_start.error()
                             : !bss_end     ? &bss_end.error()
                             : !exception_start ? &exception_start.error()
                             : !exception_end ? &exception_end.error()
                                               : &module_object.error();
        return Result<Mod0Info>::failure(error ? *error
                                               : make_error(ErrorCode::InvalidFormat,
                                                            "invalid MOD0 field"));
    }

    const auto dynamic_address = resolve_offset(mod0_address, dynamic.value(), "dynamic");
    const auto bss_start_address = resolve_offset(mod0_address, bss_start.value(), "BSS start");
    const auto bss_end_address = resolve_offset(mod0_address, bss_end.value(), "BSS end");
    const auto exception_start_address =
        resolve_offset(mod0_address, exception_start.value(), "exception info start");
    const auto exception_end_address =
        resolve_offset(mod0_address, exception_end.value(), "exception info end");
    const auto module_object_address =
        resolve_offset(mod0_address, module_object.value(), "module object");
    if (!dynamic_address || !bss_start_address || !bss_end_address ||
        !exception_start_address || !exception_end_address || !module_object_address)
    {
        const Error* error = !dynamic_address       ? &dynamic_address.error()
                             : !bss_start_address   ? &bss_start_address.error()
                             : !bss_end_address     ? &bss_end_address.error()
                             : !exception_start_address ? &exception_start_address.error()
                             : !exception_end_address ? &exception_end_address.error()
                                                        : &module_object_address.error();
        return Result<Mod0Info>::failure(error ? *error
                                               : make_error(ErrorCode::InvalidFormat,
                                                            "invalid MOD0 address"));
    }

    const auto dynamic_valid = require_readable(guest_memory, dynamic_address.value(), 1U,
                                                "dynamic");
    if (!dynamic_valid)
    {
        return Result<Mod0Info>::failure(dynamic_valid.error());
    }
    const auto bss_valid = validate_range(guest_memory, bss_start_address.value(),
                                          bss_end_address.value(), "BSS");
    if (!bss_valid)
    {
        return Result<Mod0Info>::failure(bss_valid.error());
    }
    const auto exception_valid = validate_range(guest_memory, exception_start_address.value(),
                                                exception_end_address.value(), "exception info");
    if (!exception_valid)
    {
        return Result<Mod0Info>::failure(exception_valid.error());
    }
    const auto module_valid =
        require_readable(guest_memory, module_object_address.value(), 1U, "module object");
    if (!module_valid)
    {
        return Result<Mod0Info>::failure(module_valid.error());
    }

    Mod0Info result{mod0_address,
                    dynamic.value(),
                    bss_start.value(),
                    bss_end.value(),
                    exception_start.value(),
                    exception_end.value(),
                    module_object.value(),
                    dynamic_address.value(),
                    bss_start_address.value(),
                    bss_end_address.value(),
                    exception_start_address.value(),
                    exception_end_address.value(),
                    module_object_address.value(),
                    std::nullopt,
                    std::nullopt,
                    std::nullopt};

    if (options.parse_extended_fields)
    {
        std::array<std::byte, mod0_extended_header_size> extended{};
        const auto extended_read = guest_memory.read(mod0_address, extended);
        if (extended_read)
        {
            const BinaryReader extended_reader(extended);
            const auto parse_extended_range = [&](std::size_t start_offset,
                                                   std::size_t end_offset,
                                                   std::string_view field)
                -> Result<std::optional<Mod0Range>>
            {
                const auto start = read_i32(extended_reader, start_offset,
                                            std::string(field) + " start offset");
                const auto end = read_i32(extended_reader, end_offset,
                                          std::string(field) + " end offset");
                if (!start)
                {
                    return Result<std::optional<Mod0Range>>::failure(start.error());
                }
                if (!end)
                {
                    return Result<std::optional<Mod0Range>>::failure(end.error());
                }
                const auto start_address = resolve_offset(mod0_address, start.value(), field);
                const auto end_address = resolve_offset(mod0_address, end.value(), field);
                if (!start_address || !end_address)
                {
                    return Result<std::optional<Mod0Range>>::failure(
                        !start_address ? start_address.error() : end_address.error());
                }
                const auto valid =
                    validate_range(guest_memory, start_address.value(), end_address.value(), field);
                if (!valid)
                {
                    return Result<std::optional<Mod0Range>>::failure(valid.error());
                }
                return Result<std::optional<Mod0Range>>::success(
                    Mod0Range{start.value(), end.value(), start_address.value(), end_address.value()});
            };

            const auto relro = parse_extended_range(0x1cU, 0x20U, "RELRO");
            const auto nx_debug = parse_extended_range(0x24U, 0x28U, "NX debug link");
            const auto build_id = parse_extended_range(0x2cU, 0x30U, "GNU build ID note");
            if (!relro || !nx_debug || !build_id)
            {
                const Error* error = !relro ? &relro.error() : !nx_debug ? &nx_debug.error()
                                                                         : &build_id.error();
                return Result<Mod0Info>::failure(*error);
            }
            result.relro = relro.value();
            result.nx_debug_link = nx_debug.value();
            result.gnu_build_id_note = build_id.value();
        }
    }

    return Result<Mod0Info>::success(std::move(result));
}

} // namespace switchrecomp::format
