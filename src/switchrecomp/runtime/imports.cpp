#include "switchrecomp/runtime/imports.hpp"

#include "switchrecomp/common/checked_arithmetic.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <utility>

namespace switchrecomp::runtime
{

namespace
{

[[nodiscard]] bool is_memory_error(ErrorCode code) noexcept
{
    switch (code)
    {
    case ErrorCode::OutOfBounds:
    case ErrorCode::UnmappedMemory:
    case ErrorCode::PermissionDenied:
    case ErrorCode::InstructionFetchFailed:
    case ErrorCode::NonExecutableAddress:
    case ErrorCode::InvalidGuestAddress:
    case ErrorCode::MisalignedAtomicAccess:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] std::uint64_t load_u64_le(std::span<const std::byte> bytes) noexcept
{
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < bytes.size(); ++index)
    {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[index])) <<
                 (index * 8U);
    }
    return value;
}

[[nodiscard]] bool valid_alignment(std::uint64_t alignment) noexcept
{
    return alignment != 0U && (alignment & (alignment - 1U)) == 0U;
}

struct AddressRange
{
    memory::GuestAddress begin = 0U;
    memory::GuestAddress end = 0U;
    const char* name = "range";
};

[[nodiscard]] bool overlaps(const AddressRange& left, const AddressRange& right) noexcept
{
    return left.begin < right.end && right.begin < left.end;
}

[[nodiscard]] Result<void> validate_range_shape(const AddressRange& range)
{
    if (range.end < range.begin)
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            std::string("DSO ") + range.name + " end precedes its start"));
    }
    const auto checked = checked_guest_range(range.begin, range.end - range.begin);
    if (!checked)
    {
        return Result<void>::failure(make_error(
            checked.error().code,
            std::string("DSO ") + range.name + " range arithmetic failed: " +
                checked.error().message));
    }
    return Result<void>::success();
}

[[nodiscard]] Result<void> require_permissions(const memory::GuestMemory& memory,
                                                const AddressRange& range,
                                                memory::GuestMemoryPermissions required)
{
    if (range.begin == range.end)
    {
        return Result<void>::success();
    }
    const auto permissions = memory.permissions_at(range.begin, range.end - range.begin);
    if (!permissions)
    {
        return Result<void>::failure(make_error(
            permissions.error().code,
            std::string("DSO ") + range.name + " range is not readable/mapped: " +
                permissions.error().message));
    }
    if (!memory::has_permission(permissions.value(), required))
    {
        return Result<void>::failure(make_error(
            ErrorCode::PermissionDenied,
            std::string("DSO ") + range.name + " range has insufficient permissions"));
    }
    return Result<void>::success();
}

[[nodiscard]] RuntimeImportOutcome outcome_from_validation_error(const Error& error)
{
    if (is_memory_error(error.code))
    {
        return RuntimeImportOutcome::memory_fault(error);
    }
    return RuntimeImportOutcome::abi_violation(error);
}

} // namespace

std::string_view runtime_subsystem_name(RuntimeSubsystem subsystem) noexcept
{
    switch (subsystem)
    {
    case RuntimeSubsystem::C: return "c_runtime";
    case RuntimeSubsystem::Cxx: return "cxx_runtime";
    case RuntimeSubsystem::Tls: return "tls";
    case RuntimeSubsystem::DynamicLoader: return "dynamic_loader";
    case RuntimeSubsystem::Memory: return "memory";
    case RuntimeSubsystem::Threading: return "threading";
    case RuntimeSubsystem::Time: return "time";
    case RuntimeSubsystem::Filesystem: return "filesystem";
    case RuntimeSubsystem::Horizon: return "horizon";
    case RuntimeSubsystem::Graphics: return "graphics";
    case RuntimeSubsystem::Audio: return "audio";
    case RuntimeSubsystem::Input: return "input";
    case RuntimeSubsystem::Unknown: return "unknown";
    }
    return "unknown";
}

