#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/target/manifest.hpp"

#include <filesystem>
#include <string_view>

namespace switchrecomp::target
{

enum class FileValidationStatus
{
    Match,
    MissingFile,
    SizeMismatch,
    HashMismatch,
    PlaceholderManifest,
    UnsupportedManifest,
    InvalidManifest,
};

[[nodiscard]] std::string_view file_validation_status_name(FileValidationStatus status) noexcept;

struct FileValidationResult
{
    FileValidationStatus status;
    std::string message;
};

[[nodiscard]] Result<FileValidationResult> validate_module_file(
    const std::filesystem::path& path,
    const TargetManifest& manifest,
    std::string_view module_name);

} // namespace switchrecomp::target
