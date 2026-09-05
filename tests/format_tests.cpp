#include "switchrecomp/format/nso_magic.hpp"

#include <array>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("synthetic NSO magic is recognized without parsing the format")
{
    constexpr std::array<std::byte, 4> magic{
        std::byte{'N'}, std::byte{'S'}, std::byte{'O'}, std::byte{'0'}};
    const auto result = switchrecomp::format::inspect_nso_magic(magic);
    REQUIRE(result);
    REQUIRE(result.value() == switchrecomp::format::NsoMagicStatus::Valid);
}

TEST_CASE("NSO magic check reports short and unexpected input")
{
    constexpr std::array<std::byte, 3> short_input{
        std::byte{'N'}, std::byte{'S'}, std::byte{'O'}};
    constexpr std::array<std::byte, 4> other_magic{
        std::byte{'E'}, std::byte{'L'}, std::byte{'F'}, std::byte{0}};
    REQUIRE(switchrecomp::format::inspect_nso_magic(short_input).value() ==
            switchrecomp::format::NsoMagicStatus::TooShort);
    REQUIRE(switchrecomp::format::inspect_nso_magic(other_magic).value() ==
            switchrecomp::format::NsoMagicStatus::Unexpected);
}
