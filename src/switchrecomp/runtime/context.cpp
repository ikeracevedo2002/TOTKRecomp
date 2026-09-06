#include "switchrecomp/runtime/context.hpp"
#include "switchrecomp/runtime/execution.hpp"

#include "switchrecomp/common/checked_arithmetic.hpp"

#include <array>
#include <cstddef>
#include <new>
#include <span>
#include <string>
#include <utility>

namespace switchrecomp::runtime
{

void RuntimeContext::set_error(Error error) noexcept
{
    try { last_error = std::move(error); has_error = true; }
    catch (const std::bad_alloc&) {
        last_error = Error{ErrorCode::ResourceLimit, "runtime error diagnostic allocation failed"};
        has_error = true;
    }
}

namespace
{
[[nodiscard]] std::uint32_t failure(RuntimeContext* runtime, Error error) noexcept
{
    if (runtime != nullptr) runtime->set_error(std::move(error));
    return 1U;
}
[[nodiscard]] Result<void> validate_size(std::uint8_t size)
{
    if (size != 1U && size != 2U && size != 4U && size != 8U)
        return Result<void>::failure(make_error(ErrorCode::InvalidArgument,
            "guest access size must be 1, 2, 4, or 8"));
    return Result<void>::success();
}
[[nodiscard]] std::uint64_t load_le(std::span<const std::byte> bytes) noexcept
{
    std::uint64_t value = 0U;
    for (std::size_t i = 0; i < bytes.size(); ++i)
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[i])) << (i * 8U);
    return value;
}
void store_le(std::span<std::byte> bytes, std::uint64_t value) noexcept
{
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
}
[[nodiscard]] Result<ir::MemoryOrder> decode_order(std::uint8_t raw)
{
    if (raw > static_cast<std::uint8_t>(ir::MemoryOrder::SequentiallyConsistent))
        return Result<ir::MemoryOrder>::failure(make_error(ErrorCode::InvalidMemoryOrder,
            "runtime received an invalid memory-order value"));
    return Result<ir::MemoryOrder>::success(static_cast<ir::MemoryOrder>(raw));
}
[[nodiscard]] Result<ir::BarrierKind> decode_barrier_kind(std::uint8_t raw)
{
    if (raw > static_cast<std::uint8_t>(ir::BarrierKind::Isb))
        return Result<ir::BarrierKind>::failure(make_error(ErrorCode::InvalidBarrier,
            "runtime received an invalid barrier kind"));
    return Result<ir::BarrierKind>::success(static_cast<ir::BarrierKind>(raw));
}
[[nodiscard]] Result<ir::BarrierOption> decode_barrier_option(std::uint8_t raw)
{
    if (raw > static_cast<std::uint8_t>(ir::BarrierOption::Oshld))
        return Result<ir::BarrierOption>::failure(make_error(ErrorCode::InvalidBarrier,
            "runtime received an invalid barrier option"));
    return Result<ir::BarrierOption>::success(static_cast<ir::BarrierOption>(raw));
}
[[nodiscard]] Result<ir::SystemRegister> decode_system_register(std::uint8_t raw)
{
    if (raw > static_cast<std::uint8_t>(ir::SystemRegister::TpidrroEl0))
        return Result<ir::SystemRegister>::failure(make_error(ErrorCode::UnsupportedSystemRegister,
            "runtime received an unsupported system register"));
    return Result<ir::SystemRegister>::success(static_cast<ir::SystemRegister>(raw));
}
} // namespace

extern "C" std::uint32_t switchrecomp_runtime_guest_load(RuntimeContext* runtime,
    std::uint64_t address, std::uint8_t size, std::uint64_t* result) noexcept
{
    try {
        if (runtime == nullptr || runtime->memory == nullptr || result == nullptr)
            return failure(runtime, make_error(ErrorCode::InvalidRuntimeContext,
                "guest load requires memory, result, and context"));
        const auto valid = validate_size(size); if (!valid) return failure(runtime, valid.error());
        if (runtime->shared != nullptr) {
            const auto loaded = synchronized_load(*runtime->shared, address, size, ir::MemoryOrder::Relaxed);
            if (!loaded) return failure(runtime, loaded.error());
            *result = loaded.value(); return 0U;
        }
        std::array<std::byte, 8> bytes{};
        const auto read = runtime->memory->read(address, std::span<std::byte>(bytes).first(size));
        if (!read) return failure(runtime, read.error());
        *result = load_le(std::span<const std::byte>(bytes).first(size)); return 0U;
    } catch (const std::exception& ex) { return failure(runtime, make_error(ErrorCode::ResourceLimit, ex.what())); }
    catch (...) { return failure(runtime, make_error(ErrorCode::InvalidRuntimeContext, "guest load failed unexpectedly")); }
}

