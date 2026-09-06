#include "switchrecomp/common/error.hpp"

namespace switchrecomp
{

std::string_view error_code_name(ErrorCode code) noexcept
{
    switch (code)
    {
    case ErrorCode::InvalidArgument: return "invalid_argument";
    case ErrorCode::OutOfBounds: return "out_of_bounds";
    case ErrorCode::UnmappedMemory: return "unmapped_memory";
    case ErrorCode::PermissionDenied: return "permission_denied";
    case ErrorCode::ArithmeticOverflow: return "arithmetic_overflow";
    case ErrorCode::ArithmeticUnderflow: return "arithmetic_underflow";
    case ErrorCode::IoError: return "io_error";
    case ErrorCode::InvalidFormat: return "invalid_format";
    case ErrorCode::InvalidManifest: return "invalid_manifest";
    case ErrorCode::Unsupported: return "unsupported";
    case ErrorCode::UnsupportedCompression: return "unsupported_compression";
    case ErrorCode::DecompressionFailed: return "decompression_failed";
    case ErrorCode::ResourceLimit: return "resource_limit";
    case ErrorCode::MissingFile: return "missing_file";
    case ErrorCode::SizeMismatch: return "size_mismatch";
    case ErrorCode::HashMismatch: return "hash_mismatch";
    case ErrorCode::PlaceholderManifest: return "placeholder_manifest";
    case ErrorCode::DecodeFailed: return "decode_failed";
    case ErrorCode::InstructionFetchFailed: return "instruction_fetch_failed";
    case ErrorCode::MisalignedInstructionAddress: return "misaligned_instruction_address";
    case ErrorCode::NonExecutableAddress: return "non_executable_address";
    case ErrorCode::InvalidBranchTarget: return "invalid_branch_target";
    case ErrorCode::InvalidControlFlow: return "invalid_control_flow";
    case ErrorCode::UnsupportedControlFlow: return "unsupported_control_flow";
    case ErrorCode::AnalysisInstructionLimitExceeded: return "analysis_instruction_limit_exceeded";
    case ErrorCode::AnalysisBlockLimitExceeded: return "analysis_block_limit_exceeded";
    case ErrorCode::AnalysisWorklistLimitExceeded: return "analysis_worklist_limit_exceeded";
    case ErrorCode::AnalysisScopeViolation: return "analysis_scope_violation";
    case ErrorCode::InvalidRelocationEntrySize: return "invalid_relocation_entry_size";
    case ErrorCode::InvalidSymbolEntrySize: return "invalid_symbol_entry_size";
    case ErrorCode::RelocationTableOutOfBounds: return "relocation_table_out_of_bounds";
    case ErrorCode::RelocationTargetNotWritableDuringLoad: return "relocation_target_not_writable_during_load";
    case ErrorCode::MisalignedRelocationTarget: return "misaligned_relocation_target";
    case ErrorCode::UnsupportedRelocationType: return "unsupported_relocation_type";
    case ErrorCode::InvalidSymbolIndex: return "invalid_symbol_index";
    case ErrorCode::SymbolTableOutOfBounds: return "symbol_table_out_of_bounds";
    case ErrorCode::StringTableOutOfBounds: return "string_table_out_of_bounds";
    case ErrorCode::UnterminatedSymbolName: return "unterminated_symbol_name";
    case ErrorCode::UndefinedStrongSymbol: return "undefined_strong_symbol";
    case ErrorCode::DuplicateExternalSymbol: return "duplicate_external_symbol";
    case ErrorCode::UnsupportedRelFormat: return "unsupported_rel_format";
    case ErrorCode::IrVerificationFailed: return "ir_verification_failed";
    case ErrorCode::InvalidIrValue: return "invalid_ir_value";
    case ErrorCode::InvalidIrBlock: return "invalid_ir_block";
    case ErrorCode::InvalidIrOperand: return "invalid_ir_operand";
    case ErrorCode::UnsupportedInstruction: return "unsupported_instruction";
    case ErrorCode::UnsupportedOperandForm: return "unsupported_operand_form";
    case ErrorCode::LiftLimitExceeded: return "lift_limit_exceeded";
    case ErrorCode::InterpreterError: return "interpreter_error";
    case ErrorCode::ExecutionLimitExceeded: return "execution_limit_exceeded";
    case ErrorCode::ExecutionTrap: return "execution_trap";
    case ErrorCode::InvalidRuntimeContext: return "invalid_runtime_context";
    case ErrorCode::InvalidAtomicWidth: return "invalid_atomic_width";
    case ErrorCode::MisalignedAtomicAccess: return "misaligned_atomic_access";
    case ErrorCode::InvalidMemoryOrder: return "invalid_memory_order";
    case ErrorCode::InvalidBarrier: return "invalid_barrier";
    case ErrorCode::UnsupportedSystemRegister: return "unsupported_system_register";
    case ErrorCode::ThreadCreationFailed: return "thread_creation_failed";
    case ErrorCode::InvalidThreadState: return "invalid_thread_state";
    case ErrorCode::InvalidThreadId: return "invalid_thread_id";
    case ErrorCode::LlvmVerificationFailed: return "llvm_verification_failed";
    case ErrorCode::JitCompilationFailed: return "jit_compilation_failed";
    }
    return "unknown";
}

} // namespace switchrecomp
