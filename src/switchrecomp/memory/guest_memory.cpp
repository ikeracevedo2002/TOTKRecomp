#include "switchrecomp/memory/guest_memory.hpp"

#include "switchrecomp/common/checked_arithmetic.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <new>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace switchrecomp::memory
{

namespace
{

[[nodiscard]] std::string hex_address(GuestAddress address)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::setw(16) << std::setfill('0') << address;
    return output.str();
}

[[nodiscard]] Error mapping_error(ErrorCode code, std::string message)
{
    return make_error(code, "guest memory mapping " + std::move(message));
}

[[nodiscard]] Error access_error(ErrorCode code, std::string_view operation, GuestAddress address,
                                 GuestSize size, std::string message)
{
    return make_error(code, "guest " + std::string(operation) + " at address " +
                                hex_address(address) + " for " + std::to_string(size) +
                                " bytes: " + std::move(message));
}

[[nodiscard]] Result<std::size_t> host_size(GuestSize size)
{
    if (size > static_cast<GuestSize>(std::numeric_limits<std::size_t>::max()))
    {
        return Result<std::size_t>::failure(
            make_error(ErrorCode::ArithmeticOverflow,
                       "guest mapping size does not fit in the host size type"));
    }
    return Result<std::size_t>::success(static_cast<std::size_t>(size));
}

[[nodiscard]] Result<GuestSize> guest_size_from_host(std::size_t size)
{
    if (size > static_cast<std::size_t>(std::numeric_limits<GuestSize>::max()))
    {
        return Result<GuestSize>::failure(make_error(
            ErrorCode::ArithmeticOverflow, "host mapping size does not fit in the guest size type"));
    }
    return Result<GuestSize>::success(static_cast<GuestSize>(size));
}

} // namespace

std::string_view guest_region_kind_name(GuestRegionKind kind) noexcept
{
    switch (kind)
    {
    case GuestRegionKind::Other:
        return "other";
    case GuestRegionKind::Text:
        return "text";
    case GuestRegionKind::Rodata:
        return "rodata";
    case GuestRegionKind::Data:
        return "data";
    case GuestRegionKind::Bss:
        return "bss";
    }
    return "unknown";
}

GuestMemory::GuestMemory(GuestMemoryLimits limits)
    : limits_(limits), ownership_domain_(std::make_shared<OwnershipDomain>())
{
}

GuestMemory::GuestMemory(const GuestMemory& other)
    : limits_(other.limits_), ownership_domain_(std::make_shared<OwnershipDomain>()),
      total_mapped_size_(other.total_mapped_size_),
      peak_live_mapped_size_(other.peak_live_mapped_size_),
      cumulative_mapped_size_(other.cumulative_mapped_size_),
      peak_region_count_(other.peak_region_count_),
      owned_mappings_created_(other.owned_mappings_created_),
      owned_mappings_reclaimed_(other.owned_mappings_reclaimed_),
      live_owned_mappings_(other.live_owned_mappings_),
      peak_live_owned_mappings_(other.peak_live_owned_mappings_),
      live_owned_size_(other.live_owned_size_),
      peak_live_owned_size_(other.peak_live_owned_size_),
      cumulative_owned_size_(other.cumulative_owned_size_),
      virtual_address_high_water_(other.virtual_address_high_water_), regions_(other.regions_)
{
    // Staging copies are used by the loader before controlled execution starts.
    // A live dynamic mapping has an owner token tied to the source object and
    // must never be copied into a second object without a corresponding token.
    if (other.live_owned_mappings_ != 0U || other.live_owned_size_ != 0U)
    {
        throw std::logic_error("cannot copy GuestMemory with live owned mappings");
    }
}

GuestMemory& GuestMemory::operator=(const GuestMemory& other)
{
    if (this == &other) return *this;
    GuestMemory copy(other);
    *this = std::move(copy);
    return *this;
}

Result<void> GuestMemory::map(GuestAddress base, GuestSize size,
                              GuestMemoryPermissions permissions, std::string_view name,
                              GuestRegionKind kind)
{
    return map_bytes(base, {}, size, permissions, name, kind);
}