extern "C" std::uint32_t switchrecomp_runtime_guest_store(RuntimeContext* runtime,
    std::uint64_t address, std::uint8_t size, std::uint64_t value) noexcept
{
    try {
        if (runtime == nullptr || runtime->memory == nullptr)
            return failure(runtime, make_error(ErrorCode::InvalidRuntimeContext,
                "guest store requires memory and context"));
        const auto valid = validate_size(size); if (!valid) return failure(runtime, valid.error());
        if (runtime->shared != nullptr) {
            const auto stored = synchronized_store(*runtime->shared, address, size, value, ir::MemoryOrder::Relaxed);
            return stored ? 0U : failure(runtime, stored.error());
        }
        std::array<std::byte, 8> bytes{}; store_le(std::span<std::byte>(bytes).first(size), value);
        const auto write = runtime->memory->write(address, std::span<const std::byte>(bytes).first(size));
        return write ? 0U : failure(runtime, write.error());
    } catch (const std::exception& ex) { return failure(runtime, make_error(ErrorCode::ResourceLimit, ex.what())); }
    catch (...) { return failure(runtime, make_error(ErrorCode::InvalidRuntimeContext, "guest store failed unexpectedly")); }
}

extern "C" std::uint32_t switchrecomp_runtime_guest_load_vector(RuntimeContext* runtime,
    std::uint64_t address, Vector128* result) noexcept
{
    try {
        if (runtime == nullptr || runtime->memory == nullptr || result == nullptr)
            return failure(runtime, make_error(ErrorCode::InvalidRuntimeContext,
                "vector guest load requires memory, result, and context"));
        std::array<std::byte, 16> bytes{};
        const auto read = runtime->shared != nullptr
            ? synchronized_read_bytes(*runtime->shared, address, bytes)
            : runtime->memory->read(address, bytes);
        if (!read) return failure(runtime, read.error());
        result->lo = load_le(std::span<const std::byte>(bytes).first(8U));
        result->hi = load_le(std::span<const std::byte>(bytes).subspan(8U)); return 0U;
    } catch (const std::exception& ex) { return failure(runtime, make_error(ErrorCode::ResourceLimit, ex.what())); }
    catch (...) { return failure(runtime, make_error(ErrorCode::InvalidRuntimeContext, "vector load failed unexpectedly")); }
}

extern "C" std::uint32_t switchrecomp_runtime_guest_store_vector(RuntimeContext* runtime,
    std::uint64_t address, const Vector128* value) noexcept
{
    try {
        if (runtime == nullptr || runtime->memory == nullptr || value == nullptr)
            return failure(runtime, make_error(ErrorCode::InvalidRuntimeContext,
                "vector guest store requires memory, value, and context"));
        std::array<std::byte, 16> bytes{};
        store_le(std::span<std::byte>(bytes).first(8U), value->lo);
        store_le(std::span<std::byte>(bytes).subspan(8U), value->hi);
        const auto write = runtime->shared != nullptr
            ? synchronized_write_bytes(*runtime->shared, address, bytes)
            : runtime->memory->write(address, bytes);
        return write ? 0U : failure(runtime, write.error());
    } catch (const std::exception& ex) { return failure(runtime, make_error(ErrorCode::ResourceLimit, ex.what())); }
    catch (...) { return failure(runtime, make_error(ErrorCode::InvalidRuntimeContext, "vector store failed unexpectedly")); }
}

