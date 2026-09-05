#include "switchrecomp/common/binary_reader.hpp"
#include "switchrecomp/common/checked_arithmetic.hpp"
#include "switchrecomp/common/logging.hpp"
#include "switchrecomp/common/sha256.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using switchrecomp::BinaryReader;

TEST_CASE("checked arithmetic rejects overflow and underflow")
{
    REQUIRE(switchrecomp::checked_add(4, 5).value() == 9);
    REQUIRE_FALSE(switchrecomp::checked_add(std::numeric_limits<std::size_t>::max(), 1));
    REQUIRE(switchrecomp::checked_sub(9, 4).value() == 5);
    REQUIRE_FALSE(switchrecomp::checked_sub(4, 9));
}

TEST_CASE("checked ranges accept zero and exact-end ranges")
{
    REQUIRE(switchrecomp::checked_range(0, 0, 8));
    REQUIRE(switchrecomp::checked_range(2, 6, 8));
    REQUIRE_FALSE(switchrecomp::checked_range(3, 6, 8));
    REQUIRE_FALSE(switchrecomp::checked_range(std::numeric_limits<std::size_t>::max(), 1, 8));
    REQUIRE(switchrecomp::contains_range(2, 6, 2, 6));
    REQUIRE_FALSE(switchrecomp::contains_range(2, 6, 1, 1));
}

TEST_CASE("binary reader reads little-endian values and checks bounds")
{
    constexpr std::array<std::byte, 16> bytes{
        std::byte{0x34}, std::byte{0x12}, std::byte{0x78}, std::byte{0x56},
        std::byte{0x34}, std::byte{0x12}, std::byte{0xef}, std::byte{0xcd},
        std::byte{0xab}, std::byte{0x89}, std::byte{0x67}, std::byte{0x45},
        std::byte{0x23}, std::byte{0x01}, std::byte{0x00}, std::byte{0xff}};
    const BinaryReader reader(bytes);

    REQUIRE(reader.read_u16_le(0).value() == 0x1234);
    REQUIRE(reader.read_u32_le(2).value() == 0x12345678);
    REQUIRE(reader.read_u64_le(6).value() == 0x0123456789abcdefULL);
    REQUIRE(reader.slice(16, 0));
    REQUIRE(reader.slice(15, 2).error().code == switchrecomp::ErrorCode::OutOfBounds);
    REQUIRE(reader.read_u32_le(13).error().code == switchrecomp::ErrorCode::OutOfBounds);
}

TEST_CASE("SHA-256 matches public test vectors")
{
    const std::string empty;
    const std::string abc = "abc";
    const auto empty_digest = switchrecomp::sha256_bytes(std::as_bytes(std::span(empty)));
    const auto abc_digest = switchrecomp::sha256_bytes(std::as_bytes(std::span(abc)));
    REQUIRE(empty_digest);
    REQUIRE(abc_digest);
    REQUIRE(switchrecomp::sha256_to_hex(empty_digest.value()) ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    REQUIRE(switchrecomp::sha256_to_hex(abc_digest.value()) ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    REQUIRE(switchrecomp::parse_sha256_hex(
                "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD")
                .value() == abc_digest.value());
    REQUIRE_FALSE(switchrecomp::parse_sha256_hex("abc"));
}

TEST_CASE("file SHA-256 streams a synthetic file")
{
    const auto path = std::filesystem::temp_directory_path() / "totkrecomp-sha256-test.txt";
    {
        std::ofstream output(path, std::ios::binary);
        output << "abc";
    }
    const auto digest = switchrecomp::sha256_file(path);
    REQUIRE(digest);
    REQUIRE(switchrecomp::sha256_to_hex(digest.value()) ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::filesystem::remove(path);
}

TEST_CASE("logging exposes structured records")
{
    std::vector<switchrecomp::logging::LogRecord> records;
    switchrecomp::logging::set_sink([&records](const switchrecomp::logging::LogRecord& record) {
        records.push_back(record);
    });
    switchrecomp::logging::set_minimum_level(switchrecomp::logging::LogLevel::Info);
    switchrecomp::logging::log_info(switchrecomp::logging::LogCategory::Format, "test message");
    switchrecomp::logging::log_debug(switchrecomp::logging::LogCategory::Format, "filtered");
    switchrecomp::logging::reset_sink();
    REQUIRE(records.size() == 1);
    REQUIRE(records.front().message == "test message");
    REQUIRE(records.front().category == switchrecomp::logging::LogCategory::Format);
}
