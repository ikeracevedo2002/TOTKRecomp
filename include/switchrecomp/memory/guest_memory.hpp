#pragma once

#include "switchrecomp/common/result.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace switchrecomp::memory
{

using GuestAddress = std::uint64_t;
using GuestSize = std::uint64_t;

enum class GuestMemoryPermissions : std::uint8_t
{
    None = 0U,
    Read = 1U << 0U,
    Write = 1U << 1U,
    Execute = 1U << 2U,
};

[[nodiscard]] constexpr GuestMemoryPermissions operator|(GuestMemoryPermissions left,
                                                         GuestMemoryPermissions right) noexcept
{
    return static_cast<GuestMemoryPermissions>(static_cast<std::uint8_t>(left) |
                                               static_cast<std::uint8_t>(right));
}

[[nodiscard]] constexpr GuestMemoryPermissions operator&(GuestMemoryPermissions left,
                                                         GuestMemoryPermissions right) noexcept
{
    return static_cast<GuestMemoryPermissions>(static_cast<std::uint8_t>(left) &
                                               static_cast<std::uint8_t>(right));
}

constexpr GuestMemoryPermissions& operator|=(GuestMemoryPermissions& left,
                                             GuestMemoryPermissions right) noexcept
{
    left = left | right;
    return left;
}

[[nodiscard]] constexpr bool has_permission(GuestMemoryPermissions actual,
                                            GuestMemoryPermissions required) noexcept
{
    return (actual & required) == required;
}

[[nodiscard]] constexpr bool is_valid_permissions(GuestMemoryPermissions permissions) noexcept
{
    constexpr auto known = GuestMemoryPermissions::Read | GuestMemoryPermissions::Write |
                           GuestMemoryPermissions::Execute;
    return (static_cast<std::uint8_t>(permissions) & ~static_cast<std::uint8_t>(known)) == 0U;
}

enum class GuestRegionKind
{
    Other,
    Text,
    Rodata,
    Data,
    Bss,
};

[[nodiscard]] std::string_view guest_region_kind_name(GuestRegionKind kind) noexcept;

struct GuestMemoryRegionInfo
{
    GuestAddress base;
    GuestSize size;
    GuestMemoryPermissions permissions;
    GuestRegionKind kind;
    std::string name;

    [[nodiscard]] GuestAddress end() const noexcept
    {
        return base + size;
    }
};

inline constexpr GuestSize guest_default_max_region_size =
    GuestSize{256U} * GuestSize{1024U} * GuestSize{1024U};
inline constexpr GuestSize guest_default_max_total_size =
    GuestSize{512U} * GuestSize{1024U} * GuestSize{1024U};
inline constexpr std::size_t guest_default_max_regions = 1024U;

struct GuestMemoryLimits
{
    GuestSize max_region_size = guest_default_max_region_size;
    GuestSize max_total_size = guest_default_max_total_size;
    std::size_t max_regions = guest_default_max_regions;
};

// A mapping token is an exact capability for one dynamically owned mapping.
// It intentionally exposes no address or name that a caller could use to
// request an imprecise unmap. GuestMemory authenticates both the memory domain
// and the per-mapping identity before releasing a region.
struct GuestMemoryMappingToken
{
    GuestMemoryMappingToken() = default;

  private:
    std::shared_ptr<const void> memory_domain_;
    std::shared_ptr<const void> mapping_identity_;
    GuestAddress base_ = 0U;
    GuestSize size_ = 0U;

    GuestMemoryMappingToken(std::shared_ptr<const void> memory_domain,
                            std::shared_ptr<const void> mapping_identity,
                            GuestAddress base, GuestSize size)
        : memory_domain_(std::move(memory_domain)),
          mapping_identity_(std::move(mapping_identity)), base_(base), size_(size)
    {
    }

    friend class GuestMemory;
};

struct GuestMemoryAccounting
{
    GuestSize max_region_size = 0U;
    GuestSize max_total_size = 0U;
    std::size_t max_regions = 0U;
    GuestSize live_mapped_bytes = 0U;
    GuestSize peak_live_mapped_bytes = 0U;
    GuestSize cumulative_mapped_bytes = 0U;
    std::size_t live_region_count = 0U;
    std::size_t peak_region_count = 0U;
    std::size_t owned_mappings_created = 0U;
    std::size_t owned_mappings_reclaimed = 0U;
    std::size_t live_owned_mappings = 0U;
    std::size_t peak_live_owned_mappings = 0U;
    GuestSize live_owned_bytes = 0U;
    GuestSize peak_live_owned_bytes = 0U;
    GuestSize cumulative_owned_bytes = 0U;
    GuestAddress virtual_address_high_water = 0U;
};

class GuestMemory
{
  public:
    explicit GuestMemory(GuestMemoryLimits limits = {});

    GuestMemory(const GuestMemory& other);
    GuestMemory(GuestMemory&&) noexcept = default;
    GuestMemory& operator=(const GuestMemory& other);
    GuestMemory& operator=(GuestMemory&&) noexcept = default;
    ~GuestMemory() = default;

    [[nodiscard]] Result<void> map(GuestAddress base, GuestSize size,
                                   GuestMemoryPermissions permissions, std::string_view name = {},
                                   GuestRegionKind kind = GuestRegionKind::Other);

    [[nodiscard]] Result<void> map(GuestAddress base, std::span<const std::byte> initial_data,
                                   GuestMemoryPermissions permissions, std::string_view name = {},
                                   GuestRegionKind kind = GuestRegionKind::Other);

    [[nodiscard]] Result<GuestMemoryMappingToken> map_owned(
        GuestAddress base, GuestSize size, GuestMemoryPermissions permissions,
        std::string_view name = {}, GuestRegionKind kind = GuestRegionKind::Other);

    [[nodiscard]] Result<GuestMemoryMappingToken> map_owned(
        GuestAddress base, std::span<const std::byte> initial_data,
        GuestMemoryPermissions permissions, std::string_view name = {},
        GuestRegionKind kind = GuestRegionKind::Other);

    // Release exactly the whole mapping represented by token. A token from a
    // different GuestMemory, a stale token, a token for a different mapping,
    // or a token for a static map fails without mutating this object.
    [[nodiscard]] Result<void> release_owned(const GuestMemoryMappingToken& token);

    [[nodiscard]] Result<void> read(GuestAddress address, std::span<std::byte> destination) const;

    [[nodiscard]] Result<void> write(GuestAddress address, std::span<const std::byte> source);

    // Loader-time relocation writes deliberately bypass the final region write
    // permission, but retain all address, overflow, mapping, and containment
    // checks. Normal guest code must continue to use write().
    [[nodiscard]] Result<void> validate_loader_write(GuestAddress address,
                                                     GuestSize size) const;
    [[nodiscard]] Result<void> loader_write(GuestAddress address,
                                            std::span<const std::byte> source);

    [[nodiscard]] Result<GuestMemoryPermissions> permissions_at(GuestAddress address,
                                                                GuestSize size = 1U) const;

    [[nodiscard]] Result<bool> is_executable(GuestAddress address, GuestSize size = 1U) const;

    [[nodiscard]] Result<GuestMemoryRegionInfo> region_at(GuestAddress address) const;

    [[nodiscard]] std::vector<GuestMemoryRegionInfo> regions() const;

    [[nodiscard]] std::size_t region_count() const noexcept;
    [[nodiscard]] GuestSize total_mapped_size() const noexcept;
    [[nodiscard]] GuestMemoryAccounting accounting() const noexcept;
    [[nodiscard]] const GuestMemoryLimits& limits() const noexcept;

  private:
    struct OwnershipDomain
    {
    };

    struct OwnedMappingIdentity
    {
    };

    struct GuestRegion
    {
        GuestMemoryRegionInfo info;
        std::vector<std::byte> bytes;
        std::shared_ptr<const void> owned_mapping;
    };

    [[nodiscard]] Result<void> map_bytes(GuestAddress base, std::span<const std::byte> initial_data,
                                         GuestSize size, GuestMemoryPermissions permissions,
                                         std::string_view name, GuestRegionKind kind,
                                         std::shared_ptr<const void> owned_mapping = {});

    [[nodiscard]] const GuestRegion* find_region(GuestAddress address) const noexcept;
    [[nodiscard]] GuestRegion* find_region(GuestAddress address) noexcept;

    [[nodiscard]] Result<const GuestRegion*> validate_read_range(GuestAddress address,
                                                                 GuestSize size) const;
    [[nodiscard]] Result<const GuestRegion*> validate_range(GuestAddress address, GuestSize size,
                                                            std::string_view operation) const;
    [[nodiscard]] Result<GuestRegion*> validate_write_range(GuestAddress address, GuestSize size);

    GuestMemoryLimits limits_;
    std::shared_ptr<const OwnershipDomain> ownership_domain_;
    GuestSize total_mapped_size_ = 0U;
    GuestSize peak_live_mapped_size_ = 0U;
    GuestSize cumulative_mapped_size_ = 0U;
    std::size_t peak_region_count_ = 0U;
    std::size_t owned_mappings_created_ = 0U;
    std::size_t owned_mappings_reclaimed_ = 0U;
    std::size_t live_owned_mappings_ = 0U;
    std::size_t peak_live_owned_mappings_ = 0U;
    GuestSize live_owned_size_ = 0U;
    GuestSize peak_live_owned_size_ = 0U;
    GuestSize cumulative_owned_size_ = 0U;
    GuestAddress virtual_address_high_water_ = 0U;
    std::vector<GuestRegion> regions_;
};

} // namespace switchrecomp::memory