extern "C" std::uint32_t switchrecomp_runtime_atomic_load(RuntimeContext* runtime,
    std::uint64_t address, std::uint8_t size, std::uint8_t raw_order, std::uint64_t* result) noexcept
{
    try {
        if (runtime == nullptr || runtime->shared == nullptr || result == nullptr)
            return failure(runtime, make_error(ErrorCode::InvalidRuntimeContext,
                "atomic load requires shared runtime state and result"));
        const auto order = decode_order(raw_order); if (!order) return failure(runtime, order.error());
        const auto loaded = synchronized_load(*runtime->shared, address, size, order.value());
        if (!loaded) return failure(runtime, loaded.error());
        *result = loaded.value(); return 0U;
    } catch (const std::exception& ex) { return failure(runtime, make_error(ErrorCode::ResourceLimit, ex.what())); }
    catch (...) { return failure(runtime, make_error(ErrorCode::InvalidRuntimeContext, "atomic load failed unexpectedly")); }
}

extern "C" std::uint32_t switchrecomp_runtime_atomic_store(RuntimeContext* runtime,
    std::uint64_t address, std::uint8_t size, std::uint64_t value, std::uint8_t raw_order) noexcept
{
    try {
        if (runtime == nullptr || runtime->shared == nullptr)
            return failure(runtime, make_error(ErrorCode::InvalidRuntimeContext,
                "atomic store requires shared runtime state"));
        const auto order = decode_order(raw_order); if (!order) return failure(runtime, order.error());
        const auto stored = synchronized_store(*runtime->shared, address, size, value, order.value());
        return stored ? 0U : failure(runtime, stored.error());
    } catch (const std::exception& ex) { return failure(runtime, make_error(ErrorCode::ResourceLimit, ex.what())); }
    catch (...) { return failure(runtime, make_error(ErrorCode::InvalidRuntimeContext, "atomic store failed unexpectedly")); }
}

extern "C" std::uint32_t switchrecomp_runtime_exclusive_load(RuntimeContext* runtime,
    std::uint64_t address, std::uint8_t size, std::uint8_t raw_order, std::uint64_t* result) noexcept
{
    try {
        if (runtime == nullptr || runtime->shared == nullptr || result == nullptr)
            return failure(runtime, make_error(ErrorCode::InvalidRuntimeContext,
                "exclusive load requires shared runtime state and result"));
        const auto order = decode_order(raw_order); if (!order) return failure(runtime, order.error());
        const auto loaded = exclusive_load(*runtime->shared, runtime->exclusive, address, size, order.value());
        if (!loaded) return failure(runtime, loaded.error());
        *result = loaded.value(); return 0U;
    } catch (const std::exception& ex) { return failure(runtime, make_error(ErrorCode::ResourceLimit, ex.what())); }
    catch (...) { return failure(runtime, make_error(ErrorCode::InvalidRuntimeContext, "exclusive load failed unexpectedly")); }
}

extern "C" std::uint32_t switchrecomp_runtime_exclusive_store(RuntimeContext* runtime,
    std::uint64_t address, std::uint8_t size, std::uint64_t value, std::uint8_t raw_order,
    std::uint32_t* status) noexcept
{
    try {
        if (runtime == nullptr || runtime->shared == nullptr || status == nullptr)
            return failure(runtime, make_error(ErrorCode::InvalidRuntimeContext,
                "exclusive store requires shared runtime state and status result"));
        const auto order = decode_order(raw_order); if (!order) return failure(runtime, order.error());
        const auto stored = exclusive_store(*runtime->shared, runtime->exclusive, address, size, value, order.value());
        if (!stored) return failure(runtime, stored.error());
        *status = stored.value(); return 0U;
    } catch (const std::exception& ex) { return failure(runtime, make_error(ErrorCode::ResourceLimit, ex.what())); }
    catch (...) { return failure(runtime, make_error(ErrorCode::InvalidRuntimeContext, "exclusive store failed unexpectedly")); }
}

extern "C" std::uint32_t switchrecomp_runtime_clear_exclusive(RuntimeContext* runtime) noexcept
{
    if (runtime == nullptr) return 1U;
    runtime->exclusive.clear(); return 0U;
}