std::string_view runtime_support_status_name(RuntimeSupportStatus status) noexcept
{
    switch (status)
    {
    case RuntimeSupportStatus::Unknown: return "unknown";
    case RuntimeSupportStatus::Unimplemented: return "unimplemented";
    case RuntimeSupportStatus::Implemented: return "implemented";
    }
    return "unknown";
}

std::string_view abi_value_kind_name(AbiValueKind kind) noexcept
{
    switch (kind)
    {
    case AbiValueKind::Integer32: return "integer32";
    case AbiValueKind::Integer64: return "integer64";
    case AbiValueKind::Pointer: return "pointer";
    case AbiValueKind::Unknown: return "unknown";
    }
    return "unknown";
}

std::string_view abi_return_kind_name(AbiReturnKind kind) noexcept
{
    switch (kind)
    {
    case AbiReturnKind::Void: return "void";
    case AbiReturnKind::Integer32: return "integer32";
    case AbiReturnKind::Integer64: return "integer64";
    case AbiReturnKind::Unknown: return "unknown";
    }
    return "unknown";
}

std::string_view external_invocation_kind_name(ExternalInvocationKind kind) noexcept
{
    switch (kind)
    {
    case ExternalInvocationKind::Call: return "call";
    case ExternalInvocationKind::TailTransfer: return "tail_transfer";
    }
    return "unknown";
}

Result<memory::GuestAddress> AArch64GuestCall::stack_argument_address(std::size_t index) const
{
    if (cpu_ == nullptr || memory_ == nullptr || signature_ == nullptr)
    {
        return Result<memory::GuestAddress>::failure(make_error(
            ErrorCode::InvalidRuntimeContext, "AArch64 guest call has no CPU, memory, or signature"));
    }
    if (index < 8U)
    {
        return Result<memory::GuestAddress>::failure(make_error(
            ErrorCode::InvalidArgument, "register argument has no stack location"));
    }
    if (index > static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max()))
    {
        return Result<memory::GuestAddress>::failure(make_error(
            ErrorCode::ArithmeticOverflow, "stack argument index does not fit guest arithmetic"));
    }
    const auto byte_offset = checked_mul_u64(
        static_cast<std::uint64_t>(index - 8U), static_cast<std::uint64_t>(sizeof(std::uint64_t)));
    if (!byte_offset)
    {
        return Result<memory::GuestAddress>::failure(byte_offset.error());
    }
    const auto address = checked_add_u64(cpu_->sp, byte_offset.value());
    if (!address)
    {
        return Result<memory::GuestAddress>::failure(make_error(
            address.error().code, "stack argument address overflows from SP"));
    }
    return address;
}

Result<void> AArch64GuestCall::validate() const
{
    if (cpu_ == nullptr || memory_ == nullptr || signature_ == nullptr)
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidRuntimeContext, "AArch64 guest call has no CPU, memory, or signature"));
    }
    if (cpu_->sp == 0U || (cpu_->sp & 0xfU) != 0U)
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidGuestAddress,
            "AArch64 ABI requires a non-zero 16-byte-aligned guest SP"));
    }
    const auto count = signature_->argument_count.value_or(signature_->observed_argument_count);
    if (signature_->argument_count && !signature_->arguments.empty() &&
        signature_->arguments.size() != signature_->argument_count.value())
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument, "ABI signature argument vector does not match its count"));
    }
    if (signature_->argument_count && signature_->observed_argument_count > count)
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument, "ABI evidence observes more arguments than its signature"));
    }

    // A register-only call may arrive with SP at the upper edge of a
    // downward-growing guest stack mapping. Require a mapped SP only when
    // this signature actually needs stack argument slots.
    if (count > 8U)
    {
        const auto stack_mapping = memory_->permissions_at(cpu_->sp, 1U);
        if (!stack_mapping)
        {
            return Result<void>::failure(stack_mapping.error());
        }
    }

    for (std::size_t index = 8U; index < count; ++index)
    {
        const auto address = stack_argument_address(index);
        if (!address)
        {
            return Result<void>::failure(address.error());
        }
        std::array<std::byte, sizeof(std::uint64_t)> bytes{};
        const auto read = memory_->read(address.value(), bytes);
        if (!read)
        {
            return Result<void>::failure(read.error());
        }
    }
    return Result<void>::success();
}

