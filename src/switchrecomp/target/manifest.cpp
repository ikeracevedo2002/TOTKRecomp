#include "switchrecomp/target/manifest.hpp"

#include "switchrecomp/common/sha256.hpp"

#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

namespace switchrecomp::target
{
namespace
{

using Json = nlohmann::json;

[[nodiscard]] bool is_tbd(std::string_view value) noexcept
{
    return value == "TBD";
}

[[nodiscard]] bool is_hex_string(std::string_view value) noexcept
{
    if (value.empty() || value.size() % 2 != 0)
    {
        return false;
    }
    for (const char character : value)
    {
        const bool digit = character >= '0' && character <= '9';
        const bool lower = character >= 'a' && character <= 'f';
        const bool upper = character >= 'A' && character <= 'F';
        if (!digit && !lower && !upper)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Result<void> reject_unknown_keys(
    const Json& object,
    std::initializer_list<std::string_view> allowed,
    std::string_view context)
{
    for (const auto& [key, value] : object.items())
    {
        (void)value;
        bool known = false;
        for (const auto allowed_key : allowed)
        {
            if (key == allowed_key)
            {
                known = true;
                break;
            }
        }
        if (!known)
        {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidManifest,
                std::string(context) + ": unknown field '" + key + "'"));
        }
    }
    return Result<void>::success();
}

[[nodiscard]] Result<std::string> required_string(
    const Json& object,
    std::string_view key,
    std::string_view context)
{
    if (!object.contains(key))
    {
        return Result<std::string>::failure(make_error(
            ErrorCode::InvalidManifest,
            std::string(context) + "." + std::string(key) + ": required field is missing"));
    }
    if (!object.at(key).is_string())
    {
        return Result<std::string>::failure(make_error(
            ErrorCode::InvalidManifest,
            std::string(context) + "." + std::string(key) + ": expected a string"));
    }
    return Result<std::string>::success(object.at(key).get<std::string>());
}

[[nodiscard]] Result<std::optional<std::string>> optional_string(
    const Json& object,
    std::string_view key,
    std::string_view context)
{
    if (!object.contains(key))
    {
        return Result<std::optional<std::string>>::success(std::nullopt);
    }
    if (!object.at(key).is_string())
    {
        return Result<std::optional<std::string>>::failure(make_error(
            ErrorCode::InvalidManifest,
            std::string(context) + "." + std::string(key) + ": expected a string"));
    }
    return Result<std::optional<std::string>>::success(object.at(key).get<std::string>());
}

[[nodiscard]] Result<std::optional<std::uint64_t>> optional_uint64(
    const Json& object,
    std::string_view key,
    std::string_view context)
{
    if (!object.contains(key))
    {
        return Result<std::optional<std::uint64_t>>::success(std::nullopt);
    }
    if (!object.at(key).is_number_unsigned())
    {
        return Result<std::optional<std::uint64_t>>::failure(make_error(
            ErrorCode::InvalidManifest,
            std::string(context) + "." + std::string(key) + ": expected a non-negative integer"));
    }
    return Result<std::optional<std::uint64_t>>::success(object.at(key).get<std::uint64_t>());
}

} // namespace

std::string_view support_status_name(SupportStatus status) noexcept
{
    switch (status)
    {
    case SupportStatus::Template:
        return "template";
    case SupportStatus::Experimental:
        return "experimental";
    case SupportStatus::Supported:
        return "supported";
    case SupportStatus::Unsupported:
        return "unsupported";
    }
    return "unknown";
}

Result<SupportStatus> parse_support_status(std::string_view value)
{
    if (value == "template")
    {
        return Result<SupportStatus>::success(SupportStatus::Template);
    }
    if (value == "experimental")
    {
        return Result<SupportStatus>::success(SupportStatus::Experimental);
    }
    if (value == "supported")
    {
        return Result<SupportStatus>::success(SupportStatus::Supported);
    }
    if (value == "unsupported")
    {
        return Result<SupportStatus>::success(SupportStatus::Unsupported);
    }
    return Result<SupportStatus>::failure(make_error(
        ErrorCode::InvalidManifest,
        "support_status: expected template, experimental, supported, or unsupported"));
}

Result<void> validate_manifest(const TargetManifest& manifest)
{
    if (manifest.schema_version != 1)
    {
        return Result<void>::failure(make_error(
            ErrorCode::Unsupported,
            "target manifest schema version " + std::to_string(manifest.schema_version) +
                " is unsupported; supported version: 1"));
    }
    if (manifest.game != "totk")
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidManifest,
            "game: expected 'totk'"));
    }
    if (manifest.version.empty() || manifest.region.empty() || manifest.title_update.empty())
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidManifest,
            "version, region, and title_update must be non-empty"));
    }
    if (manifest.modules.empty())
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidManifest,
            "modules: at least one module is required"));
    }

    std::set<std::string> module_names;
    for (std::size_t index = 0; index < manifest.modules.size(); ++index)
    {
        const auto& module = manifest.modules[index];
        const auto prefix = "modules[" + std::to_string(index) + "]";
        if (module.name.empty())
        {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidManifest,
                prefix + ".name: must not be empty"));
        }
        if (!module_names.insert(module.name).second)
        {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidManifest,
                prefix + ".name: duplicate module name '" + module.name + "'"));
        }

        const bool has_placeholder = is_tbd(manifest.version) || is_tbd(manifest.region) ||
                                     is_tbd(manifest.title_update) || is_tbd(module.build_id) ||
                                     is_tbd(module.sha256);
        if (has_placeholder && manifest.support_status != SupportStatus::Template)
        {
            return Result<void>::failure(make_error(
                ErrorCode::PlaceholderManifest,
                prefix + ": TBD placeholders are allowed only when support_status is 'template'"));
        }

        if (!is_tbd(module.build_id) && !is_hex_string(module.build_id))
        {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidManifest,
                prefix + ".build_id: expected a non-empty even-length hexadecimal string"));
        }
        if (!is_tbd(module.sha256))
        {
            if (module.sha256.size() != 64 || !is_hex_string(module.sha256))
            {
                return Result<void>::failure(make_error(
                    ErrorCode::InvalidManifest,
                    prefix + ".sha256: expected exactly 64 hexadecimal characters"));
            }
        }
    }
    return Result<void>::success();
}

