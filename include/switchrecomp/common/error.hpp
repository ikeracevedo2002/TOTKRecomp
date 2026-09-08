#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
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
    InvalidControlFlow,
    UnsupportedControlFlow,
    AnalysisInstructionLimitExceeded,
    AnalysisBlockLimitExceeded,
    AnalysisWorklistLimitExceeded,
    AnalysisScopeViolation,
    AnalysisBudgetExceeded,
    FunctionBoundaryConflict,
    InvalidGuestAddress,
    UnresolvedIndirectFlow,
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
    IrVerificationFailed,
    InvalidIrValue,
    InvalidIrBlock,
    InvalidIrOperand,
    UnsupportedInstruction,
    UnsupportedOperandForm,
    LiftLimitExceeded,
    InterpreterError,
    ExecutionLimitExceeded,
    ExecutionTrap,
    InvalidRuntimeContext,
    InvalidAtomicWidth,
    MisalignedAtomicAccess,
    InvalidMemoryOrder,
    InvalidBarrier,
    UnsupportedSystemRegister,
    ThreadCreationFailed,
    InvalidThreadState,
    InvalidThreadId,
    FunctionRegistryFrozen,
    UnknownGuestFunction,
    RuntimeBoundary,
    RuntimeImportUnimplemented,
    RuntimeImportAbiViolation,
    RuntimeImportMemoryFault,
    RuntimeImportInvariantViolation,
    MissingImportBinding,
    CodegenFailure,
    LlvmVerificationFailed,
    JitCompilationFailed,
    DuplicateModuleIdentity,
    ModuleAddressOverlap,
    ModuleLayoutOverflow,
    AmbiguousProvider,
    InvalidProviderDefinition,
    ProviderSearchIncomplete,
    InvalidCrossModuleTransfer,
    ModuleSetEmpty,
    ModuleSetIdentityConflict,
    ModuleParseFailed,
    ModuleMaterializationFailed,
    ModuleManifestMismatch,
    ProviderNotFoundComplete,
    ProviderIneligible,
    ProviderMetadataInvalid,
    RelocationPlanFailed,
    ProviderFunctionNotDiscovered,
    ProviderExecutionBlocked,
    DirectoryScanFailed,
};

[[nodiscard]] std::string_view error_code_name(ErrorCode code) noexcept;

struct Error
{
    ErrorCode code;
    std::string message;
    // Optional deterministic context for a bounded analysis failure.  The
    // common error type owns this dependency-free record so the common layer
    // does not depend on analysis-specific types.
    struct BudgetContext
    {
        std::string domain;
        std::string dimension;
        std::size_t consumed = 0U;
        std::size_t limit = 0U;
        std::string module;
        std::string phase;
        std::size_t pending_work = 0U;
        std::optional<std::uint64_t> next_work;
    };
    std::optional<BudgetContext> budget_context = std::nullopt;
};

[[nodiscard]] inline Error make_error(ErrorCode code, std::string message)
{
    return Error{code, std::move(message), std::nullopt};
}

} // namespace switchrecomp