Result<std::uint64_t> AArch64GuestCall::raw_argument(std::size_t index) const
{
    if (cpu_ == nullptr || memory_ == nullptr || signature_ == nullptr)
    {
        return Result<std::uint64_t>::failure(make_error(
            ErrorCode::InvalidRuntimeContext, "AArch64 guest call has no CPU, memory, or signature"));
    }
    const auto count = signature_->argument_count.value_or(signature_->observed_argument_count);
    if (index >= count)
    {
        return Result<std::uint64_t>::failure(make_error(
            ErrorCode::OutOfBounds, "ABI argument index is outside the declared/evidenced call"));
    }
    if (index < 8U)
    {
        return Result<std::uint64_t>::success(cpu_->x[index]);
    }

    const auto address = stack_argument_address(index);
    if (!address)
    {
        return Result<std::uint64_t>::failure(address.error());
    }
    std::array<std::byte, sizeof(std::uint64_t)> bytes{};
    const auto read = memory_->read(address.value(), bytes);
    if (!read)
    {
        return Result<std::uint64_t>::failure(read.error());
    }
    return Result<std::uint64_t>::success(load_u64_le(bytes));
}

Result<memory::GuestAddress> AArch64GuestCall::pointer_argument(std::size_t index) const
{
    const auto value = raw_argument(index);
    if (!value)
    {
        return Result<memory::GuestAddress>::failure(value.error());
    }
    return Result<memory::GuestAddress>::success(value.value());
}

Result<void> AArch64GuestCall::set_integer_return(std::uint64_t value) noexcept
{
    if (cpu_ == nullptr)
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidRuntimeContext, "AArch64 guest call has no CPU state"));
    }
    cpu_->x[0U] = value;
    return_written_ = true;
    return Result<void>::success();
}

Result<void> AArch64GuestCall::set_integer32_return(std::uint32_t value) noexcept
{
    return set_integer_return(static_cast<std::uint64_t>(value));
}

memory::GuestAddress AArch64GuestCall::sp() const noexcept
{
    return cpu_ == nullptr ? 0U : cpu_->sp;
}

memory::GuestAddress AArch64GuestCall::lr() const noexcept
{
    return cpu_ == nullptr ? 0U : cpu_->x[30U];
}

GuestAbiSnapshot AArch64GuestCall::snapshot() const noexcept
{
    GuestAbiSnapshot snapshot;
    if (cpu_ == nullptr)
    {
        return snapshot;
    }
    for (std::size_t index = 0U; index < snapshot.x0_x7.size(); ++index)
    {
        snapshot.x0_x7[index] = cpu_->x[index];
    }
    snapshot.sp = cpu_->sp;
    snapshot.x29 = cpu_->x[29U];
    snapshot.x30 = cpu_->x[30U];
    snapshot.pc = cpu_->pc;
    return snapshot;
}

