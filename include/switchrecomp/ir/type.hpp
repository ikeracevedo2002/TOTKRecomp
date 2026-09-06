#pragma once

#include <cstdint>
#include <string_view>

namespace switchrecomp::ir
{

enum class TypeKind : std::uint8_t
{
    Void,
    I1,
    I8,
    I16,
    I32,
    I64,
};

class Type
{
  public:
    constexpr Type() noexcept = default;
    constexpr explicit Type(TypeKind kind) noexcept : kind_(kind) {}

    [[nodiscard]] constexpr TypeKind kind() const noexcept { return kind_; }
    [[nodiscard]] constexpr bool is_void() const noexcept { return kind_ == TypeKind::Void; }
    [[nodiscard]] constexpr bool is_integer() const noexcept
    {
        return kind_ != TypeKind::Void;
    }
    [[nodiscard]] constexpr std::uint8_t bit_width() const noexcept
    {
        switch (kind_)
        {
        case TypeKind::Void:
            return 0U;
        case TypeKind::I1:
            return 1U;
        case TypeKind::I8:
            return 8U;
        case TypeKind::I16:
            return 16U;
        case TypeKind::I32:
            return 32U;
        case TypeKind::I64:
            return 64U;
        }
        return 0U;
    }

    [[nodiscard]] constexpr bool valid() const noexcept
    {
        return kind_ <= TypeKind::I64;
    }

    friend constexpr bool operator==(Type, Type) noexcept = default;

  private:
    TypeKind kind_ = TypeKind::Void;
};

[[nodiscard]] constexpr Type void_type() noexcept { return Type(TypeKind::Void); }
[[nodiscard]] constexpr Type i1_type() noexcept { return Type(TypeKind::I1); }
[[nodiscard]] constexpr Type i8_type() noexcept { return Type(TypeKind::I8); }
[[nodiscard]] constexpr Type i16_type() noexcept { return Type(TypeKind::I16); }
[[nodiscard]] constexpr Type i32_type() noexcept { return Type(TypeKind::I32); }
[[nodiscard]] constexpr Type i64_type() noexcept { return Type(TypeKind::I64); }

[[nodiscard]] std::string_view type_name(Type type) noexcept;

} // namespace switchrecomp::ir