Result<TargetManifest> parse_manifest(std::string_view json_text)
{
    Json root;
    try
    {
        root = Json::parse(json_text);
    }
    catch (const Json::parse_error& error)
    {
        return Result<TargetManifest>::failure(make_error(
            ErrorCode::InvalidManifest,
            std::string("manifest JSON syntax error: ") + error.what()));
    }

    if (!root.is_object())
    {
        return Result<TargetManifest>::failure(make_error(
            ErrorCode::InvalidManifest,
            "manifest root: expected a JSON object"));
    }
    const auto known = reject_unknown_keys(
        root,
        {"schema_version", "game", "support_status", "version", "region", "title_update",
         "minimum_tool_version", "notes", "modules"},
        "manifest");
    if (!known)
    {
        return Result<TargetManifest>::failure(known.error());
    }

    TargetManifest manifest;
    if (!root.contains("schema_version") || !root.at("schema_version").is_number_unsigned())
    {
        return Result<TargetManifest>::failure(make_error(
            ErrorCode::InvalidManifest,
            "schema_version: required non-negative integer is missing"));
    }
    const auto schema_version = root.at("schema_version").get<std::uint64_t>();
    if (schema_version > std::numeric_limits<std::uint32_t>::max())
    {
        return Result<TargetManifest>::failure(make_error(
            ErrorCode::Unsupported,
            "target manifest schema version is outside the supported integer range"));
    }
    manifest.schema_version = static_cast<std::uint32_t>(schema_version);

    const auto game = required_string(root, "game", "manifest");
    const auto status_text = required_string(root, "support_status", "manifest");
    const auto version = required_string(root, "version", "manifest");
    const auto region = required_string(root, "region", "manifest");
    const auto title_update = required_string(root, "title_update", "manifest");
    if (!game || !status_text || !version || !region || !title_update)
    {
        const Error& error = !game ? game.error()
                             : !status_text ? status_text.error()
                             : !version ? version.error()
                             : !region ? region.error()
                                       : title_update.error();
        return Result<TargetManifest>::failure(error);
    }
    manifest.game = game.value();
    const auto status = parse_support_status(status_text.value());
    if (!status)
    {
        return Result<TargetManifest>::failure(status.error());
    }
    manifest.support_status = status.value();
    manifest.version = version.value();
    manifest.region = region.value();
    manifest.title_update = title_update.value();

    const auto minimum_tool_version = optional_string(root, "minimum_tool_version", "manifest");
    const auto notes = optional_string(root, "notes", "manifest");
    if (!minimum_tool_version || !notes)
    {
        return Result<TargetManifest>::failure(!minimum_tool_version ? minimum_tool_version.error() : notes.error());
    }
    manifest.minimum_tool_version = minimum_tool_version.value();
    manifest.notes = notes.value();

    if (!root.contains("modules") || !root.at("modules").is_array())
    {
        return Result<TargetManifest>::failure(make_error(
            ErrorCode::InvalidManifest,
            "modules: required array is missing"));
    }
    for (std::size_t index = 0; index < root.at("modules").size(); ++index)
    {
        const auto& json_module = root.at("modules").at(index);
        const auto prefix = "modules[" + std::to_string(index) + "]";
        if (!json_module.is_object())
        {
            return Result<TargetManifest>::failure(make_error(
                ErrorCode::InvalidManifest,
                prefix + ": expected an object"));
        }
        const auto module_keys = reject_unknown_keys(
            json_module,
            {"name", "build_id", "sha256", "expected_size", "expected_decompressed_size", "notes"},
            prefix);
        if (!module_keys)
        {
            return Result<TargetManifest>::failure(module_keys.error());
        }
        const auto name = required_string(json_module, "name", prefix);
        const auto build_id = required_string(json_module, "build_id", prefix);
        const auto sha256 = required_string(json_module, "sha256", prefix);
        const auto expected_size = optional_uint64(json_module, "expected_size", prefix);
        const auto expected_decompressed_size = optional_uint64(
            json_module, "expected_decompressed_size", prefix);
        const auto module_notes = optional_string(json_module, "notes", prefix);
        if (!name || !build_id || !sha256 || !expected_size || !expected_decompressed_size || !module_notes)
        {
            const Error& error = !name ? name.error()
                                 : !build_id ? build_id.error()
                                 : !sha256 ? sha256.error()
                                 : !expected_size ? expected_size.error()
                                 : !expected_decompressed_size ? expected_decompressed_size.error()
                                                               : module_notes.error();
            return Result<TargetManifest>::failure(error);
        }
        manifest.modules.push_back(ManifestModule{
            name.value(),
            build_id.value(),
            sha256.value(),
            expected_size.value(),
            expected_decompressed_size.value(),
            module_notes.value()});
    }

    const auto valid = validate_manifest(manifest);
    if (!valid)
    {
        return Result<TargetManifest>::failure(valid.error());
    }
    return Result<TargetManifest>::success(std::move(manifest));
}

Result<TargetManifest> read_manifest(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return Result<TargetManifest>::failure(make_error(
            ErrorCode::MissingFile,
            "could not open target manifest: " + path.string()));
    }
    std::ostringstream content;
    content << input.rdbuf();
    if (input.bad())
    {
        return Result<TargetManifest>::failure(make_error(
            ErrorCode::IoError,
            "could not read target manifest: " + path.string()));
    }
    return parse_manifest(content.str());
}

} // namespace switchrecomp::target