Result<void> validate_dso_tls_descriptor(const memory::GuestMemory& memory,
                                         const DsoTlsDescriptor& descriptor)
{
    const std::array<AddressRange, 3U> ranges{
        AddressRange{descriptor.executable_begin, descriptor.executable_end, "executable"},
        AddressRange{descriptor.tdata_begin, descriptor.tdata_end, ".tdata"},
        AddressRange{descriptor.tbss_begin, descriptor.tbss_end, ".tbss"},
    };
    for (const auto& range : ranges)
    {
        const auto shape = validate_range_shape(range);
        if (!shape)
        {
            return shape;
        }
    }
    for (std::size_t left = 0U; left < ranges.size(); ++left)
    {
        for (std::size_t right = left + 1U; right < ranges.size(); ++right)
        {
            if (overlaps(ranges[left], ranges[right]))
            {
                return Result<void>::failure(make_error(
                    ErrorCode::InvalidArgument,
                    std::string("DSO ranges overlap: ") + ranges[left].name + " and " +
                        ranges[right].name));
            }
        }
    }
    if (!valid_alignment(descriptor.tdata_alignment) ||
        !valid_alignment(descriptor.tbss_alignment))
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument, "DSO TLS alignment must be a non-zero power of two"));
    }
    if (descriptor.tdata_begin != descriptor.tdata_end &&
        descriptor.tdata_begin % descriptor.tdata_alignment != 0U)
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument, "DSO .tdata start does not satisfy its alignment"));
    }
    if (descriptor.tbss_begin != descriptor.tbss_end &&
        descriptor.tbss_begin % descriptor.tbss_alignment != 0U)
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument, "DSO .tbss start does not satisfy its alignment"));
    }

    const auto executable = require_permissions(
        memory, ranges[0U], memory::GuestMemoryPermissions::Read |
                                 memory::GuestMemoryPermissions::Execute);
    if (!executable)
    {
        return executable;
    }
    const auto tdata = require_permissions(memory, ranges[1U], memory::GuestMemoryPermissions::Read);
    if (!tdata)
    {
        return tdata;
    }
    return require_permissions(memory, ranges[2U], memory::GuestMemoryPermissions::Read |
                                                        memory::GuestMemoryPermissions::Write);
}

Result<void> RuntimeState::register_dso_tls(const memory::GuestMemory& memory,
                                            const DsoTlsDescriptor& descriptor)
{
    const auto valid = validate_dso_tls_descriptor(memory, descriptor);
    if (!valid)
    {
        return valid;
    }

    const std::array<AddressRange, 3U> new_ranges{
        AddressRange{descriptor.executable_begin, descriptor.executable_end, "executable"},
        AddressRange{descriptor.tdata_begin, descriptor.tdata_end, ".tdata"},
        AddressRange{descriptor.tbss_begin, descriptor.tbss_end, ".tbss"},
    };
    for (const auto& existing : dso_modules_)
    {
        const std::array<AddressRange, 3U> existing_ranges{
            AddressRange{existing.executable_begin, existing.executable_end, "executable"},
            AddressRange{existing.tdata_begin, existing.tdata_end, ".tdata"},
            AddressRange{existing.tbss_begin, existing.tbss_end, ".tbss"},
        };
        for (const auto& left : new_ranges)
        {
            for (const auto& right : existing_ranges)
            {
                if (overlaps(left, right))
                {
                    return Result<void>::failure(make_error(
                        ErrorCode::InvalidArgument,
                        "DSO registration overlaps an already registered module"));
                }
            }
        }
    }

    try
    {
        // Build a proposed container first. The original vector is unchanged
        // if allocation fails, and is committed only after push_back succeeds.
        auto proposed = dso_modules_;
        proposed.push_back(descriptor);
        dso_modules_.swap(proposed);
    }
    catch (const std::bad_alloc&)
    {
        return Result<void>::failure(
            make_error(ErrorCode::ResourceLimit, "DSO runtime state allocation failed"));
    }
    return Result<void>::success();
}

std::string_view runtime_import_outcome_name(RuntimeImportOutcomeKind kind) noexcept
{
    switch (kind)
    {
    case RuntimeImportOutcomeKind::Handled: return "handled";
    case RuntimeImportOutcomeKind::Unimplemented: return "unimplemented";
    case RuntimeImportOutcomeKind::AbiViolation: return "abi_violation";
    case RuntimeImportOutcomeKind::MemoryFault: return "memory_fault";
    case RuntimeImportOutcomeKind::InvariantViolation: return "invariant_violation";
    }
    return "invariant_violation";
}

RuntimeImportOutcome RuntimeImportOutcome::handled()
{
    return RuntimeImportOutcome{RuntimeImportOutcomeKind::Handled, {}, std::nullopt, false, false};
}

RuntimeImportOutcome RuntimeImportOutcome::unimplemented(std::string diagnostic)
{
    return RuntimeImportOutcome{RuntimeImportOutcomeKind::Unimplemented, std::move(diagnostic),
                                std::nullopt, false, false};
}

