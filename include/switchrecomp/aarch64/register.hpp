#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace switchrecomp::aarch64
{

enum class RegisterKind : std::uint8_t
{
    Invalid,
    General,
    Vector,
    Predicate,
    System,
};

enum class RegisterWidth : std::uint8_t
{
    None,
    W32,
    X64,
    B8,
    H16,
    S32,
    D64,
    Q128,
};

struct Register
{
    RegisterKind kind = RegisterKind::Invalid;
    RegisterWidth width = RegisterWidth::None;
    std::uint8_t index = 0U;
    bool is_stack_pointer = false;
    bool is_zero = false;

    [[nodiscard]] bool valid() const noexcept
    {
        return kind != RegisterKind::Invalid;
    }

    friend bool operator==(const Register&, const Register&) = default;
};

[[nodiscard]] std::string_view register_kind_name(RegisterKind kind) noexcept;
[[nodiscard]] std::string_view register_width_name(RegisterWidth width) noexcept;
[[nodiscard]] std::string register_name(const Register& reg);

} // namespace switchrecomp::aarch64