Result<void> GuestMemory::map(GuestAddress base, std::span<const std::byte> initial_data,
                              GuestMemoryPermissions permissions, std::string_view name,
                              GuestRegionKind kind)
{
    const auto size = guest_size_from_host(initial_data.size());
    if (!size) return Result<void>::failure(size.error());
    return map_bytes(base, initial_data, size.value(), permissions, name, kind);
}

Result<GuestMemoryMappingToken> GuestMemory::map_owned(
    GuestAddress base, GuestSize size, GuestMemoryPermissions permissions, std::string_view name,
    GuestRegionKind kind)
{
    if (size == 0U)
    {
        const auto validated = map_bytes(base, {}, size, permissions, name, kind);
        if (!validated)
        {
            return Result<GuestMemoryMappingToken>::failure(validated.error());
        }
        return Result<GuestMemoryMappingToken>::success(GuestMemoryMappingToken{});
    }
    std::shared_ptr<const void> identity;
    try
    {
        identity = std::make_shared<OwnedMappingIdentity>();
    }
    catch (const std::bad_alloc&)
    {
        return Result<GuestMemoryMappingToken>::failure(
            mapping_error(ErrorCode::ResourceLimit, "owned mapping identity allocation failed"));
    }
    const auto mapped = map_bytes(base, {}, size, permissions, name, kind, identity);
    if (!mapped)
    {
        return Result<GuestMemoryMappingToken>::failure(mapped.error());
    }
    return Result<GuestMemoryMappingToken>::success(
        GuestMemoryMappingToken{ownership_domain_, std::move(identity), base, size});
}

Result<GuestMemoryMappingToken> GuestMemory::map_owned(
    GuestAddress base, std::span<const std::byte> initial_data,
    GuestMemoryPermissions permissions, std::string_view name, GuestRegionKind kind)
{
    if (initial_data.empty())
    {
        const auto size = guest_size_from_host(initial_data.size());
        if (!size) return Result<GuestMemoryMappingToken>::failure(size.error());
        const auto validated = map_bytes(base, initial_data, size.value(), permissions, name, kind);
        if (!validated)
        {
            return Result<GuestMemoryMappingToken>::failure(validated.error());
        }
        return Result<GuestMemoryMappingToken>::success(GuestMemoryMappingToken{});
    }
    std::shared_ptr<const void> identity;
    try
    {
        identity = std::make_shared<OwnedMappingIdentity>();
    }
    catch (const std::bad_alloc&)
    {
        return Result<GuestMemoryMappingToken>::failure(
            mapping_error(ErrorCode::ResourceLimit, "owned mapping identity allocation failed"));
    }
    const auto size = guest_size_from_host(initial_data.size());
    if (!size) return Result<GuestMemoryMappingToken>::failure(size.error());
    const auto mapped = map_bytes(base, initial_data, size.value(), permissions, name, kind,
                                  identity);
    if (!mapped)
    {
        return Result<GuestMemoryMappingToken>::failure(mapped.error());
    }
    return Result<GuestMemoryMappingToken>::success(
        GuestMemoryMappingToken{ownership_domain_, std::move(identity), base, size.value()});
}

