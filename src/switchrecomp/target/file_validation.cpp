#include "switchrecomp/target/file_validation.hpp"

#include "switchrecomp/common/sha256.hpp"

#include <algorithm>
#include <system_error>

namespace switchrecomp::target
{

std::string_view file_validation_status_name(FileValidationStatus status) noexcept
{
    switch (status)
    {
    case FileValidationStatus::Match:
        return "match";
    case FileValidationStatus::MissingFile:
        return "missing_file";
    case FileValidationStatus::SizeMismatch:
        return "size_mismatch";
    case FileValidationStatus::HashMismatch:
        return "hash_mismatch";
    case FileValidationStatus::PlaceholderManifest:
        return "placeholder_manifest";
    case FileValidationStatus::UnsupportedManifest:
        return "unsupported_manifest";
    case FileValidationStatus::InvalidManifest:
        return "invalid_manifest";
    }
    return "unknown";
}

Result<FileValidationResult> validate_module_file(
    const std::filesystem::path& path,
    const TargetManifest& manifest,
    std::string_view module_name)
{
    const auto valid_manifest = validate_manifest(manifest);
    if (!valid_manifest)
    {
        const auto status = valid_manifest.error().code == ErrorCode::PlaceholderManifest
                                ? FileValidationStatus::PlaceholderManifest
                                : FileValidationStatus::InvalidManifest;
        return Result<FileValidationResult>::success(FileValidationResult{status, valid_manifest.error().message});
    }
    if (manifest.support_status == SupportStatus::Template)
    {
        return Result<FileValidationResult>::success(FileValidationResult{
            FileValidationStatus::PlaceholderManifest,
            "target manifest is a template and cannot validate a supported input"});
    }
    if (manifest.support_status == SupportStatus::Unsupported)
    {
        return Result<FileValidationResult>::success(FileValidationResult{
            FileValidationStatus::UnsupportedManifest,
            "target manifest explicitly marks this target as unsupported"});
    }

    const auto module = std::find_if(
        manifest.modules.begin(),
        manifest.modules.end(),
        [module_name](const ManifestModule& candidate) { return candidate.name == module_name; });
    if (module == manifest.modules.end())
    {
        return Result<FileValidationResult>::success(FileValidationResult{
            FileValidationStatus::InvalidManifest,
            "manifest does not contain module '" + std::string(module_name) + "'"});
    }

    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(path, filesystem_error))
    {
        return Result<FileValidationResult>::success(FileValidationResult{
            FileValidationStatus::MissingFile,
            "module file does not exist or is not a regular file: " + path.string()});
    }
    if (module->expected_size.has_value())
    {
        const auto size = std::filesystem::file_size(path, filesystem_error);
        if (filesystem_error)
        {
            return Result<FileValidationResult>::success(FileValidationResult{
                FileValidationStatus::MissingFile,
                "could not read module file size: " + path.string()});
        }
        if (size != module->expected_size.value())
        {
            return Result<FileValidationResult>::success(FileValidationResult{
                FileValidationStatus::SizeMismatch,
                "module '" + std::string(module_name) + "' has unexpected file size"});
        }
    }

    const auto expected = parse_sha256_hex(module->sha256);
    if (!expected)
    {
        return Result<FileValidationResult>::success(FileValidationResult{
            FileValidationStatus::InvalidManifest,
            expected.error().message});
    }
    const auto actual = sha256_file(path);
    if (!actual)
    {
        const auto status = actual.error().code == ErrorCode::MissingFile
                                ? FileValidationStatus::MissingFile
                                : FileValidationStatus::InvalidManifest;
        return Result<FileValidationResult>::success(FileValidationResult{status, actual.error().message});
    }
    if (actual.value() != expected.value())
    {
        return Result<FileValidationResult>::success(FileValidationResult{
            FileValidationStatus::HashMismatch,
            "module '" + std::string(module_name) + "' SHA-256 does not match the manifest"});
    }
    return Result<FileValidationResult>::success(FileValidationResult{
        FileValidationStatus::Match,
        "module '" + std::string(module_name) + "' matches the manifest"});
}

} // namespace switchrecomp::target
