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
    case ErrorCode::DecodeFailed:
        return "decode_failed";
    case ErrorCode::InstructionFetchFailed:
        return "instruction_fetch_failed";
    case ErrorCode::MisalignedInstructionAddress:
        return "misaligned_instruction_address";
    case ErrorCode::NonExecutableAddress:
        return "non_executable_address";
    case ErrorCode::InvalidBranchTarget:
        return "invalid_branch_target";
    case ErrorCode::UnsupportedControlFlow:
        return "unsupported_control_flow";
    case ErrorCode::AnalysisInstructionLimitExceeded:
        return "analysis_instruction_limit_exceeded";
    case ErrorCode::AnalysisBlockLimitExceeded:
        return "analysis_block_limit_exceeded";
    case ErrorCode::AnalysisWorklistLimitExceeded:
        return "analysis_worklist_limit_exceeded";
    case ErrorCode::AnalysisScopeViolation:
        return "analysis_scope_violation";
    }
    return "unknown";
}

} // namespace switchrecomp
