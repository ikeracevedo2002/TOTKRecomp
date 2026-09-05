#pragma once

#include "switchrecomp/common/result.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace switchrecomp
{

struct Sha256Digest
{
    std::array<std::byte, 32> bytes{};

    friend bool operator==(const Sha256Digest&, const Sha256Digest&) = default;
};

[[nodiscard]] Result<Sha256Digest> sha256_bytes(std::span<const std::byte> bytes);
[[nodiscard]] Result<Sha256Digest> sha256_file(const std::filesystem::path& path);
[[nodiscard]] Result<Sha256Digest> parse_sha256_hex(std::string_view text);
[[nodiscard]] std::string sha256_to_hex(const Sha256Digest& digest);

} // namespace switchrecomp