extern "C" std::uint32_t switchrecomp_runtime_memory_barrier(RuntimeContext* runtime,
    std::uint8_t raw_kind, std::uint8_t raw_option) noexcept
{
    try {
        if (runtime == nullptr) return 1U;
        const auto kind = decode_barrier_kind(raw_kind); if (!kind) return failure(runtime, kind.error());
        const auto option = decode_barrier_option(raw_option); if (!option) return failure(runtime, option.error());
        const auto result = memory_barrier(kind.value(), option.value());
        return result ? 0U : failure(runtime, result.error());
    } catch (...) { return failure(runtime, make_error(ErrorCode::InvalidBarrier, "barrier failed unexpectedly")); }
}

extern "C" std::uint32_t switchrecomp_runtime_read_system_register(RuntimeContext* runtime,
    std::uint8_t raw_reg, std::uint64_t* result) noexcept
{
    if (runtime == nullptr || runtime->cpu == nullptr || result == nullptr)
        return failure(runtime, make_error(ErrorCode::InvalidRuntimeContext,
            "system-register read requires per-thread CpuState"));
    const auto reg = decode_system_register(raw_reg); if (!reg) return failure(runtime, reg.error());
    switch (reg.value()) {
    case ir::SystemRegister::TpidrEl0: *result = runtime->cpu->tpidr_el0; return 0U;
    case ir::SystemRegister::TpidrroEl0: *result = runtime->cpu->tpidrro_el0; return 0U;
    }
    return failure(runtime, make_error(ErrorCode::UnsupportedSystemRegister, "unsupported system-register read"));
}

extern "C" std::uint32_t switchrecomp_runtime_write_system_register(RuntimeContext* runtime,
    std::uint8_t raw_reg, std::uint64_t value) noexcept
{
    if (runtime == nullptr || runtime->cpu == nullptr)
        return failure(runtime, make_error(ErrorCode::InvalidRuntimeContext,
            "system-register write requires per-thread CpuState"));
    const auto reg = decode_system_register(raw_reg); if (!reg) return failure(runtime, reg.error());
    if (reg.value() == ir::SystemRegister::TpidrEl0) { runtime->cpu->tpidr_el0 = value; return 0U; }
    return failure(runtime, make_error(ErrorCode::UnsupportedSystemRegister,
        "TPIDRRO_EL0 is read-only at guest EL0"));
}

extern "C" std::uint32_t switchrecomp_runtime_guest_address_add(RuntimeContext* runtime,
    std::uint64_t base, std::int64_t offset, std::uint64_t* result) noexcept
{
    if (runtime == nullptr || result == nullptr)
        return failure(runtime, make_error(ErrorCode::InvalidRuntimeContext,
            "guest address addition requires context and result"));
    const auto sum = checked_add_signed_u64(base, offset);
    if (!sum) return failure(runtime, sum.error());
    *result = sum.value(); return 0U;
}

extern "C" std::uint32_t switchrecomp_runtime_guest_address_add_value(RuntimeContext* runtime,
    std::uint64_t base, std::uint64_t offset, std::uint8_t signed_offset,
    std::uint64_t* result) noexcept
{
    if (runtime == nullptr || result == nullptr)
        return failure(runtime, make_error(ErrorCode::InvalidRuntimeContext,
            "guest address addition requires context and result"));
    const auto sum = signed_offset != 0U ? checked_add_signed_u64(base, signed_value_from_u64(offset))
                                        : checked_add_u64(base, offset);
    if (!sum) return failure(runtime, sum.error());
    *result = sum.value(); return 0U;
}

extern "C" std::uint32_t switchrecomp_runtime_trap(RuntimeContext* runtime,
    const char* reason) noexcept
{
    return failure(runtime, make_error(ErrorCode::ExecutionTrap,
        std::string("guest execution trapped: ") + (reason == nullptr ? "unknown reason" : reason)));
}

const char* execution_status_name(ExecutionStatus status) noexcept
{
    switch (status) {
    case ExecutionStatus::Returned: return "returned";
    case ExecutionStatus::Trapped: return "trapped";
    case ExecutionStatus::Fault: return "fault";
    case ExecutionStatus::LimitExceeded: return "limit_exceeded";
    }
    return "unknown";
}

} // namespace switchrecomp::runtime
