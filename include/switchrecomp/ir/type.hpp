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
    F32,
    F64,
    V128,
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
        return kind_ >= TypeKind::I1 && kind_ <= TypeKind::I64;
    }
    [[nodiscard]] constexpr bool is_floating() const noexcept
    {
        return kind_ == TypeKind::F32 || kind_ == TypeKind::F64;
    }
    [[nodiscard]] constexpr bool is_vector() const noexcept { return kind_ == TypeKind::V128; }
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
        case TypeKind::F32:
            return 32U;
        case TypeKind::F64:
            return 64U;
        case TypeKind::V128:
            return 128U;
        }
        return 0U;
    }

    [[nodiscard]] constexpr bool valid() const noexcept
    {
        return kind_ <= TypeKind::V128;
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
[[nodiscard]] constexpr Type f32_type() noexcept { return Type(TypeKind::F32); }
[[nodiscard]] constexpr Type f64_type() noexcept { return Type(TypeKind::F64); }
[[nodiscard]] constexpr Type v128_type() noexcept { return Type(TypeKind::V128); }

[[nodiscard]] std::string_view type_name(Type type) noexcept;

} // namespace switchrecomp::ir
