#include "switchrecomp/format/dynamic_symbols.hpp"

#include "switchrecomp/common/binary_reader.hpp"
#include "switchrecomp/common/checked_arithmetic.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

namespace switchrecomp::format
{

namespace
{

using memory::GuestAddress;
using memory::GuestMemory;

template <typename T>
[[nodiscard]] Result<T> contextual_failure(const Error& error, std::string_view context)
{
    return Result<T>::failure(make_error(error.code,
                                         std::string(context) + ": " + error.message));
}

[[nodiscard]] Result<std::uint32_t> read_u32(const GuestMemory& memory, GuestAddress address,
                                             std::string_view context)
{
    std::array<std::byte, sizeof(std::uint32_t)> bytes{};
    const auto read = memory.read(address, bytes);
    if (!read)
    {
        return contextual_failure<std::uint32_t>(read.error(), context);
    }
    const BinaryReader reader(bytes);
    const auto value = reader.read_u32_le(0U);
    if (!value)
    {
        return contextual_failure<std::uint32_t>(value.error(), context);
    }
    return value;
}

[[nodiscard]] Result<void> require_readable(const GuestMemory& memory, GuestAddress address,
                                            std::uint64_t size, std::string_view context)
{
    if (size == 0U)
    {
        return Result<void>::success();
    }
    const auto permissions = memory.permissions_at(address, size);
    if (!permissions)
    {
        return Result<void>::failure(make_error(
            permissions.error().code, std::string(context) + " is not mapped: " +
                                           permissions.error().message));
    }
    if (!memory::has_permission(permissions.value(), memory::GuestMemoryPermissions::Read))
    {
        return Result<void>::failure(
            make_error(ErrorCode::PermissionDenied,
                       std::string(context) + " is mapped without read permission"));
    }
    return Result<void>::success();
}

[[nodiscard]] Result<std::uint64_t> table_bytes(std::uint64_t count, std::uint64_t entry_size,
                                                std::string_view context)
{
    const auto payload = checked_mul_u64(count, entry_size);
    if (!payload)
    {
        return Result<std::uint64_t>::failure(
            make_error(payload.error().code, std::string(context) + " size overflows"));
    }
    const auto total = checked_add_u64(8U, payload.value());
    if (!total)
    {
        return Result<std::uint64_t>::failure(
            make_error(total.error().code, std::string(context) + " size overflows"));
    }
    return total;
}

[[nodiscard]] Result<std::size_t> derive_sysv_count(const GuestMemory& memory,
                                                    const DynamicInfo& dynamic,
                                                    std::size_t max_symbols)
{
    if (!dynamic.hash)
    {
        return Result<std::size_t>::failure(
            make_error(ErrorCode::Unsupported,
                       "dynsym bounds require DT_HASH or DT_GNU_HASH metadata"));
    }
    const auto buckets = read_u32(memory, dynamic.hash->address, "DT_HASH nbucket");
    const auto chains_address = checked_add_u64(dynamic.hash->address, 4U);
    if (!buckets || !chains_address)
    {
        const Error* error = !buckets ? &buckets.error() : &chains_address.error();
        return Result<std::size_t>::failure(error ? *error
                                                 : make_error(ErrorCode::InvalidFormat,
                                                              "invalid DT_HASH header"));
    }
    const auto chains = read_u32(memory, chains_address.value(), "DT_HASH nchain");
    if (!chains)
    {
        return Result<std::size_t>::failure(chains.error());
    }
    if (chains.value() > max_symbols)
    {
        return Result<std::size_t>::failure(make_error(
            ErrorCode::ResourceLimit, "DT_HASH symbol count exceeds the configured limit"));
    }
    const auto bytes = table_bytes(static_cast<std::uint64_t>(buckets.value()) + 1U +
                                       static_cast<std::uint64_t>(chains.value()),
                                   sizeof(std::uint32_t), "DT_HASH");
    if (!bytes)
    {
        return Result<std::size_t>::failure(bytes.error());
    }
    const auto readable = require_readable(memory, dynamic.hash->address, bytes.value(), "DT_HASH");
    if (!readable)
    {
        return Result<std::size_t>::failure(readable.error());
    }
    return Result<std::size_t>::success(static_cast<std::size_t>(chains.value()));
}

[[nodiscard]] Result<std::size_t> derive_gnu_count(const GuestMemory& memory,
                                                   const DynamicInfo& dynamic,
                                                   std::size_t max_symbols)
{
    if (!dynamic.gnu_hash)
    {
        return Result<std::size_t>::failure(
            make_error(ErrorCode::Unsupported, "DT_GNU_HASH is not present"));
    }

    const auto nbuckets = read_u32(memory, dynamic.gnu_hash->address, "DT_GNU_HASH nbuckets");
    const auto symoffset_address = checked_add_u64(dynamic.gnu_hash->address, 4U);
    const auto bloom_size_address = checked_add_u64(dynamic.gnu_hash->address, 8U);
    const auto bloom_shift_address = checked_add_u64(dynamic.gnu_hash->address, 12U);
    if (!nbuckets || !symoffset_address || !bloom_size_address || !bloom_shift_address)
    {
        const Error* error = !nbuckets          ? &nbuckets.error()
                             : !symoffset_address ? &symoffset_address.error()
                             : !bloom_size_address ? &bloom_size_address.error()
                                                    : &bloom_shift_address.error();
        return Result<std::size_t>::failure(*error);
    }
    const auto symoffset = read_u32(memory, symoffset_address.value(), "DT_GNU_HASH symoffset");
    const auto bloom_size = read_u32(memory, bloom_size_address.value(), "DT_GNU_HASH bloom_size");
    const auto bloom_shift = read_u32(memory, bloom_shift_address.value(), "DT_GNU_HASH bloom_shift");
    (void)bloom_shift;
    if (!symoffset || !bloom_size || !bloom_shift)
    {
        const Error* error = !symoffset ? &symoffset.error()
                             : !bloom_size ? &bloom_size.error()
                                           : &bloom_shift.error();
        return Result<std::size_t>::failure(*error);
    }
    if (bloom_size.value() == 0U)
    {
        return Result<std::size_t>::failure(
            make_error(ErrorCode::InvalidFormat, "DT_GNU_HASH bloom_size cannot be zero"));
    }
    if (nbuckets.value() == 0U)
    {
        return Result<std::size_t>::failure(
            make_error(ErrorCode::InvalidFormat, "DT_GNU_HASH nbuckets cannot be zero"));
    }
    if (symoffset.value() > max_symbols)
    {
        return Result<std::size_t>::failure(make_error(
            ErrorCode::ResourceLimit, "DT_GNU_HASH symbol offset exceeds the configured limit"));
    }

    const auto bloom_bytes = checked_mul_u64(bloom_size.value(), sizeof(std::uint64_t));
    const auto bucket_bytes = checked_mul_u64(nbuckets.value(), sizeof(std::uint32_t));
    if (!bloom_bytes || !bucket_bytes)
    {
        const Error* error = !bloom_bytes ? &bloom_bytes.error() : &bucket_bytes.error();
        return Result<std::size_t>::failure(*error);
    }
    const auto buckets_offset = checked_add_u64(16U, bloom_bytes.value());
    const auto chains_offset = buckets_offset ? checked_add_u64(buckets_offset.value(), bucket_bytes.value())
                                              : Result<std::uint64_t>::failure(buckets_offset.error());
    if (!buckets_offset || !chains_offset)
    {
        const Error* error = !buckets_offset ? &buckets_offset.error() : &chains_offset.error();
        return Result<std::size_t>::failure(*error);
    }
    const auto header_and_buckets = require_readable(memory, dynamic.gnu_hash->address,
                                                      chains_offset.value(), "DT_GNU_HASH");
    if (!header_and_buckets)
    {
        return Result<std::size_t>::failure(header_and_buckets.error());
    }

    std::uint32_t maximum_bucket = 0U;
    for (std::uint32_t index = 0U; index < nbuckets.value(); ++index)
    {
        const auto index_offset = checked_mul_u64(index, sizeof(std::uint32_t));
        const auto bucket_offset = index_offset
                                       ? checked_add_u64(buckets_offset.value(), index_offset.value())
                                       : Result<std::uint64_t>::failure(index_offset.error());
        const auto bucket_address = bucket_offset
                                        ? checked_add_u64(dynamic.gnu_hash->address,
                                                          bucket_offset.value())
                                        : Result<std::uint64_t>::failure(bucket_offset.error());
        if (!index_offset || !bucket_offset || !bucket_address)
        {
            const Error* error = !index_offset   ? &index_offset.error()
                                 : !bucket_offset ? &bucket_offset.error()
                                                   : &bucket_address.error();
            return Result<std::size_t>::failure(*error);
        }
        const auto bucket = read_u32(memory, bucket_address.value(), "DT_GNU_HASH bucket");
        if (!bucket)
        {
            return Result<std::size_t>::failure(bucket.error());
        }
        maximum_bucket = std::max(maximum_bucket, bucket.value());
    }

    if (maximum_bucket < symoffset.value())
    {
        return Result<std::size_t>::success(static_cast<std::size_t>(symoffset.value()));
    }

    std::uint64_t chain_index = static_cast<std::uint64_t>(maximum_bucket) - symoffset.value();
    for (;; ++chain_index)
    {
        const auto symbol_count = checked_add_u64(symoffset.value(), chain_index + 1U);
        if (!symbol_count)
        {
            return Result<std::size_t>::failure(symbol_count.error());
        }
        if (symbol_count.value() > max_symbols)
        {
            return Result<std::size_t>::failure(make_error(
                ErrorCode::ResourceLimit,
                "DT_GNU_HASH chain exceeds the configured symbol limit before termination"));
        }
        const auto chain_offset = checked_mul_u64(chain_index, sizeof(std::uint32_t));
        const auto chain_absolute_offset = chain_offset
                                               ? checked_add_u64(chains_offset.value(),
                                                                 chain_offset.value())
                                               : Result<std::uint64_t>::failure(chain_offset.error());
        const auto chain_address = chain_absolute_offset
                                       ? checked_add_u64(dynamic.gnu_hash->address,
                                                         chain_absolute_offset.value())
                                       : Result<std::uint64_t>::failure(chain_absolute_offset.error());
        if (!chain_offset || !chain_absolute_offset || !chain_address)
        {
            const Error* error = !chain_offset          ? &chain_offset.error()
                                 : !chain_absolute_offset ? &chain_absolute_offset.error()
                                                           : &chain_address.error();
            return Result<std::size_t>::failure(*error);
        }
        const auto chain = read_u32(memory, chain_address.value(), "DT_GNU_HASH chain");
        if (!chain)
        {
            return Result<std::size_t>::failure(chain.error());
        }
        if ((chain.value() & 1U) != 0U)
        {
            return Result<std::size_t>::success(static_cast<std::size_t>(symbol_count.value()));
        }
    }
}

[[nodiscard]] Result<std::size_t> derive_symbol_count(const GuestMemory& memory,
                                                       const DynamicInfo& dynamic,
                                                       const DynamicParseLimits& limits)
{
    if (dynamic.hash)
    {
        const auto sysv = derive_sysv_count(memory, dynamic, limits.max_symbols);
        if (!sysv)
        {
            return sysv;
        }
        if (dynamic.gnu_hash)
        {
            const auto gnu = derive_gnu_count(memory, dynamic, limits.max_symbols);
            if (!gnu)
            {
                return gnu;
            }
            if (gnu.value() != sysv.value())
            {
                return Result<std::size_t>::failure(make_error(
                    ErrorCode::InvalidFormat,
                    "DT_HASH and DT_GNU_HASH derive different dynsym sizes"));
            }
        }
        return sysv;
    }
    return derive_gnu_count(memory, dynamic, limits.max_symbols);
}

[[nodiscard]] SymbolBinding decode_binding(std::uint8_t info) noexcept
{
    switch (info >> 4U)
    {
    case 0U:
        return SymbolBinding::Local;
    case 1U:
        return SymbolBinding::Global;
    case 2U:
        return SymbolBinding::Weak;
    default:
        return SymbolBinding::Unknown;
    }
}

[[nodiscard]] SymbolType decode_type(std::uint8_t info) noexcept
{
    switch (info & 0x0fU)
    {
    case 0U:
        return SymbolType::None;
    case 1U:
        return SymbolType::Object;
    case 2U:
        return SymbolType::Function;
    case 3U:
        return SymbolType::Section;
    case 4U:
        return SymbolType::File;
    case 6U:
        return SymbolType::Tls;
    default:
        return SymbolType::Unknown;
    }
}

[[nodiscard]] SymbolVisibility decode_visibility(std::uint8_t other) noexcept
{
    switch (other & 0x03U)
    {
    case 0U:
        return SymbolVisibility::Default;
    case 1U:
        return SymbolVisibility::Internal;
    case 2U:
        return SymbolVisibility::Hidden;
    case 3U:
        return SymbolVisibility::Protected;
    default:
        return SymbolVisibility::Unknown;
    }
}

} // namespace

Result<DynamicStringTable> DynamicStringTable::parse(const GuestMemory& guest_memory,
                                                     const DynamicInfo& dynamic,
                                                     const DynamicParseLimits& limits)
{
    if (!dynamic.strtab || !dynamic.strsz)
    {
        return Result<DynamicStringTable>::failure(
            make_error(ErrorCode::InvalidFormat, "dynamic string table metadata is incomplete"));
    }
    if (dynamic.strsz.value() > limits.max_string_table_size)
    {
        return Result<DynamicStringTable>::failure(make_error(
            ErrorCode::ResourceLimit, "dynamic string table exceeds the configured size limit"));
    }
    if (dynamic.strsz.value() > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
    {
        return Result<DynamicStringTable>::failure(make_error(
            ErrorCode::ArithmeticOverflow, "dynamic string table size does not fit host size_t"));
    }

    try
    {
        const auto size = static_cast<std::size_t>(dynamic.strsz.value());
        std::vector<std::byte> bytes(size);
        if (!bytes.empty())
        {
            const auto read = guest_memory.read(dynamic.strtab->address, bytes);
            if (!read)
            {
                return Result<DynamicStringTable>::failure(make_error(
                    read.error().code,
                    "dynamic string table is not readable: " + read.error().message));
            }
        }
        std::string owned;
        owned.reserve(size);
        for (const auto byte : bytes)
        {
            owned.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
        }
        return Result<DynamicStringTable>::success(DynamicStringTable(std::move(owned)));
    }
    catch (const std::bad_alloc&)
    {
        return Result<DynamicStringTable>::failure(
            make_error(ErrorCode::ResourceLimit, "dynamic string table allocation failed"));
    }
    catch (const std::length_error&)
    {
        return Result<DynamicStringTable>::failure(
            make_error(ErrorCode::ResourceLimit, "dynamic string table exceeds host limits"));
    }
}

Result<std::string_view> DynamicStringTable::get(std::uint64_t offset) const
{
    if (offset >= bytes_.size())
    {
        return Result<std::string_view>::failure(make_error(
            ErrorCode::StringTableOutOfBounds,
            "dynamic string offset " + std::to_string(offset) + " is outside DT_STRSZ"));
    }
    const auto start = static_cast<std::size_t>(offset);
    const auto end = bytes_.find('\0', start);
    if (end == std::string::npos)
    {
        return Result<std::string_view>::failure(make_error(
            ErrorCode::UnterminatedSymbolName,
            "dynamic string at offset " + std::to_string(offset) + " is not NUL terminated"));
    }
    return Result<std::string_view>::success(std::string_view(bytes_).substr(start, end - start));
}

std::string_view symbol_binding_name(SymbolBinding binding) noexcept
{
    switch (binding)
    {
    case SymbolBinding::Local:
        return "LOCAL";
    case SymbolBinding::Global:
        return "GLOBAL";
    case SymbolBinding::Weak:
        return "WEAK";
    case SymbolBinding::Unknown:
        return "UNKNOWN";
    }
    return "UNKNOWN";
}

std::string_view symbol_type_name(SymbolType type) noexcept
{
    switch (type)
    {
    case SymbolType::None:
        return "NOTYPE";
    case SymbolType::Object:
        return "OBJECT";
    case SymbolType::Function:
        return "FUNC";
    case SymbolType::Section:
        return "SECTION";
    case SymbolType::File:
        return "FILE";
    case SymbolType::Tls:
        return "TLS";
    case SymbolType::Unknown:
        return "UNKNOWN";
    }
    return "UNKNOWN";
}

std::string_view symbol_visibility_name(SymbolVisibility visibility) noexcept
{
    switch (visibility)
    {
    case SymbolVisibility::Default:
        return "DEFAULT";
    case SymbolVisibility::Internal:
        return "INTERNAL";
    case SymbolVisibility::Hidden:
        return "HIDDEN";
    case SymbolVisibility::Protected:
        return "PROTECTED";
    case SymbolVisibility::Unknown:
        return "UNKNOWN";
    }
    return "UNKNOWN";
}

Result<DynamicSymbolTable> DynamicSymbolTable::parse(const GuestMemory& guest_memory,
                                                     const DynamicInfo& dynamic,
                                                     const DynamicParseLimits& limits)
{
    if (!dynamic.symtab || !dynamic.syment)
    {
        return Result<DynamicSymbolTable>::failure(
            make_error(ErrorCode::InvalidFormat, "dynamic symbol table metadata is incomplete"));
    }
    if (dynamic.syment.value() != elf64_sym_size)
    {
        return Result<DynamicSymbolTable>::failure(make_error(
            ErrorCode::InvalidSymbolEntrySize,
            "DT_SYMENT is " + std::to_string(dynamic.syment.value()) + ", expected " +
                std::to_string(elf64_sym_size)));
    }

    const auto strings = DynamicStringTable::parse(guest_memory, dynamic, limits);
    if (!strings)
    {
        return Result<DynamicSymbolTable>::failure(strings.error());
    }
    const auto count = derive_symbol_count(guest_memory, dynamic, limits);
    if (!count)
    {
        return Result<DynamicSymbolTable>::failure(count.error());
    }
    if (count.value() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
    {
        return Result<DynamicSymbolTable>::failure(make_error(
            ErrorCode::ResourceLimit, "dynamic symbol count exceeds the 32-bit symbol index domain"));
    }

    const auto table_size = checked_mul_u64(static_cast<std::uint64_t>(count.value()),
                                            static_cast<std::uint64_t>(elf64_sym_size));
    if (!table_size)
    {
        return Result<DynamicSymbolTable>::failure(table_size.error());
    }
    const auto table_readable = require_readable(guest_memory, dynamic.symtab->address,
                                                 table_size.value(), "DT_SYMTAB");
    if (!table_readable)
    {
        return Result<DynamicSymbolTable>::failure(table_readable.error());
    }

    try
    {
        DynamicSymbolTable result{ {}, strings.value() };
        result.symbols.reserve(count.value());
        for (std::size_t index = 0U; index < count.value(); ++index)
        {
            const auto byte_offset = checked_mul_u64(static_cast<std::uint64_t>(index),
                                                     static_cast<std::uint64_t>(elf64_sym_size));
            const auto address = byte_offset
                                     ? checked_add_u64(dynamic.symtab->address, byte_offset.value())
                                     : Result<std::uint64_t>::failure(byte_offset.error());
            if (!byte_offset || !address)
            {
                const Error* error = !byte_offset ? &byte_offset.error() : &address.error();
                return Result<DynamicSymbolTable>::failure(*error);
            }

            std::array<std::byte, elf64_sym_size> bytes{};
            const auto read = guest_memory.read(address.value(), bytes);
            if (!read)
            {
                return Result<DynamicSymbolTable>::failure(make_error(
                    read.error().code,
                    "symbol[" + std::to_string(index) + "] is not readable: " +
                        read.error().message));
            }
            const BinaryReader reader(bytes);
            const auto name_offset = reader.read_u32_le(0U);
            const auto raw_info = reader.slice(4U, 1U);
            const auto raw_other = reader.slice(5U, 1U);
            const auto section = reader.read_u16_le(6U);
            const auto value = reader.read_u64_le(8U);
            const auto size = reader.read_u64_le(16U);
            if (!name_offset || !raw_info || !raw_other || !section || !value || !size)
            {
                const Error* error = !name_offset ? &name_offset.error()
                                  : !raw_info    ? &raw_info.error()
                                  : !raw_other   ? &raw_other.error()
                                  : !section     ? &section.error()
                                  : !value       ? &value.error()
                                                 : &size.error();
                return Result<DynamicSymbolTable>::failure(make_error(
                    error->code, "failed to decode symbol[" + std::to_string(index) + "]: " +
                                     error->message));
            }
            const auto symbol_name = strings.value().get(name_offset.value());
            if (!symbol_name)
            {
                return Result<DynamicSymbolTable>::failure(make_error(
                    symbol_name.error().code,
                    "symbol[" + std::to_string(index) + "]: " + symbol_name.error().message));
            }
            const auto info = std::to_integer<std::uint8_t>(raw_info.value()[0]);
            const auto other = std::to_integer<std::uint8_t>(raw_other.value()[0]);
            result.symbols.push_back(DynamicSymbol{
                static_cast<std::uint32_t>(index), name_offset.value(), std::string(symbol_name.value()),
                decode_binding(info), decode_type(info), decode_visibility(other), section.value(),
                value.value(), size.value()});
        }
        return Result<DynamicSymbolTable>::success(std::move(result));
    }
    catch (const std::bad_alloc&)
    {
        return Result<DynamicSymbolTable>::failure(
            make_error(ErrorCode::ResourceLimit, "dynamic symbol table allocation failed"));
    }
    catch (const std::length_error&)
    {
        return Result<DynamicSymbolTable>::failure(
            make_error(ErrorCode::ResourceLimit, "dynamic symbol table exceeds host limits"));
    }
}

const DynamicSymbol* DynamicSymbolTable::at(std::uint32_t index) const noexcept
{
    if (index >= symbols.size())
    {
        return nullptr;
    }
    return &symbols[index];
}

std::vector<ImportSymbol> DynamicSymbolTable::imports() const
{
    std::vector<ImportSymbol> result;
    for (const auto& symbol : symbols)
    {
        if (!symbol.is_defined())
        {
            result.push_back(ImportSymbol{symbol.index, symbol.name, symbol.binding, symbol.type,
                                          symbol.visibility});
        }
    }
    return result;
}

} // namespace switchrecomp::format
