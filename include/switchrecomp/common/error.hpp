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
    UnmappedMemory,
    PermissionDenied,
    ArithmeticOverflow,
    ArithmeticUnderflow,
    IoError,
    InvalidFormat,
    InvalidManifest,
    Unsupported,
    UnsupportedCompression,
    DecompressionFailed,
    ResourceLimit,
    MissingFile,
    SizeMismatch,
    HashMismatch,
    PlaceholderManifest,
    DecodeFailed,
    InstructionFetchFailed,
    MisalignedInstructionAddress,
    NonExecutableAddress,
    InvalidBranchTarget,
    UnsupportedControlFlow,
    AnalysisInstructionLimitExceeded,
    AnalysisBlockLimitExceeded,
    AnalysisWorklistLimitExceeded,
    AnalysisScopeViolation,
    InvalidRelocationEntrySize,
    InvalidSymbolEntrySize,
    RelocationTableOutOfBounds,
    RelocationTargetNotWritableDuringLoad,
    MisalignedRelocationTarget,
    UnsupportedRelocationType,
    InvalidSymbolIndex,
    SymbolTableOutOfBounds,
    StringTableOutOfBounds,
    UnterminatedSymbolName,
    UndefinedStrongSymbol,
    DuplicateExternalSymbol,
    UnsupportedRelFormat,
    UnsupportedInstruction,
    UnsupportedOperandForm,
    UnsupportedAddressingMode,
    InvalidOperandCount,
    InvalidRegisterWidth,
    InvalidCfg,
    IrVerificationFailed,
    InvalidValueId,
    InvalidBlockId,
    UseBeforeDefinition,
    InvalidIrType,
    ExecutionMemoryFault,
    ExecutionInvalidBlock,
    ExecutionStepLimitExceeded,
    LLVMVerificationFailed,
    LLVMUnavailable,
    JitFailure,
    ObjectEmissionFailed,
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
