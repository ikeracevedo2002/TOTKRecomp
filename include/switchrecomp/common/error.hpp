#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace switchrecomp
{

enum class ErrorCode
{
    InvalidArgument,
    OutOfBounds,
    ArithmeticOverflow,
    ArithmeticUnderflow,
    IoError,
    InvalidFormat,
    InvalidManifest,
    Unsupported,
    MissingFile,
    SizeMismatch,
    HashMismatch,
    PlaceholderManifest,
};

[[nodiscard]] std::string_view error_code_name(ErrorCode code) noexcept;

struct Error
{
    ErrorCode code;
    std::string message;
};

[[nodiscard]] inline Error make_error(ErrorCode code, std::string message)
{
    return Error{code, std::move(message)};
}

} // namespace switchrecomp