RuntimeImportOutcome RuntimeImportOutcome::abi_violation(Error error)
{
    return RuntimeImportOutcome{RuntimeImportOutcomeKind::AbiViolation, error.message,
                                std::move(error), false, false};
}

RuntimeImportOutcome RuntimeImportOutcome::memory_fault(Error error)
{
    return RuntimeImportOutcome{RuntimeImportOutcomeKind::MemoryFault, error.message,
                                std::move(error), false, false};
}

RuntimeImportOutcome RuntimeImportOutcome::invariant_violation(Error error)
{
    return RuntimeImportOutcome{RuntimeImportOutcomeKind::InvariantViolation, error.message,
                                std::move(error), false, false};
}

Result<void> RuntimeImportRegistry::register_import(RuntimeImportDescriptor descriptor,
                                                     RuntimeImportHandler handler)
{
    if (descriptor.symbol_name.empty())
    {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidArgument, "runtime import symbol name cannot be empty"));
    }
    if (descriptor.support == RuntimeSupportStatus::Unknown)
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument, "registered runtime import requires an explicit support status"));
    }
    if (descriptor.signature.argument_count && !descriptor.signature.arguments.empty() &&
        descriptor.signature.arguments.size() != descriptor.signature.argument_count.value())
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument, "runtime import ABI argument vector does not match its count"));
    }
    if (descriptor.signature.argument_count &&
        descriptor.signature.observed_argument_count > descriptor.signature.argument_count.value())
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument, "runtime import ABI evidence exceeds its argument count"));
    }
    if (descriptor.support == RuntimeSupportStatus::Implemented && !handler)
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument, "implemented runtime import requires a controlled handler"));
    }
    if (descriptor.support == RuntimeSupportStatus::Unimplemented && handler)
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument, "unimplemented runtime import cannot have a handler"));
    }
    try
    {
        // Preserve the lookup key before moving the descriptor into the
        // registration object. Argument evaluation order is otherwise not a
        // suitable basis for a stable registry identity.
        const auto key = descriptor.symbol_name;
        auto inserted = entries_.emplace(
            key,
            Registration{std::move(descriptor), std::move(handler)});
        if (!inserted.second)
        {
            return Result<void>::failure(make_error(
                ErrorCode::DuplicateExternalSymbol,
                "runtime import is already registered: " + inserted.first->first));
        }
    }
    catch (const std::bad_alloc&)
    {
        return Result<void>::failure(
            make_error(ErrorCode::ResourceLimit, "runtime import registry allocation failed"));
    }
    return Result<void>::success();
}

const RuntimeImportDescriptor* RuntimeImportRegistry::find(std::string_view symbol_name) const noexcept
{
    const auto found = entries_.find(symbol_name);
    return found == entries_.end() ? nullptr : &found->second.descriptor;
}

std::vector<RuntimeImportDescriptor> RuntimeImportRegistry::descriptors() const
{
    std::vector<RuntimeImportDescriptor> result;
    result.reserve(entries_.size());
    for (const auto& [name, registration] : entries_)
    {
        (void)name;
        result.push_back(registration.descriptor);
    }
    return result;
}

