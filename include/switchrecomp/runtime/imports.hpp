#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/format/dynamic_symbols.hpp"
#include "switchrecomp/format/module_metadata.hpp"
#include "switchrecomp/format/elf_rela.hpp"
#include "switchrecomp/memory/guest_memory.hpp"
#include "switchrecomp/runtime/cpu_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace switchrecomp::runtime
{

enum class RuntimeSubsystem : std::uint8_t
{
    C,
    Cxx,
    Tls,
    DynamicLoader,
    Memory,
    Threading,
    Time,
    Filesystem,
    Horizon,
    Graphics,
    Audio,
    Input,
    Unknown,
};

[[nodiscard]] std::string_view runtime_subsystem_name(RuntimeSubsystem subsystem) noexcept;

enum class RuntimeSupportStatus : std::uint8_t
{
    Unknown,
    Unimplemented,
    Implemented,
};

[[nodiscard]] std::string_view runtime_support_status_name(RuntimeSupportStatus status) noexcept;

enum class AbiValueKind : std::uint8_t
{
    Integer32,
    Integer64,
    Pointer,
    Unknown,
};

[[nodiscard]] std::string_view abi_value_kind_name(AbiValueKind kind) noexcept;

enum class AbiReturnKind : std::uint8_t
{
    Void,
    Integer32,
    Integer64,
    Unknown,
};

[[nodiscard]] std::string_view abi_return_kind_name(AbiReturnKind kind) noexcept;

// A signature may remain deliberately partial. observed_argument_count records
// the slots established by binary evidence without claiming that those slots
// are the complete formal prototype.
struct AbiSignature
{
    std::optional<std::size_t> argument_count;
    std::vector<AbiValueKind> arguments;
    std::size_t observed_argument_count = 0U;
    AbiReturnKind return_kind = AbiReturnKind::Unknown;
};

struct RuntimeEvidence
{
    std::vector<std::string> sources;
    std::string confidence;
    std::string rationale;
};

struct RuntimeImportDescriptor
{
    std::string symbol_name;
    RuntimeSubsystem subsystem = RuntimeSubsystem::Unknown;
    RuntimeSupportStatus support = RuntimeSupportStatus::Unknown;
    AbiSignature signature;
    RuntimeEvidence evidence;
};

enum class ExternalInvocationKind : std::uint8_t
{
    Call,
    TailTransfer,
};

[[nodiscard]] std::string_view external_invocation_kind_name(
    ExternalInvocationKind kind) noexcept;

// This is copied from the already validated relocation boundary. It is a
// guest-side identity and provenance record; it contains no host address.
struct ImportProvenance
{
    memory::GuestAddress relocation_target = 0U;
    std::size_t relocation_index = 0U;
    format::Relocation relocation{};
    format::ImportSymbol symbol{};
};

struct GuestAbiSnapshot
{
    std::array<std::uint64_t, 8> x0_x7{};
    memory::GuestAddress sp = 0U;
    memory::GuestAddress x29 = 0U;
    memory::GuestAddress x30 = 0U;
    memory::GuestAddress pc = 0U;
};

class AArch64GuestCall
{
  public:
    AArch64GuestCall(CpuState& cpu, memory::GuestMemory& memory,
                     const AbiSignature& signature) noexcept
        : cpu_(&cpu), memory_(&memory), signature_(&signature)
    {
    }

    // Validates the call-frame parts that are required by the known or
    // observed signature. Pointer target validity remains handler-owned: a
    // null pointer can be a valid argument, while a non-null range needs a
    // handler-specific extent and permission check.
    [[nodiscard]] Result<void> validate() const;
    [[nodiscard]] Result<std::uint64_t> raw_argument(std::size_t index) const;
    [[nodiscard]] Result<std::uint64_t> integer_argument(std::size_t index) const
    {
        return raw_argument(index);
    }
    [[nodiscard]] Result<memory::GuestAddress> pointer_argument(std::size_t index) const;

    [[nodiscard]] Result<void> set_integer_return(std::uint64_t value) noexcept;
    [[nodiscard]] Result<void> set_integer32_return(std::uint32_t value) noexcept;

    [[nodiscard]] memory::GuestAddress sp() const noexcept;
    [[nodiscard]] memory::GuestAddress lr() const noexcept;
    [[nodiscard]] GuestAbiSnapshot snapshot() const noexcept;
    [[nodiscard]] const AbiSignature& signature() const noexcept { return *signature_; }
    [[nodiscard]] bool return_written() const noexcept { return return_written_; }

  private:
    [[nodiscard]] Result<memory::GuestAddress> stack_argument_address(std::size_t index) const;

    CpuState* cpu_ = nullptr;
    memory::GuestMemory* memory_ = nullptr;
    const AbiSignature* signature_ = nullptr;
    bool return_written_ = false;
};

struct DsoTlsDescriptor
{
    memory::GuestAddress executable_begin = 0U;
    memory::GuestAddress executable_end = 0U;
    memory::GuestAddress tdata_begin = 0U;
    memory::GuestAddress tdata_end = 0U;
    memory::GuestAddress tbss_begin = 0U;
    memory::GuestAddress tbss_end = 0U;
    std::uint64_t tdata_alignment = 1U;
    std::uint64_t tbss_alignment = 1U;

    friend bool operator==(const DsoTlsDescriptor&, const DsoTlsDescriptor&) = default;
};

[[nodiscard]] Result<void> validate_dso_tls_descriptor(
    const memory::GuestMemory& memory, const DsoTlsDescriptor& descriptor);

// Runtime-owned mutable state is session-local. Registration validates a
// complete proposed descriptor before committing it, so a failed operation
// cannot leave a half-registered DSO.
class RuntimeState
{
  public:
    void reset() noexcept { dso_modules_.clear(); }

    [[nodiscard]] Result<void> register_dso_tls(const memory::GuestMemory& memory,
                                                const DsoTlsDescriptor& descriptor);
    [[nodiscard]] const std::vector<DsoTlsDescriptor>& dso_modules() const noexcept
    {
        return dso_modules_;
    }
    [[nodiscard]] std::size_t dso_modules_registered() const noexcept
    {
        return dso_modules_.size();
    }

  private:
    std::vector<DsoTlsDescriptor> dso_modules_;
};

struct RuntimeImportContext
{
    AArch64GuestCall& abi;
    memory::GuestMemory& memory;
    RuntimeState& runtime;
    const RuntimeImportDescriptor& descriptor;
    ImportProvenance provenance;
    ExternalInvocationKind invocation = ExternalInvocationKind::Call;
    const format::ModuleMetadata* module_metadata = nullptr;
};

enum class RuntimeImportOutcomeKind : std::uint8_t
{
    Handled,
    Unimplemented,
    AbiViolation,
    MemoryFault,
    InvariantViolation,
};

[[nodiscard]] std::string_view runtime_import_outcome_name(
    RuntimeImportOutcomeKind kind) noexcept;

struct RuntimeImportOutcome
{
    RuntimeImportOutcomeKind kind = RuntimeImportOutcomeKind::InvariantViolation;
    std::string diagnostic;
    std::optional<Error> error;
    bool abi_validated = false;
    bool abi_signature_known = false;

    [[nodiscard]] static RuntimeImportOutcome handled();
    [[nodiscard]] static RuntimeImportOutcome unimplemented(std::string diagnostic);
    [[nodiscard]] static RuntimeImportOutcome abi_violation(Error error);
    [[nodiscard]] static RuntimeImportOutcome memory_fault(Error error);
    [[nodiscard]] static RuntimeImportOutcome invariant_violation(Error error);
};

using RuntimeImportHandler =
    std::function<Result<RuntimeImportOutcome>(RuntimeImportContext&)>;

class RuntimeImportRegistry
{
  public:
    [[nodiscard]] Result<void> register_import(RuntimeImportDescriptor descriptor,
                                               RuntimeImportHandler handler = {});

    [[nodiscard]] const RuntimeImportDescriptor* find(std::string_view symbol_name) const noexcept;
    [[nodiscard]] std::vector<RuntimeImportDescriptor> descriptors() const;
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    // The registry never exposes a native function pointer. Invocation is
    // only through the controlled context and the typed outcome.
    [[nodiscard]] Result<RuntimeImportOutcome> invoke(RuntimeImportContext& context) const;

  private:
    struct Registration
    {
        RuntimeImportDescriptor descriptor;
        RuntimeImportHandler handler;
    };

    std::map<std::string, Registration, std::less<>> entries_;
};

// This descriptor is intentionally registered without a handler. It records
// the measured call shape and evidence boundary for M12; it does not model
// Nintendo's loader or return a guessed value.
[[nodiscard]] RuntimeImportDescriptor nnmusl_init_dso_descriptor();
[[nodiscard]] Result<void> register_m12_evidence_imports(RuntimeImportRegistry& registry);

} // namespace switchrecomp::runtime
