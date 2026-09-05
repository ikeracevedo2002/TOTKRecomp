#pragma once

#include "switchrecomp/common/result.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace switchrecomp::target
{

enum class SupportStatus
{
    Template,
    Experimental,
    Supported,
    Unsupported,
};

[[nodiscard]] std::string_view support_status_name(SupportStatus status) noexcept;
[[nodiscard]] Result<SupportStatus> parse_support_status(std::string_view value);

struct ManifestModule
{
    std::string name;
    std::string build_id;
    std::string sha256;
    std::optional<std::uint64_t> expected_size;
    std::optional<std::uint64_t> expected_decompressed_size;
    std::optional<std::string> notes;
};

struct TargetManifest
{
    std::uint32_t schema_version = 0;
    std::string game;
    SupportStatus support_status = SupportStatus::Unsupported;
    std::string version;
    std::string region;
    std::string title_update;
    std::optional<std::string> minimum_tool_version;
    std::optional<std::string> notes;
    std::vector<ManifestModule> modules;
};

[[nodiscard]] Result<TargetManifest> parse_manifest(std::string_view json_text);
[[nodiscard]] Result<TargetManifest> read_manifest(const std::filesystem::path& path);
[[nodiscard]] Result<void> validate_manifest(const TargetManifest& manifest);

} // namespace switchrecomp::target