Result<RuntimeImportOutcome> RuntimeImportRegistry::invoke(RuntimeImportContext& context) const
{
    const auto found = entries_.find(context.descriptor.symbol_name);
    if (found == entries_.end())
    {
        return Result<RuntimeImportOutcome>::failure(make_error(
            ErrorCode::MissingImportBinding,
            "runtime import is not registered: " + context.descriptor.symbol_name));
    }
    const auto& registration = found->second;
    if (&registration.descriptor != &context.descriptor ||
        context.provenance.symbol.name != registration.descriptor.symbol_name)
    {
        return Result<RuntimeImportOutcome>::success(RuntimeImportOutcome::invariant_violation(
            make_error(ErrorCode::RuntimeImportInvariantViolation,
                       "runtime import descriptor and provenance identity disagree")));
    }

    const auto abi = context.abi.validate();
    if (!abi)
    {
        auto outcome = outcome_from_validation_error(abi.error());
        outcome.abi_validated = false;
        outcome.abi_signature_known = registration.descriptor.signature.argument_count.has_value();
        return Result<RuntimeImportOutcome>::success(std::move(outcome));
    }
    if (registration.descriptor.support == RuntimeSupportStatus::Unimplemented)
    {
        auto outcome = RuntimeImportOutcome::unimplemented(
            "runtime import is recognized but no evidence-backed handler is implemented");
        outcome.abi_validated = true;
        outcome.abi_signature_known = registration.descriptor.signature.argument_count.has_value();
        return Result<RuntimeImportOutcome>::success(std::move(outcome));
    }
    if (registration.descriptor.support != RuntimeSupportStatus::Implemented || !registration.handler)
    {
        return Result<RuntimeImportOutcome>::success(RuntimeImportOutcome::invariant_violation(
            make_error(ErrorCode::RuntimeImportInvariantViolation,
                       "runtime import has no valid support/handler pairing")));
    }

    try
    {
        auto handled = registration.handler(context);
        if (!handled)
        {
            auto outcome = is_memory_error(handled.error().code)
                               ? RuntimeImportOutcome::memory_fault(handled.error())
                               : RuntimeImportOutcome::invariant_violation(handled.error());
            outcome.abi_validated = true;
            outcome.abi_signature_known = registration.descriptor.signature.argument_count.has_value();
            return Result<RuntimeImportOutcome>::success(std::move(outcome));
        }
        auto outcome = std::move(handled).value();
        outcome.abi_validated = true;
        outcome.abi_signature_known = registration.descriptor.signature.argument_count.has_value();
        if (outcome.kind == RuntimeImportOutcomeKind::Handled &&
            (registration.descriptor.signature.return_kind == AbiReturnKind::Integer32 ||
             registration.descriptor.signature.return_kind == AbiReturnKind::Integer64) &&
            !context.abi.return_written())
        {
            outcome = RuntimeImportOutcome::invariant_violation(make_error(
                ErrorCode::RuntimeImportInvariantViolation,
                "handled runtime import did not set its declared integer return register"));
            outcome.abi_validated = true;
            outcome.abi_signature_known = registration.descriptor.signature.argument_count.has_value();
        }
        return Result<RuntimeImportOutcome>::success(std::move(outcome));
    }
    catch (const std::exception& ex)
    {
        return Result<RuntimeImportOutcome>::success(RuntimeImportOutcome::invariant_violation(
            make_error(ErrorCode::RuntimeImportInvariantViolation,
                       std::string("runtime import handler threw: ") + ex.what())));
    }
    catch (...)
    {
        return Result<RuntimeImportOutcome>::success(RuntimeImportOutcome::invariant_violation(
            make_error(ErrorCode::RuntimeImportInvariantViolation,
                       "runtime import handler threw an unknown exception")));
    }
}

RuntimeImportDescriptor nnmusl_init_dso_descriptor()
{
    RuntimeImportDescriptor descriptor;
    descriptor.symbol_name = "__nnmusl_init_dso";
    descriptor.subsystem = RuntimeSubsystem::DynamicLoader;
    descriptor.support = RuntimeSupportStatus::Unimplemented;
    descriptor.signature.argument_count = std::nullopt;
    descriptor.signature.observed_argument_count = 10U;
    descriptor.signature.return_kind = AbiReturnKind::Unknown;
    descriptor.evidence.sources = {
        "M11 real-call-site analysis",
        "AAPCS64 register/stack placement model",
        "public generated nnsdk symbol declaration (name only)",
    };
    descriptor.evidence.confidence = "partial_observation";
    descriptor.evidence.rationale =
        "The measured caller prepares X0-X7 and two 8-byte stack slots. The formal parameter "
        "types, complete prototype, DSO/TLS side effects, and return-value contract remain "
        "unestablished; no handler is therefore registered.";
    return descriptor;
}

Result<void> register_m12_evidence_imports(RuntimeImportRegistry& registry)
{
    return registry.register_import(nnmusl_init_dso_descriptor());
}

} // namespace switchrecomp::runtime
