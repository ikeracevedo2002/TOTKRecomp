#include "switchrecomp/loader/relocation_processor.hpp"

#include "switchrecomp/common/checked_arithmetic.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace switchrecomp::loader
{

namespace
{

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

[[nodiscard]] Result<void> validate_target(const memory::GuestMemory& guest_memory,
                                            const format::Relocation& relocation,
                                            std::size_t index,
                                            const RelocationProcessorOptions& options)
{
    if ((relocation.target_address % sizeof(std::uint64_t)) != 0U)
    {
        return Result<void>::failure(make_error(
            ErrorCode::MisalignedRelocationTarget,
            "relocation[" + std::to_string(index) + "] target is not 8-byte aligned"));
    }

    const auto target_end = checked_add_u64(relocation.target_address, sizeof(std::uint64_t));
    if (!target_end)
    {
        return Result<void>::failure(make_error(
            target_end.error().code,
            "relocation[" + std::to_string(index) + "] target range overflows"));
    }

    if (options.use_loader_write)
    {
        const auto valid_target = guest_memory.validate_loader_write(
            relocation.target_address, sizeof(std::uint64_t));
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
            relocation.target_address, sizeof(std::uint64_t));
        if (!permissions)
        {
            return Result<void>::failure(make_error(
                permissions.error().code,
                "relocation[" + std::to_string(index) + "] target " +
                    std::to_string(relocation.target_address) + ": " +
                    permissions.error().message));
        }
        if (!memory::has_permission(permissions.value(), memory::GuestMemoryPermissions::Write))
        {
            return Result<void>::failure(make_error(
                ErrorCode::PermissionDenied,
                "relocation[" + std::to_string(index) +
                    "] target mapping does not grant write permission"));
        }
    }
    return Result<void>::success();
}

[[nodiscard]] format::ImportSymbol import_from(const ResolvedSymbol& symbol)
{
    return format::ImportSymbol{symbol.symbol_index, symbol.name, symbol.binding, symbol.type,
                                symbol.visibility};
}

void encode_u64_le(std::uint64_t value,
                   std::array<std::byte, sizeof(std::uint64_t)>& bytes) noexcept
{
    for (std::size_t index = 0U; index < bytes.size(); ++index)
    {
        bytes[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

} // namespace

Result<RelocationPlan> plan_relocations(const memory::GuestMemory& guest_memory,
                                        std::span<const format::Relocation> relocations,
                                        const SymbolResolver& resolver,
                                        const RelocationProcessorOptions& options)
{
    try
    {
        RelocationPlan plan;
        plan.relocation_count = relocations.size();
        plan.applied.reserve(relocations.size());

        for (std::size_t index = 0U; index < relocations.size(); ++index)
        {
            const auto& relocation = relocations[index];
            if (relocation.type == format::AArch64RelocationType::None)
            {
                continue;
            }
            if (relocation.type == format::AArch64RelocationType::Unknown)
            {
                return Result<RelocationPlan>::failure(make_error(
                    ErrorCode::UnsupportedRelocationType,
                    "relocation[" + std::to_string(index) + "] has unsupported AArch64 type " +
                        std::to_string(relocation.raw_type)));
            }

            std::optional<std::uint64_t> value;
            if (relocation.type == format::AArch64RelocationType::Relative)
            {
                const auto calculated = addend_result(resolver.module_base_for_relocation(),
                                                      relocation.addend, index);
                if (!calculated)
                {
                    return Result<RelocationPlan>::failure(calculated.error());
                }
                value = calculated.value();
            }
            else
            {
                const auto resolved = resolver.resolve_for_relocation(relocation.symbol_index);
                if (!resolved)
                {
                    return Result<RelocationPlan>::failure(make_error(
                        resolved.error().code,
                        "relocation[" + std::to_string(index) + "] symbol " +
                            std::to_string(relocation.symbol_index) + ": " +
                            resolved.error().message));
                }
                if (!resolved.value().resolved)
                {
                    const auto target = validate_target(guest_memory, relocation, index, options);
                    if (!target)
                    {
                        return Result<RelocationPlan>::failure(target.error());
                    }
                    plan.unresolved.push_back(UnresolvedRelocation{
                        index, relocation, import_from(resolved.value())});
                    continue;
                }
                const auto calculated = addend_result(resolved.value().address,
                                                      relocation.addend, index);
                if (!calculated)
                {
                    return Result<RelocationPlan>::failure(calculated.error());
                }
                value = calculated.value();
            }

            const auto target = validate_target(guest_memory, relocation, index, options);
            if (!target)
            {
                return Result<RelocationPlan>::failure(target.error());
            }
            plan.applied.push_back(AppliedRelocation{index, relocation, value.value()});
        }
        return Result<RelocationPlan>::success(std::move(plan));
    }
    catch (const std::bad_alloc&)
    {
        return Result<RelocationPlan>::failure(
            make_error(ErrorCode::ResourceLimit, "relocation planning allocation failed"));
    }
    catch (const std::length_error&)
    {
        return Result<RelocationPlan>::failure(
            make_error(ErrorCode::ResourceLimit, "relocation plan exceeds host limits"));
    }
}

Result<void> apply_relocation_plan(memory::GuestMemory& guest_memory, const RelocationPlan& plan,
                                   const RelocationProcessorOptions& options)
{
    try
    {
        // Revalidate the complete write set before committing any bytes. This
        // keeps a retained plan safe even if its memory was changed meanwhile.
        for (const auto& entry : plan.applied)
        {
            const auto target = validate_target(guest_memory, entry.relocation,
                                                entry.relocation_index, options);
            if (!target)
            {
                return target;
            }
        }

        for (const auto& entry : plan.applied)
        {
            std::array<std::byte, sizeof(std::uint64_t)> bytes{};
            encode_u64_le(entry.value, bytes);
            const auto result = options.use_loader_write
                                    ? guest_memory.loader_write(entry.relocation.target_address,
                                                                bytes)
                                    : guest_memory.write(entry.relocation.target_address, bytes);
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
            make_error(ErrorCode::ResourceLimit, "relocation application allocation failed"));
    }
}

Result<void> apply_relocations(memory::GuestMemory& guest_memory,
                               std::span<const format::Relocation> relocations,
                               const SymbolResolver& resolver,
                               const RelocationProcessorOptions& options)
{
    const auto plan = plan_relocations(guest_memory, relocations, resolver, options);
    if (!plan)
    {
        return Result<void>::failure(plan.error());
    }
    if (!plan.value().unresolved.empty())
    {
        const auto& unresolved = plan.value().unresolved.front();
        const auto code = unresolved.symbol.binding == format::SymbolBinding::Weak
                              ? ErrorCode::MissingImportBinding
                              : ErrorCode::UndefinedStrongSymbol;
        return Result<void>::failure(make_error(
            code, "relocation[" + std::to_string(unresolved.relocation_index) + "] symbol " +
                     std::to_string(unresolved.symbol.symbol_index) + ": unresolved external '" +
                     unresolved.symbol.name + "'"));
    }
    return apply_relocation_plan(guest_memory, plan.value(), options);
}

} // namespace switchrecomp::loader