Result<void> GuestMemory::map_bytes(GuestAddress base, std::span<const std::byte> initial_data,
                                    GuestSize size, GuestMemoryPermissions permissions,
                                    std::string_view name, GuestRegionKind kind,
                                    std::shared_ptr<const void> owned_mapping)
{
    if (!is_valid_permissions(permissions))
    {
        return Result<void>::failure(
            mapping_error(ErrorCode::InvalidArgument, "contains unknown permission bits"));
    }
    if (static_cast<GuestSize>(initial_data.size()) > size)
    {
        return Result<void>::failure(mapping_error(
            ErrorCode::InvalidArgument, "initial data is larger than the requested guest region"));
    }
    if (size == 0U)
    {
        return Result<void>::success();
    }
    if (size > limits_.max_region_size)
    {
        return Result<void>::failure(mapping_error(
            ErrorCode::ResourceLimit,
            "requested region size exceeds the configured maximum region size (requested " +
                std::to_string(size) + ", limit " +
                std::to_string(limits_.max_region_size) + ")"));
    }

    const auto range = checked_guest_range(base, size);
    if (!range)
    {
        return Result<void>::failure(
            mapping_error(ErrorCode::ArithmeticOverflow,
                          "guest address range overflows the 64-bit address space"));
    }
    const auto host_region_size = host_size(size);
    if (!host_region_size)
    {
        return Result<void>::failure(host_region_size.error());
    }
    const auto total = checked_add_u64(total_mapped_size_, size);
    if (!total)
    {
        return Result<void>::failure(
            mapping_error(ErrorCode::ArithmeticOverflow,
                          "total mapped guest size overflows the 64-bit size domain"));
    }
    if (total.value() > limits_.max_total_size)
    {
        return Result<void>::failure(mapping_error(
            ErrorCode::ResourceLimit,
            "total mapped guest size exceeds the configured maximum (consumed " +
                std::to_string(total.value()) + ", limit " +
                std::to_string(limits_.max_total_size) + ")"));
    }
    const auto next_region_count = checked_add(regions_.size(), 1U);
    if (!next_region_count)
    {
        return Result<void>::failure(
            mapping_error(ErrorCode::ArithmeticOverflow, "region count overflows"));
    }
    if (next_region_count.value() > limits_.max_regions)
    {
        return Result<void>::failure(mapping_error(
            ErrorCode::ResourceLimit,
            "number of guest regions exceeds the configured maximum (consumed " +
                std::to_string(next_region_count.value()) + ", limit " +
                std::to_string(limits_.max_regions) + ")"));
    }

    const auto next_cumulative = checked_add_u64(cumulative_mapped_size_, size);
    if (!next_cumulative)
    {
        return Result<void>::failure(
            mapping_error(ErrorCode::ArithmeticOverflow, "cumulative mapped size overflows"));
    }
    const bool is_owned = owned_mapping != nullptr;
    const auto next_created = checked_add(owned_mappings_created_, is_owned ? 1U : 0U);
    if (!next_created)
    {
        return Result<void>::failure(
            mapping_error(ErrorCode::ArithmeticOverflow, "owned mapping count overflows"));
    }
    const auto next_owned_size = is_owned
                                     ? checked_add_u64(live_owned_size_, size)
                                     : Result<GuestSize>::success(live_owned_size_);
    if (!next_owned_size)
    {
        return Result<void>::failure(
            mapping_error(ErrorCode::ArithmeticOverflow, "live owned mapping size overflows"));
    }
    const auto next_cumulative_owned = is_owned
                                           ? checked_add_u64(cumulative_owned_size_, size)
                                           : Result<GuestSize>::success(cumulative_owned_size_);
    if (!next_cumulative_owned)
    {
        return Result<void>::failure(
            mapping_error(ErrorCode::ArithmeticOverflow,
                          "cumulative owned mapping size overflows"));
    }
    const auto next_live_owned_mappings = is_owned
                                              ? checked_add(live_owned_mappings_, 1U)
                                              : Result<std::size_t>::success(live_owned_mappings_);
    if (!next_live_owned_mappings)
    {
        return Result<void>::failure(
            mapping_error(ErrorCode::ArithmeticOverflow, "live owned mapping count overflows"));
    }

    const auto next = std::lower_bound(regions_.begin(), regions_.end(), base,
                                       [](const GuestRegion& region, GuestAddress value)
                                       { return region.info.base < value; });
    if (next != regions_.end() && next->info.base < range.value().end())
    {
        return Result<void>::failure(
            mapping_error(ErrorCode::InvalidArgument, "requested range [" + hex_address(base) +
                                                          ", " + hex_address(range.value().end()) +
                                                          ") overlaps mapping " + next->info.name));
    }
    if (next != regions_.begin())
    {
        const auto& previous = *(next - 1);
        if (previous.info.end() > base)
        {
            return Result<void>::failure(mapping_error(
                ErrorCode::InvalidArgument, "requested range [" + hex_address(base) + ", " +
                                                hex_address(range.value().end()) +
                                                ") overlaps mapping " + previous.info.name));
        }
    }

    try
    {
        GuestRegion region{GuestMemoryRegionInfo{base, size, permissions, kind, std::string(name)},
                           std::vector<std::byte>(host_region_size.value(), std::byte{0}),
                           std::move(owned_mapping)};
        std::copy(initial_data.begin(), initial_data.end(), region.bytes.begin());

        const auto insertion_index = static_cast<std::size_t>(next - regions_.begin());
        regions_.reserve(next_region_count.value());
        regions_.insert(regions_.begin() + static_cast<std::ptrdiff_t>(insertion_index),
                        std::move(region));
    }
    catch (const std::bad_alloc&)
    {
        return Result<void>::failure(
            mapping_error(ErrorCode::ResourceLimit, "host backing storage allocation failed"));
    }
    catch (const std::length_error&)
    {
        return Result<void>::failure(mapping_error(
            ErrorCode::ResourceLimit, "host backing storage exceeds container limits"));
    }

    total_mapped_size_ = total.value();
    cumulative_mapped_size_ = next_cumulative.value();
    peak_live_mapped_size_ = std::max(peak_live_mapped_size_, total.value());
    peak_region_count_ = std::max(peak_region_count_, next_region_count.value());
    virtual_address_high_water_ = std::max(virtual_address_high_water_, range.value().end());
    if (is_owned)
    {
        owned_mappings_created_ = next_created.value();
        live_owned_mappings_ = next_live_owned_mappings.value();
        live_owned_size_ = next_owned_size.value();
        peak_live_owned_mappings_ =
            std::max(peak_live_owned_mappings_, live_owned_mappings_);
        peak_live_owned_size_ = std::max(peak_live_owned_size_, live_owned_size_);
        cumulative_owned_size_ = next_cumulative_owned.value();
    }
    return Result<void>::success();
}

