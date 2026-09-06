#include "switchrecomp/common/error.hpp"

namespace switchrecomp
{

std::string_view error_code_name(ErrorCode code) noexcept
{
    switch (code)
    {
    case ErrorCode::InvalidArgument:
        return "invalid_argument";
    case ErrorCode::OutOfBounds:
        return "out_of_bounds";
    case ErrorCode::UnmappedMemory:
        return "unmapped_memory";
    case ErrorCode::PermissionDenied:
        return "permission_denied";
    case ErrorCode::ArithmeticOverflow:
        return "arithmetic_overflow";
    case ErrorCode::ArithmeticUnderflow:
        return "arithmetic_underflow";
    case ErrorCode::IoError:
        return "io_error";
    case ErrorCode::InvalidFormat:
        return "invalid_format";
    case ErrorCode::InvalidManifest:
        return "invalid_manifest";
    case ErrorCode::Unsupported:
        return "unsupported";
    case ErrorCode::UnsupportedCompression:
        return "unsupported_compression";
    case ErrorCode::DecompressionFailed:
        return "decompression_failed";
    case ErrorCode::ResourceLimit:
        return "resource_limit";
    case ErrorCode::MissingFile:
        return "missing_file";
    case ErrorCode::SizeMismatch:
        return "size_mismatch";
    case ErrorCode::HashMismatch:
        return "hash_mismatch";
    case ErrorCode::PlaceholderManifest:
        return "placeholder_manifest";
    }
    return "unknown";
}

} // namespace switchrecomp
