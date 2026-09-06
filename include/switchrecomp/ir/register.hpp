#pragma once

#include <cstdint>
#include <string>

namespace switchrecomp::ir
{

enum class RegisterWidth : std::uint8_t
{
    W32,
    X64,
};

struct GuestRegister
{
    RegisterWidth width = RegisterWidth::X64;
    std::uint8_t index = 0U;
    bool is_stack_pointer = false;
    bool is_zero = false;

    [[nodiscard]] bool valid() const noexcept
    {
        return index <= 31U && !(is_stack_pointer && is_zero) &&
               (is_stack_pointer || is_zero || index < 31U);
    }

    friend bool operator==(const GuestRegister&, const GuestRegister&) = default;
};

[[nodiscard]] std::string register_name(const GuestRegister& reg);

} // namespace switchrecomp::ir