Result<void> GuestMemory::release_owned(const GuestMemoryMappingToken& token)
{
    if (token.memory_domain_ == nullptr || token.mapping_identity_ == nullptr ||
        token.memory_domain_.get() != ownership_domain_.get())
    {
        return Result<void>::failure(mapping_error(
            ErrorCode::InvalidArgument, "mapping ownership token is not from this memory"));
    }

    const auto found = std::lower_bound(
        regions_.begin(), regions_.end(), token.base_,
        [](const GuestRegion& region, GuestAddress value) { return region.info.base < value; });
    if (found == regions_.end() || found->info.base != token.base_ ||
        found->info.size != token.size_ || found->owned_mapping == nullptr ||
        found->owned_mapping != token.mapping_identity_)
    {
        return Result<void>::failure(mapping_error(
            ErrorCode::InvalidArgument, "mapping ownership token does not identify a live mapping"));
    }

    const auto next_total = checked_sub_u64(total_mapped_size_, found->info.size);
    const auto next_owned_size = checked_sub_u64(live_owned_size_, found->info.size);
    const auto next_regions = checked_sub(regions_.size(), 1U);
    const auto next_live_owned = checked_sub(live_owned_mappings_, 1U);
    const auto next_reclaimed = checked_add(owned_mappings_reclaimed_, 1U);
    if (!next_total || !next_owned_size || !next_regions || !next_live_owned || !next_reclaimed ||
        next_reclaimed.value() > owned_mappings_created_)
    {
        return Result<void>::failure(
            mapping_error(ErrorCode::ArithmeticUnderflow,
                          "owned mapping accounting would underflow during release"));
    }

    regions_.erase(found);
    total_mapped_size_ = next_total.value();
    live_owned_size_ = next_owned_size.value();
    live_owned_mappings_ = next_live_owned.value();
    owned_mappings_reclaimed_ = next_reclaimed.value();
    return Result<void>::success();
}

const GuestMemory::GuestRegion* GuestMemory::find_region(GuestAddress address) const noexcept
{
    const auto next = std::lower_bound(regions_.begin(), regions_.end(), address,
                                       [](const GuestRegion& region, GuestAddress value)
                                       { return region.info.base < value; });
    if (next != regions_.end() && next->info.base == address)
    {
        return &*next;
    }
    if (next == regions_.begin())
    {
        return nullptr;
    }
    const auto& previous = *(next - 1);
    return address < previous.info.end() ? &previous : nullptr;
}

