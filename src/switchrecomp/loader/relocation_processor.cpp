#include "switchrecomp/loader/relocation_processor.hpp"

#include "switchrecomp/common/checked_arithmetic.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace switchrecomp::loader
{

namespace
{

struct PendingWrite
{
    memory::GuestAddress address;
    std::array<std::byte, sizeof(std::uint64_t)> bytes;
};

void encode_u64_le(std::uint64_t value, std::array<std::byte, sizeof(std::uint64_t)>& bytes) noexcept
{
    for (std::size_t index = 0U; index < bytes.size(); ++index)
    {
        bytes[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

[[nodiscard]] Result<std::uint64_t> addend_result(std::uint64_t value, std::int64_t addend,
                                                   std::size_t index)
{
    const auto result = checked_add_signed_u64(value, addend);
    if (!result)
    {
        return Result<std::uint64_t>::failure(make_error(
            result.error().code,
            "relocation[" + std::to_string(index) + "] S/B + A arithmetic failed: " +
                result.error().message));
    }
    return result;
}

} // namespace

Result<void> apply_relocations(memory::GuestMemory& guest_memory,
                               std::span<const format::Relocation> relocations,
                               const SymbolResolver& resolver,
                               const RelocationProcessorOptions& options)
{
    try
    {
        std::vector<PendingWrite> staged;
        staged.reserve(relocations.size());

        for (std::size_t index = 0U; index < relocations.size(); ++index)
        {
            const auto& relocation = relocations[index];
            if (relocation.type == format::AArch64RelocationType::None)
            {
                continue;
            }
            if (relocation.type == format::AArch64RelocationType::Unknown)
            {
                return Result<void>::failure(make_error(
                    ErrorCode::UnsupportedRelocationType,
                    "relocation[" + std::to_string(index) + "] has unsupported AArch64 type " +
                        std::to_string(relocation.raw_type)));
            }

            if ((relocation.target_address % sizeof(std::uint64_t)) != 0U)
            {
                return Result<void>::failure(make_error(
                    ErrorCode::MisalignedRelocationTarget,
                    "relocation[" + std::to_string(index) + "] target is not 8-byte aligned"));
            }

            std::uint64_t value = 0U;
            if (relocation.type == format::AArch64RelocationType::Relative)
            {
                // R_AARCH64_RELATIVE: B + A. The module base is retained by
                const auto relative = resolver.module_base_for_relocation();
                const auto calculated = addend_result(relative, relocation.addend, index);
                if (!calculated)
                {
                    return Result<void>::failure(calculated.error());
                }
                value = calculated.value();
            }
            else
            {
                const auto symbol = resolver.resolve(relocation.symbol_index);
                if (!symbol)
                {
                    return Result<void>::failure(make_error(
                        symbol.error().code,
                        "relocation[" + std::to_string(index) + "] symbol " +
                            std::to_string(relocation.symbol_index) + ": " +
                            symbol.error().message));
                }
                const auto calculated = addend_result(symbol.value().address, relocation.addend, index);
                if (!calculated)
                {
                    return Result<void>::failure(calculated.error());
                }
                value = calculated.value();
            }

            const auto target_end = checked_add_u64(relocation.target_address, sizeof(value));
            if (!target_end)
            {
                return Result<void>::failure(make_error(
                    target_end.error().code,
                    "relocation[" + std::to_string(index) + "] target range overflows"));
            }
            if (options.use_loader_write)
            {
                const auto valid_target = guest_memory.validate_loader_write(
                    relocation.target_address, sizeof(value));
                if (!valid_target)
                {
                    return Result<void>::failure(make_error(
                        valid_target.error().code,
                        "relocation[" + std::to_string(index) + "] target " +
                            std::to_string(relocation.target_address) + ": " +
                            valid_target.error().message));
                }
            }
            else
            {
                const auto permissions = guest_memory.permissions_at(
                    relocation.target_address, sizeof(value));
                if (!permissions)
                {
                    return Result<void>::failure(make_error(
                        permissions.error().code,
                        "relocation[" + std::to_string(index) + "] target " +
                            std::to_string(relocation.target_address) + ": " +
                            permissions.error().message));
                }
                if (!memory::has_permission(permissions.value(),
                                            memory::GuestMemoryPermissions::Write))
                {
                    return Result<void>::failure(make_error(
                        ErrorCode::PermissionDenied,
                        "relocation[" + std::to_string(index) +
                            "] target mapping does not grant write permission"));
                }
            }

            PendingWrite write{relocation.target_address, {}};
            encode_u64_le(value, write.bytes);
            staged.push_back(write);
        }

        for (const auto& write : staged)
        {
            const auto result = options.use_loader_write
                                    ? guest_memory.loader_write(write.address, write.bytes)
                                    : guest_memory.write(write.address, write.bytes);
            if (!result)
            {
                return Result<void>::failure(result.error());
            }
        }
        return Result<void>::success();
    }
    catch (const std::bad_alloc&)
    {
        return Result<void>::failure(
            make_error(ErrorCode::ResourceLimit, "relocation staging allocation failed"));
    }
    catch (const std::length_error&)
    {
        return Result<void>::failure(
            make_error(ErrorCode::ResourceLimit, "relocation staging exceeds host limits"));
    }
}

} // namespace switchrecomp::loader
