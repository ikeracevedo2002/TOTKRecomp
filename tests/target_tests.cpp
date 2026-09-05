#include "switchrecomp/target/file_validation.hpp"
#include "switchrecomp/target/manifest.hpp"

#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace
{

constexpr char kTemplateManifest[] = R"json({
  "schema_version": 1,
  "game": "totk",
  "support_status": "template",
  "version": "TBD",
  "region": "TBD",
  "title_update": "TBD",
  "modules": [{"name": "main", "build_id": "TBD", "sha256": "TBD"}]
})json";

constexpr char kRealLikeManifest[] = R"json({
  "schema_version": 1,
  "game": "totk",
  "support_status": "experimental",
  "version": "1.0.0",
  "region": "global",
  "title_update": "1.0",
  "modules": [{
    "name": "main",
    "build_id": "0123456789abcdef",
    "sha256": "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
    "expected_size": 3
  }]
})json";

constexpr char kDuplicateManifest[] = R"json({
  "schema_version": 1,
  "game": "totk",
  "support_status": "experimental",
  "version": "1.0.0",
  "region": "global",
  "title_update": "1.0",
  "modules": [
    {"name": "main", "build_id": "0123456789abcdef", "sha256": "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
    {"name": "main", "build_id": "fedcba9876543210", "sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}
  ]
})json";

} // namespace

TEST_CASE("template manifest accepts explicit placeholders")
{
    const auto manifest = switchrecomp::target::parse_manifest(kTemplateManifest);
    REQUIRE(manifest);
    REQUIRE(manifest.value().support_status == switchrecomp::target::SupportStatus::Template);
    REQUIRE(manifest.value().modules.size() == 1);
}

TEST_CASE("real-like manifest validates and rejects duplicate modules")
{
    const auto manifest = switchrecomp::target::parse_manifest(kRealLikeManifest);
    REQUIRE(manifest);

    const auto duplicate_result = switchrecomp::target::parse_manifest(kDuplicateManifest);
    REQUIRE_FALSE(duplicate_result);
    REQUIRE(duplicate_result.error().message.find("duplicate module name") != std::string::npos);
}

TEST_CASE("manifest validation rejects malformed hashes and future schema")
{
    auto invalid_hash = std::string(kRealLikeManifest);
    const auto hash_position = invalid_hash.find("ba7816");
    invalid_hash.replace(hash_position, 6, "zzzzzz");
    const auto hash_result = switchrecomp::target::parse_manifest(invalid_hash);
    REQUIRE_FALSE(hash_result);
    REQUIRE(hash_result.error().message.find("sha256") != std::string::npos);

    auto future_schema = std::string(kRealLikeManifest);
    future_schema.replace(future_schema.find("\"schema_version\": 1"), 19, "\"schema_version\": 3");
    const auto schema_result = switchrecomp::target::parse_manifest(future_schema);
    REQUIRE_FALSE(schema_result);
    REQUIRE(schema_result.error().code == switchrecomp::ErrorCode::Unsupported);
}

TEST_CASE("supported manifest cannot contain placeholders")
{
    auto supported = std::string(kRealLikeManifest);
    supported.replace(supported.find("experimental"), 12, "supported");
    supported.replace(supported.find("1.0.0"), 5, "TBD");
    const auto result = switchrecomp::target::parse_manifest(supported);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == switchrecomp::ErrorCode::PlaceholderManifest);
}

TEST_CASE("synthetic file matching reports match, mismatch, and missing file")
{
    const auto manifest_result = switchrecomp::target::parse_manifest(kRealLikeManifest);
    REQUIRE(manifest_result);
    const auto path = std::filesystem::temp_directory_path() / "totkrecomp-module-test.bin";
    {
        std::ofstream output(path, std::ios::binary);
        output << "abc";
    }

    const auto match = switchrecomp::target::validate_module_file(path, manifest_result.value(), "main");
    REQUIRE(match);
    REQUIRE(match.value().status == switchrecomp::target::FileValidationStatus::Match);

    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "xyz";
    }
    const auto mismatch = switchrecomp::target::validate_module_file(path, manifest_result.value(), "main");
    REQUIRE(mismatch);
    REQUIRE(mismatch.value().status == switchrecomp::target::FileValidationStatus::HashMismatch);

    std::filesystem::remove(path);
    const auto missing = switchrecomp::target::validate_module_file(path, manifest_result.value(), "main");
    REQUIRE(missing);
    REQUIRE(missing.value().status == switchrecomp::target::FileValidationStatus::MissingFile);
}

TEST_CASE("template file matching never claims support")
{
    const auto manifest_result = switchrecomp::target::parse_manifest(kTemplateManifest);
    REQUIRE(manifest_result);
    const auto result = switchrecomp::target::validate_module_file(
        "does-not-matter", manifest_result.value(), "main");
    REQUIRE(result);
    REQUIRE(result.value().status == switchrecomp::target::FileValidationStatus::PlaceholderManifest);
}