GuestMemory::GuestRegion* GuestMemory::find_region(GuestAddress address) noexcept
{
    const auto next = std::lower_bound(regions_.begin(), regions_.end(), address,
                                       [](const GuestRegion& region, GuestAddress value)
                                       { return region.info.base < value; });
    if (next != regions_.end() && next->info.base == address)
    {
        return &*next;
    }
    if (next == regions_.begin())
    {
        return nullptr;
    }
    auto previous = next - 1;
    return address < previous->info.end() ? &*previous : nullptr;
}

Result<const GuestMemory::GuestRegion*> GuestMemory::validate_read_range(GuestAddress address,
                                                                         GuestSize size) const
{
    const auto mapped = validate_range(address, size, "read");
    if (!mapped)
    {
        return mapped;
    }
    if (!has_permission(mapped.value()->info.permissions, GuestMemoryPermissions::Read))
    {
        return Result<const GuestRegion*>::failure(
            access_error(ErrorCode::PermissionDenied, "read", address, size,
                         "mapping does not grant read permission"));
    }
    return mapped;
}

Result<const GuestMemory::GuestRegion*>
GuestMemory::validate_range(GuestAddress address, GuestSize size, std::string_view operation) const
{
    const auto range = checked_guest_range(address, size);
    if (!range)
    {
        return Result<const GuestRegion*>::failure(
            access_error(ErrorCode::ArithmeticOverflow, operation, address, size,
                         "address range overflows the 64-bit address space"));
    }
    const auto* region = find_region(address);
    if (region == nullptr)
    {
        return Result<const GuestRegion*>::failure(access_error(
            ErrorCode::UnmappedMemory, operation, address, size, "address is not mapped"));
    }
    if (range.value().end() > region->info.end())
    {
        return Result<const GuestRegion*>::failure(
            access_error(ErrorCode::UnmappedMemory, operation, address, size,
                         "range is not fully contained in one mapping"));
    }
    return Result<const GuestRegion*>::success(region);
}

Result<GuestMemory::GuestRegion*> GuestMemory::validate_write_range(GuestAddress address,
                                                                    GuestSize size)
{
    const auto range = checked_guest_range(address, size);
    if (!range)
    {
        return Result<GuestRegion*>::failure(
            access_error(ErrorCode::ArithmeticOverflow, "write", address, size,
                         "address range overflows the 64-bit address space"));
    }
    auto* region = find_region(address);
    if (region == nullptr)
    {
        return Result<GuestRegion*>::failure(access_error(ErrorCode::UnmappedMemory, "write",
                                                          address, size, "address is not mapped"));
    }
    if (range.value().end() > region->info.end())
    {
        return Result<GuestRegion*>::failure(
            access_error(ErrorCode::UnmappedMemory, "write", address, size,
                         "range is not fully contained in one mapping"));
    }
    if (!has_permission(region->info.permissions, GuestMemoryPermissions::Write))
    {
        return Result<GuestRegion*>::failure(
            access_error(ErrorCode::PermissionDenied, "write", address, size,
                         "mapping does not grant write permission"));
    }
    return Result<GuestRegion*>::success(region);
}

Result<void> GuestMemory::read(GuestAddress address, std::span<std::byte> destination) const
{
    if (destination.empty())
    {
        return Result<void>::success();
    }
    const auto result = validate_read_range(address, static_cast<GuestSize>(destination.size()));
    if (!result)
    {
        return Result<void>::failure(result.error());
    }
    const auto offset = static_cast<std::size_t>(address - result.value()->info.base);
    std::copy(result.value()->bytes.begin() + static_cast<std::ptrdiff_t>(offset),
              result.value()->bytes.begin() +
                  static_cast<std::ptrdiff_t>(offset + destination.size()),
              destination.begin());
    return Result<void>::success();
}

