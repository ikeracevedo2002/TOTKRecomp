#pragma once

#include "switchrecomp/common/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
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

class GuestMemory
{
  public:
    explicit GuestMemory(GuestMemoryLimits limits = {});

    GuestMemory(const GuestMemory&) = default;
    GuestMemory(GuestMemory&&) noexcept = default;
    GuestMemory& operator=(const GuestMemory&) = default;
    GuestMemory& operator=(GuestMemory&&) noexcept = default;
    ~GuestMemory() = default;

    [[nodiscard]] Result<void> map(GuestAddress base, GuestSize size,
                                   GuestMemoryPermissions permissions, std::string_view name = {},
                                   GuestRegionKind kind = GuestRegionKind::Other);

    [[nodiscard]] Result<void> map(GuestAddress base, std::span<const std::byte> initial_data,
                                   GuestMemoryPermissions permissions, std::string_view name = {},
                                   GuestRegionKind kind = GuestRegionKind::Other);

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
    [[nodiscard]] const GuestMemoryLimits& limits() const noexcept;

  private:
    struct GuestRegion
    {
        GuestMemoryRegionInfo info;
        std::vector<std::byte> bytes;
    };

    [[nodiscard]] Result<void> map_bytes(GuestAddress base, std::span<const std::byte> initial_data,
                                         GuestSize size, GuestMemoryPermissions permissions,
                                         std::string_view name, GuestRegionKind kind);

    [[nodiscard]] const GuestRegion* find_region(GuestAddress address) const noexcept;
    [[nodiscard]] GuestRegion* find_region(GuestAddress address) noexcept;

    [[nodiscard]] Result<const GuestRegion*> validate_read_range(GuestAddress address,
                                                                 GuestSize size) const;
    [[nodiscard]] Result<const GuestRegion*> validate_range(GuestAddress address, GuestSize size,
                                                            std::string_view operation) const;
    [[nodiscard]] Result<GuestRegion*> validate_write_range(GuestAddress address, GuestSize size);

    GuestMemoryLimits limits_;
    GuestSize total_mapped_size_ = 0U;
    std::vector<GuestRegion> regions_;
};

} // namespace switchrecomp::memory
