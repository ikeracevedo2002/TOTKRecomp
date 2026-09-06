#include "reference_executor.hpp"

#include "switchrecomp/common/checked_arithmetic.hpp"

#include <limits>

namespace switchrecomp::test_reference
{

namespace
{

[[nodiscard]] std::int64_t sign_extend(std::uint64_t value, unsigned int bits) noexcept
{
    const auto sign = std::uint64_t{1} << (bits - 1U);
    const auto mask = (std::uint64_t{1} << bits) - 1U;
    value &= mask;
    const auto signed_value = static_cast<std::int64_t>(value);
    return (value & sign) == 0U
               ? signed_value
               : signed_value - static_cast<std::int64_t>(std::uint64_t{1} << bits);
}

[[nodiscard]] aarch64::Register register_for(std::uint32_t index, bool w) noexcept
{
    return aarch64::Register{aarch64::RegisterKind::General,
                             w ? aarch64::RegisterWidth::W32 : aarch64::RegisterWidth::X64,
                             static_cast<std::uint8_t>(index), false, false};
}

[[nodiscard]] Result<std::uint32_t> instruction_at(std::span<const std::uint32_t> words,
                                                   memory::GuestAddress base,
                                                   memory::GuestAddress pc)
{
    if (pc < base || ((pc - base) / 4U) >= words.size())
    {
        return Result<std::uint32_t>::failure(
            make_error(ErrorCode::OutOfBounds, "reference executor reached an unmapped instruction"));
    }
    return Result<std::uint32_t>::success(words[static_cast<std::size_t>((pc - base) / 4U)]);
}

} // namespace

Result<ReferenceExecutionResult> AArch64ReferenceExecutor::execute(
    std::span<const std::uint32_t> words, memory::GuestAddress base,
    runtime::CpuState state, memory::GuestMemory& memory, std::size_t max_instructions) const
{
    (void)memory;
    if (words.empty() || max_instructions == 0U)
    {
        return Result<ReferenceExecutionResult>::failure(
            make_error(ErrorCode::InvalidArgument, "reference executor requires non-empty code"));
    }
    state.pc = state.pc == 0U ? base : state.pc;
    for (std::size_t step = 0U; step < max_instructions; ++step)
    {
        const auto fetched = instruction_at(words, base, state.pc);
        if (!fetched)
        {
            return Result<ReferenceExecutionResult>::failure(fetched.error());
        }
        const auto opcode = fetched.value();
        const auto pc = state.pc;
        const bool w = (opcode & 0x80000000U) == 0U;
        const auto mask = w ? std::uint64_t{0xffffffffU} : std::numeric_limits<std::uint64_t>::max();
        const auto rd = static_cast<std::uint32_t>(opcode & 0x1fU);
        const auto rn = static_cast<std::uint32_t>((opcode >> 5U) & 0x1fU);
        const auto rm = static_cast<std::uint32_t>((opcode >> 16U) & 0x1fU);
        const auto read = [&](std::uint32_t index) {
            return runtime::read_register(state, register_for(index, w));
        };
        const auto write = [&](std::uint32_t index, std::uint64_t value) {
            return runtime::write_register(state, register_for(index, w), value & mask);
        };
        if ((opcode & 0xfffffc1fU) == 0xd65f0000U)
        {
            return Result<ReferenceExecutionResult>::success(
                ReferenceExecutionResult{state, step + 1U});
        }
        if ((opcode & 0xfc000000U) == 0x14000000U)
        {
            const auto target = checked_add_signed_u64(pc,
                                                        sign_extend(opcode & 0x03ffffffU, 26U) * 4);
            if (!target)
            {
                return Result<ReferenceExecutionResult>::failure(target.error());
            }
            state.pc = target.value();
            continue;
        }
        if ((opcode & 0x7f000000U) == 0x34000000U ||
            (opcode & 0x7f000000U) == 0x35000000U)
        {
            const auto value = read(rn);
            if (!value)
                return Result<ReferenceExecutionResult>::failure(value.error());
            const bool nonzero = (opcode & 0x01000000U) != 0U;
            const bool take = nonzero ? value.value() != 0U : value.value() == 0U;
            if (take)
            {
                const auto target = checked_add_signed_u64(
                    pc, sign_extend((opcode >> 5U) & 0x7ffffU, 19U) * 4);
                if (!target)
                    return Result<ReferenceExecutionResult>::failure(target.error());
                state.pc = target.value();
            }
            else
            {
                const auto next = checked_add_u64(state.pc, 4U);
                if (!next)
                    return Result<ReferenceExecutionResult>::failure(next.error());
                state.pc = next.value();
            }
            continue;
        }
        if ((opcode & 0x1f000000U) == 0x0b000000U)
        {
            const auto left = read(rn);
            const auto right = read(rm);
            if (!left || !right)
                return Result<ReferenceExecutionResult>::failure((!left ? left : right).error());
            const bool subtract = (opcode & 0x40000000U) != 0U;
            const auto result = subtract ? left.value() - right.value() : left.value() + right.value();
            const auto written = write(rd, result);
            if (!written)
                return Result<ReferenceExecutionResult>::failure(written.error());
            const auto next = checked_add_u64(state.pc, 4U);
            if (!next)
                return Result<ReferenceExecutionResult>::failure(next.error());
            state.pc = next.value();
            continue;
        }
        if ((opcode & 0x1f000000U) == 0x11000000U)
        {
            const auto left = read(rn);
            if (!left)
                return Result<ReferenceExecutionResult>::failure(left.error());
            auto immediate = static_cast<std::uint64_t>((opcode >> 10U) & 0xfffU);
            if ((opcode & 0x00400000U) != 0U)
                immediate <<= 12U;
            const bool subtract = (opcode & 0x40000000U) != 0U;
            const auto result = subtract ? left.value() - immediate : left.value() + immediate;
            const auto written = write(rd, result);
            if (!written)
                return Result<ReferenceExecutionResult>::failure(written.error());
            const auto next = checked_add_u64(state.pc, 4U);
            if (!next)
                return Result<ReferenceExecutionResult>::failure(next.error());
            state.pc = next.value();
            continue;
        }
        if ((opcode & 0x1f000000U) == 0x0a000000U)
        {
            const auto left = read(rn);
            const auto right = read(rm);
            if (!left || !right)
                return Result<ReferenceExecutionResult>::failure((!left ? left : right).error());
            std::uint64_t result = 0U;
            switch (opcode & 0x60000000U)
            {
            case 0x00000000U:
                result = left.value() & right.value();
                break;
            case 0x20000000U:
                result = left.value() | right.value();
                break;
            default:
                result = left.value() ^ right.value();
                break;
            }
            const auto written = write(rd, result);
            if (!written)
                return Result<ReferenceExecutionResult>::failure(written.error());
            const auto next = checked_add_u64(state.pc, 4U);
            if (!next)
                return Result<ReferenceExecutionResult>::failure(next.error());
            state.pc = next.value();
            continue;
        }
        if ((opcode & 0x7f800000U) == 0x52800000U)
        {
            const auto shift = ((opcode >> 21U) & 0x3U) * 16U;
            const auto value = static_cast<std::uint64_t>((opcode >> 5U) & 0xffffU) << shift;
            const auto written = write(rd, value);
            if (!written)
                return Result<ReferenceExecutionResult>::failure(written.error());
            const auto next = checked_add_u64(state.pc, 4U);
            if (!next)
                return Result<ReferenceExecutionResult>::failure(next.error());
            state.pc = next.value();
            continue;
        }
        return Result<ReferenceExecutionResult>::failure(make_error(
            ErrorCode::UnsupportedInstruction, "reference executor does not cover opcode"));
    }
    return Result<ReferenceExecutionResult>::failure(make_error(
        ErrorCode::ExecutionStepLimitExceeded, "reference executor exceeded its step limit"));
}

} // namespace switchrecomp::test_reference