Result<void> GuestMemory::write(GuestAddress address, std::span<const std::byte> source)
{
    if (source.empty())
    {
        return Result<void>::success();
    }
    const auto result = validate_write_range(address, static_cast<GuestSize>(source.size()));
    if (!result)
    {
        return Result<void>::failure(result.error());
    }
    const auto offset = static_cast<std::size_t>(address - result.value()->info.base);
    std::copy(source.begin(), source.end(),
              result.value()->bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    return Result<void>::success();
}

Result<void> GuestMemory::validate_loader_write(GuestAddress address, GuestSize size) const
{
    if (size == 0U)
    {
        return Result<void>::success();
    }
    const auto range = checked_guest_range(address, size);
    if (!range)
    {
        return Result<void>::failure(access_error(
            ErrorCode::ArithmeticOverflow, "loader write", address, size,
            "address range overflows the 64-bit address space"));
    }
    const auto* region = find_region(address);
    if (region == nullptr || range.value().end() > region->info.end())
    {
        return Result<void>::failure(access_error(
            ErrorCode::RelocationTargetNotWritableDuringLoad, "loader write", address, size,
            "range is not fully contained in one mapped guest region"));
    }
    return Result<void>::success();
}

Result<void> GuestMemory::loader_write(GuestAddress address,
                                       std::span<const std::byte> source)
{
    if (source.empty())
    {
        return Result<void>::success();
    }
    const auto valid = validate_loader_write(address, static_cast<GuestSize>(source.size()));
    if (!valid)
    {
        return valid;
    }
    auto* region = find_region(address);
    const auto offset = static_cast<std::size_t>(address - region->info.base);
    std::copy(source.begin(), source.end(),
              region->bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    return Result<void>::success();
}

Result<GuestMemoryPermissions> GuestMemory::permissions_at(GuestAddress address,
                                                           GuestSize size) const
{
    if (size == 0U)
    {
        return Result<GuestMemoryPermissions>::failure(
            make_error(ErrorCode::InvalidArgument, "guest permission query cannot have zero size"));
    }
    const auto result = validate_range(address, size, "permission query");
    if (!result)
    {
        return Result<GuestMemoryPermissions>::failure(result.error());
    }
    return Result<GuestMemoryPermissions>::success(result.value()->info.permissions);
}

Result<bool> GuestMemory::is_executable(GuestAddress address, GuestSize size) const
{
    const auto permissions = permissions_at(address, size);
    if (!permissions)
    {
        return Result<bool>::failure(permissions.error());
    }
    return Result<bool>::success(
        has_permission(permissions.value(), GuestMemoryPermissions::Execute));
}

Result<GuestMemoryRegionInfo> GuestMemory::region_at(GuestAddress address) const
{
    const auto* region = find_region(address);
    if (region == nullptr)
    {
        return Result<GuestMemoryRegionInfo>::failure(make_error(
            ErrorCode::UnmappedMemory, "guest address " + hex_address(address) + " is not mapped"));
    }
    return Result<GuestMemoryRegionInfo>::success(region->info);
}

std::vector<GuestMemoryRegionInfo> GuestMemory::regions() const
{
    std::vector<GuestMemoryRegionInfo> result;
    result.reserve(regions_.size());
    for (const auto& region : regions_)
    {
        result.push_back(region.info);
    }
    return result;
}

std::size_t GuestMemory::region_count() const noexcept
{
    return regions_.size();
}

GuestSize GuestMemory::total_mapped_size() const noexcept
{
    return total_mapped_size_;
}

GuestMemoryAccounting GuestMemory::accounting() const noexcept
{
    return GuestMemoryAccounting{limits_.max_region_size,
                                 limits_.max_total_size,
                                 limits_.max_regions,
                                 total_mapped_size_,
                                 peak_live_mapped_size_,
                                 cumulative_mapped_size_,
                                 regions_.size(),
                                 peak_region_count_,
                                 owned_mappings_created_,
                                 owned_mappings_reclaimed_,
                                 live_owned_mappings_,
                                 peak_live_owned_mappings_,
                                 live_owned_size_,
                                 peak_live_owned_size_,
                                 cumulative_owned_size_,
                                 virtual_address_high_water_};
}

const GuestMemoryLimits& GuestMemory::limits() const noexcept
{
    return limits_;
}

} // namespace switchrecomp::memory
