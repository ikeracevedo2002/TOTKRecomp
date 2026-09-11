#include "switchrecomp/runtime/dispatcher.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace switchrecomp::runtime
{

namespace
{

[[nodiscard]] std::string hex_address(memory::GuestAddress address)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::setw(16) << std::setfill('0') << address;
    return output.str();
}

[[nodiscard]] bool overlaps(const FunctionRegistration& left,
                            const FunctionRegistration& right) noexcept
{
    return left.range_begin < right.range_end && right.range_begin < left.range_end;
}

} // namespace

Result<void> GuestFunctionRegistry::add(FunctionRegistration registration)
{
    if (frozen_)
    {
        return Result<void>::failure(make_error(
            ErrorCode::FunctionRegistryFrozen, "guest function registry is already frozen"));
    }
    if (memory_ == nullptr || registration.target == nullptr || registration.module.empty())
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument, "function registration is missing memory, module, or target"));
    }
    if ((registration.entry & 0x3U) != 0U || (registration.range_begin & 0x3U) != 0U ||
        registration.range_end <= registration.range_begin || registration.entry < registration.range_begin ||
        registration.entry >= registration.range_end)
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidGuestAddress, "function registration has an invalid guest range"));
    }
    const auto executable = memory_->is_executable(registration.range_begin,
                                                   registration.range_end - registration.range_begin);
    if (!executable)
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidGuestAddress, "function registration cannot validate executable range: " +
                                                executable.error().message));
    }
    if (!executable.value())
    {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidGuestAddress, "function registration range is not executable"));
    }
    for (const auto& current : entries_)
    {
        if (current.entry == registration.entry)
        {
            return Result<void>::failure(make_error(
                ErrorCode::FunctionBoundaryConflict,
                "two guest functions register the same entry " + hex_address(registration.entry)));
        }
        if (overlaps(current, registration))
        {
            return Result<void>::failure(make_error(
                ErrorCode::FunctionBoundaryConflict,
                "function ranges overlap at " + hex_address(registration.entry)));
        }
    }
    entries_.push_back(std::move(registration));
    return Result<void>::success();
}

Result<void> GuestFunctionRegistry::freeze()
{
    if (frozen_)
    {
        return Result<void>::success();
    }
    std::sort(entries_.begin(), entries_.end(), [](const auto& left, const auto& right) {
        return left.entry < right.entry;
    });
    frozen_ = true;
    return Result<void>::success();
}

Result<GuestFunction> GuestFunctionRegistry::lookup(memory::GuestAddress target) const
{
    if (!frozen_)
    {
        return Result<GuestFunction>::failure(make_error(
            ErrorCode::FunctionRegistryFrozen,
            "guest function lookup requires a frozen registry"));
    }
    if ((target & 0x3U) != 0U)
    {
        return Result<GuestFunction>::failure(make_error(
            ErrorCode::InvalidGuestAddress,
            "indirect guest call target " + hex_address(target) + " is not aligned"));
    }
    const auto found = std::lower_bound(
        entries_.begin(), entries_.end(), target,
        [](const auto& entry, memory::GuestAddress address) { return entry.entry < address; });
    if (found == entries_.end() || found->entry != target)
    {
        return Result<GuestFunction>::failure(make_error(
            ErrorCode::UnknownGuestFunction,
            "indirect guest call target " + hex_address(target) +
                " is not a registered function entry"));
    }
    const auto executable = memory_->is_executable(target, 4U);
    if (!executable || !executable.value())
    {
        return Result<GuestFunction>::failure(make_error(
            ErrorCode::InvalidGuestAddress,
            "registered guest function target " + hex_address(target) + " is not executable"));
    }
    return Result<GuestFunction>::success(found->target);
}

Result<std::uint32_t> GuestFunctionRegistry::dispatch(memory::GuestAddress target,
                                                       CpuState& cpu,
                                                       RuntimeContext& runtime) const
{
    const auto function = lookup(target);
    if (!function)
    {
        return Result<std::uint32_t>::failure(function.error());
    }
    runtime.cpu = &cpu;
    return Result<std::uint32_t>::success(function.value()(&cpu, &runtime));
}

} // namespace switchrecomp::runtime
