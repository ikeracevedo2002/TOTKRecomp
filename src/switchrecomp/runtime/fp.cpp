#include "switchrecomp/runtime/fp.hpp"

#include "switchrecomp/runtime/cpu_state.hpp"

#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <limits>

namespace switchrecomp::runtime
{

namespace
{

[[nodiscard]] constexpr std::uint8_t lane_count(std::uint8_t element_bits) noexcept
{
    return element_bits == 0U || element_bits > 128U ? 0U
                                                        : static_cast<std::uint8_t>(128U / element_bits);
}

[[nodiscard]] constexpr std::uint64_t element_mask(std::uint8_t element_bits) noexcept
{
    return element_bits == 64U ? std::numeric_limits<std::uint64_t>::max()
                               : ((std::uint64_t{1} << element_bits) - 1U);
}

[[nodiscard]] constexpr std::uint64_t exponent_mask(std::uint8_t width) noexcept
{
    return width == 32U ? 0x7f800000U : 0x7ff0000000000000ULL;
}

[[nodiscard]] constexpr std::uint64_t fraction_mask(std::uint8_t width) noexcept
{
    return width == 32U ? 0x007fffffU : 0x000fffffffffffffULL;
}

[[nodiscard]] constexpr std::uint64_t sign_mask(std::uint8_t width) noexcept
{
    return width == 32U ? 0x80000000U : 0x8000000000000000ULL;
}

[[nodiscard]] constexpr std::uint64_t quiet_bit(std::uint8_t width) noexcept
{
    return width == 32U ? 0x00400000U : 0x0008000000000000ULL;
}

[[nodiscard]] bool is_subnormal_bits(std::uint64_t bits, std::uint8_t width) noexcept
{
    return (bits & exponent_mask(width)) == 0U && (bits & fraction_mask(width)) != 0U;
}

[[nodiscard]] int host_rounding(FpRoundingMode mode) noexcept
{
    switch (mode)
    {
    case FpRoundingMode::NearestEven: return FE_TONEAREST;
    case FpRoundingMode::PlusInfinity: return FE_UPWARD;
    case FpRoundingMode::MinusInfinity: return FE_DOWNWARD;
    case FpRoundingMode::TowardZero: return FE_TOWARDZERO;
    }
    return FE_TONEAREST;
}

class ScopedRounding
{
  public:
    explicit ScopedRounding(FpRoundingMode mode) noexcept
        : previous_(std::fegetround()), previous_exceptions_(std::fetestexcept(FE_ALL_EXCEPT))
    {
        (void)std::fesetround(host_rounding(mode));
        (void)std::feclearexcept(FE_ALL_EXCEPT);
    }
    ~ScopedRounding()
    {
        (void)std::feclearexcept(FE_ALL_EXCEPT);
        (void)std::feraiseexcept(previous_exceptions_);
        (void)std::fesetround(previous_);
    }

    [[nodiscard]] bool inexact() const noexcept { return std::fetestexcept(FE_INEXACT) != 0; }

  private:
    int previous_ = FE_TONEAREST;
    int previous_exceptions_ = 0;
};

[[nodiscard]] std::uint64_t canonical_or_quiet_nan(const CpuState& cpu, std::uint8_t width,
                                                   std::uint64_t bits) noexcept
{
    return (cpu.fpcr & fpcr_default_nan) != 0U ? default_nan_bits(width)
                                               : quiet_nan_bits(bits, width);
}

void set_status(CpuState& cpu, std::uint32_t flags) noexcept { cpu.fpsr |= flags; }

[[nodiscard]] std::uint64_t flush_input(CpuState& cpu, std::uint64_t bits,
                                        std::uint8_t width) noexcept
{
    if ((cpu.fpcr & fpcr_flush_to_zero) != 0U && is_subnormal_bits(bits, width))
    {
        set_status(cpu, fpsr_underflow);
        return bits & sign_mask(width);
    }
    return bits;
}

[[nodiscard]] std::uint64_t flush_result(CpuState& cpu, std::uint64_t bits,
                                         std::uint8_t width) noexcept
{
    if ((cpu.fpcr & fpcr_flush_to_zero) != 0U && is_subnormal_bits(bits, width))
    {
        set_status(cpu, fpsr_underflow);
        return bits & sign_mask(width);
    }
    return bits;
}

[[nodiscard]] float as_float(std::uint64_t bits) noexcept
{
    return std::bit_cast<float>(static_cast<std::uint32_t>(bits));
}

[[nodiscard]] double as_double(std::uint64_t bits) noexcept { return std::bit_cast<double>(bits); }

[[nodiscard]] std::uint64_t from_float(float value) noexcept
{
    return std::bit_cast<std::uint32_t>(value);
}

[[nodiscard]] std::uint64_t from_double(double value) noexcept { return std::bit_cast<std::uint64_t>(value); }

[[nodiscard]] std::uint64_t apply_fp_binary(CpuState& cpu, FpBinaryOperation operation,
                                            std::uint8_t width, std::uint64_t left,
                                            std::uint64_t right) noexcept
{
    left = flush_input(cpu, left, width);
    right = flush_input(cpu, right, width);
    if (is_nan_bits(left, width))
    {
        if (is_signaling_nan_bits(left, width)) set_status(cpu, fpsr_invalid_operation);
        return canonical_or_quiet_nan(cpu, width, left);
    }
    if (is_nan_bits(right, width))
    {
        if (is_signaling_nan_bits(right, width)) set_status(cpu, fpsr_invalid_operation);
        return canonical_or_quiet_nan(cpu, width, right);
    }

    const auto left_inf = is_infinity_bits(left, width);
    const auto right_inf = is_infinity_bits(right, width);
    const auto left_zero = is_zero_bits(left, width);
    const auto right_zero = is_zero_bits(right, width);
    if ((operation == FpBinaryOperation::Add || operation == FpBinaryOperation::Sub) &&
        left_inf && right_inf && ((left ^ right) & sign_mask(width)) != 0U)
    {
        set_status(cpu, fpsr_invalid_operation);
        return default_nan_bits(width);
    }
    if (operation == FpBinaryOperation::Mul && ((left_inf && right_zero) || (right_inf && left_zero)))
    {
        set_status(cpu, fpsr_invalid_operation);
        return default_nan_bits(width);
    }
    if (operation == FpBinaryOperation::Div && left_zero && right_zero)
    {
        set_status(cpu, fpsr_invalid_operation);
        return default_nan_bits(width);
    }
    if (operation == FpBinaryOperation::Div && left_inf && right_inf)
    {
        set_status(cpu, fpsr_invalid_operation);
        return default_nan_bits(width);
    }
    if (operation == FpBinaryOperation::Div && right_zero && !left_zero && !left_inf)
    {
        set_status(cpu, fpsr_divide_by_zero);
    }

    const auto mode = rounding_mode_from_fpcr(cpu.fpcr);
    ScopedRounding rounding(mode);
    if (width == 32U)
    {
        const volatile float a = as_float(left);
        const volatile float b = as_float(right);
        volatile float result = 0.0F;
        switch (operation)
        {
        case FpBinaryOperation::Add: result = a + b; break;
        case FpBinaryOperation::Sub: result = a - b; break;
        case FpBinaryOperation::Mul: result = a * b; break;
        case FpBinaryOperation::Div: result = a / b; break;
        case FpBinaryOperation::Min:
        case FpBinaryOperation::Max:
            if (a == b && left_zero && right_zero)
            {
                const auto sign = operation == FpBinaryOperation::Min
                                      ? ((left | right) & sign_mask(width))
                                      : ((left & right) & sign_mask(width));
                result = std::bit_cast<float>(static_cast<std::uint32_t>(sign));
            }
            else
            {
                result = operation == FpBinaryOperation::Min ? (a < b ? a : b) : (a > b ? a : b);
            }
            break;
        }
        const auto bits = from_float(result);
        if (rounding.inexact()) set_status(cpu, fpsr_inexact);
        if (is_infinity_bits(bits, width) && !left_inf && !right_inf) set_status(cpu, fpsr_overflow);
        return flush_result(cpu, bits, width);
    }

    const volatile double a = as_double(left);
    const volatile double b = as_double(right);
    volatile double result = 0.0;
    switch (operation)
    {
    case FpBinaryOperation::Add: result = a + b; break;
    case FpBinaryOperation::Sub: result = a - b; break;
    case FpBinaryOperation::Mul: result = a * b; break;
    case FpBinaryOperation::Div: result = a / b; break;
    case FpBinaryOperation::Min:
    case FpBinaryOperation::Max:
        if (a == b && left_zero && right_zero)
        {
            const auto sign = operation == FpBinaryOperation::Min
                                  ? ((left | right) & sign_mask(width))
                                  : ((left & right) & sign_mask(width));
            result = std::bit_cast<double>(sign);
        }
        else
        {
            result = operation == FpBinaryOperation::Min ? (a < b ? a : b) : (a > b ? a : b);
        }
        break;
    }
    const auto bits = from_double(result);
    if (rounding.inexact()) set_status(cpu, fpsr_inexact);
    if (is_infinity_bits(bits, width) && !left_inf && !right_inf) set_status(cpu, fpsr_overflow);
    return flush_result(cpu, bits, width);
}

} // namespace

FpRoundingMode rounding_mode_from_fpcr(std::uint32_t fpcr) noexcept
{
    switch ((fpcr & fpcr_rounding_mask) >> 22U)
    {
    case 0U: return FpRoundingMode::NearestEven;
    case 1U: return FpRoundingMode::PlusInfinity;
    case 2U: return FpRoundingMode::MinusInfinity;
    case 3U: return FpRoundingMode::TowardZero;
    default: return FpRoundingMode::NearestEven;
    }
}

std::uint32_t rounding_mode_bits(FpRoundingMode mode) noexcept
{
    switch (mode)
    {
    case FpRoundingMode::NearestEven: return 0U;
    case FpRoundingMode::PlusInfinity: return 1U << 22U;
    case FpRoundingMode::MinusInfinity: return 2U << 22U;
    case FpRoundingMode::TowardZero: return 3U << 22U;
    }
    return 0U;
}

std::uint64_t read_lane_bits(Vector128 value, std::uint8_t element_bits, std::uint8_t lane) noexcept
{
    const auto count = lane_count(element_bits);
    if (count == 0U || lane >= count || element_bits > 64U) return 0U;
    const auto bit = static_cast<unsigned int>(lane) * element_bits;
    if (bit < 64U) return (value.lo >> bit) & element_mask(element_bits);
    return (value.hi >> (bit - 64U)) & element_mask(element_bits);
}

void write_lane_bits(Vector128& value, std::uint8_t element_bits, std::uint8_t lane,
                     std::uint64_t bits) noexcept
{
    const auto count = lane_count(element_bits);
    if (count == 0U || lane >= count || element_bits > 64U) return;
    const auto bit = static_cast<unsigned int>(lane) * element_bits;
    const auto mask = element_mask(element_bits);
    if (bit < 64U)
    {
        value.lo = (value.lo & ~(mask << bit)) | ((bits & mask) << bit);
    }
    else
    {
        const auto shifted = bit - 64U;
        value.hi = (value.hi & ~(mask << shifted)) | ((bits & mask) << shifted);
    }
}

Vector128 broadcast_lane(std::uint64_t bits, std::uint8_t element_bits,
                         std::uint8_t lane_count_value) noexcept
{
    Vector128 value{};
    for (std::uint8_t lane = 0U; lane < lane_count_value; ++lane)
        write_lane_bits(value, element_bits, lane, bits);
    return value;
}

Vector128 clear_unused_lanes(Vector128 value, std::uint8_t element_bits,
                             std::uint8_t lane_count_value) noexcept
{
    if (element_bits == 0U || element_bits > 64U || lane_count_value > lane_count(element_bits)) return {};
    const auto used_bits = static_cast<unsigned int>(element_bits) * lane_count_value;
    if (used_bits < 64U)
    {
        value.lo &= used_bits == 0U ? 0U : ((std::uint64_t{1} << used_bits) - 1U);
        value.hi = 0U;
    }
    else if (used_bits < 128U)
    {
        value.hi &= (std::uint64_t{1} << (used_bits - 64U)) - 1U;
    }
    return value;
}

namespace
{

[[nodiscard]] bool arrangement_info(std::uint8_t arrangement, std::uint8_t& bits,
                                    std::uint8_t& count) noexcept
{
    // This numbering mirrors ir::VectorArrangement while keeping the runtime
    // module independent of the IR library.
    switch (arrangement)
    {
    case 1U: bits = 8U; count = 8U; return true;
    case 2U: bits = 8U; count = 16U; return true;
    case 3U: bits = 16U; count = 4U; return true;
    case 4U: bits = 16U; count = 8U; return true;
    case 5U: bits = 32U; count = 2U; return true;
    case 6U: bits = 32U; count = 4U; return true;
    case 7U: bits = 64U; count = 1U; return true;
    case 8U: bits = 64U; count = 2U; return true;
    default: return false;
    }
}

[[nodiscard]] std::int64_t signed_lane(std::uint64_t value, std::uint8_t bits) noexcept
{
    if (bits == 64U) return static_cast<std::int64_t>(value);
    const auto mask = (std::uint64_t{1} << bits) - 1U;
    value &= mask;
    const auto sign = std::uint64_t{1} << (bits - 1U);
    return static_cast<std::int64_t>((value ^ sign) - sign);
}

[[nodiscard]] Vector128 vector_result_mask(std::uint8_t bits, std::uint8_t count,
                                           const Vector128& source) noexcept
{
    return clear_unused_lanes(source, bits, count);
}

} // namespace

Vector128 vector_binary(CpuState& cpu, std::uint8_t operation, std::uint8_t arrangement,
                        Vector128 left, Vector128 right) noexcept
{
    std::uint8_t bits = 0U;
    std::uint8_t count = 0U;
    if (!arrangement_info(arrangement, bits, count)) return {};
    Vector128 result{};
    const auto integer_op = operation <= 6U;
    for (std::uint8_t lane = 0U; lane < count; ++lane)
    {
        const auto a = read_lane_bits(left, bits, lane);
        const auto b = read_lane_bits(right, bits, lane);
        std::uint64_t value = 0U;
        if (operation == 0U) value = a & b;
        else if (operation == 1U) value = a | b;
        else if (operation == 2U) value = a ^ b;
        else if (operation == 3U) value = a & ~b;
        else if (operation == 4U) value = a + b;
        else if (operation == 5U) value = a - b;
        else if (operation == 6U) value = a * b;
        else
        {
            const auto fp_op = operation == 7U   ? FpBinaryOperation::Add
                               : operation == 8U ? FpBinaryOperation::Sub
                               : operation == 9U ? FpBinaryOperation::Mul
                                                 : FpBinaryOperation::Div;
            value = fp_binary(cpu, fp_op, bits, a, b);
        }
        if (integer_op && bits < 64U) value &= element_mask(bits);
        write_lane_bits(result, bits, lane, value);
    }
    return vector_result_mask(bits, count, result);
}

Vector128 vector_compare(CpuState& cpu, std::uint8_t operation, std::uint8_t arrangement,
                         Vector128 left, Vector128 right) noexcept
{
    std::uint8_t bits = 0U;
    std::uint8_t count = 0U;
    if (!arrangement_info(arrangement, bits, count)) return {};
    Vector128 result{};
    for (std::uint8_t lane = 0U; lane < count; ++lane)
    {
        const auto a = read_lane_bits(left, bits, lane);
        const auto b = read_lane_bits(right, bits, lane);
        bool equal = false;
        bool greater = false;
        bool greater_equal = false;
        if (operation >= 5U)
        {
            const auto status = fp_compare(cpu, bits, a, b, false);
            equal = status == 0b0110U;
            greater = status == 0b0010U;
            greater_equal = equal || greater;
        }
        else if (operation == 0U)
        {
            equal = a == b;
        }
        else if (operation == 1U || operation == 2U)
        {
            greater = signed_lane(a, bits) > signed_lane(b, bits);
            greater_equal = signed_lane(a, bits) >= signed_lane(b, bits);
        }
        else
        {
            greater = a > b;
            greater_equal = a >= b;
        }
        const bool selected = operation == 0U || operation == 5U ? equal
                            : operation == 1U || operation == 3U || operation == 6U ? greater
                                                                                    : greater_equal;
        write_lane_bits(result, bits, lane, selected ? element_mask(bits) : 0U);
    }
    return vector_result_mask(bits, count, result);
}

Vector128 vector_shuffle(std::uint8_t operation, std::uint8_t arrangement, Vector128 left,
                         Vector128 right, std::uint8_t immediate) noexcept
{
    std::uint8_t bits = 0U;
    std::uint8_t count = 0U;
    if (!arrangement_info(arrangement, bits, count)) return {};
    if (operation == 0U) // EXT: byte concatenation, immediate is a byte offset.
    {
        Vector128 result{};
        for (std::uint8_t index = 0U; index < 16U; ++index)
        {
            const auto source_index = static_cast<unsigned int>(index) + immediate;
            const auto byte = source_index < 16U ? read_lane_bits(left, 8U, static_cast<std::uint8_t>(source_index))
                                                 : read_lane_bits(right, 8U, static_cast<std::uint8_t>(source_index - 16U));
            write_lane_bits(result, 8U, index, byte);
        }
        return result;
    }
    Vector128 result{};
    const auto half = static_cast<std::uint8_t>(count / 2U);
    if (half == 0U) return result;
    for (std::uint8_t out = 0U; out < count; ++out)
    {
        std::uint8_t source_lane = 0U;
        bool from_right = false;
        if (operation == 1U || operation == 2U) // ZIP1/ZIP2
        {
            source_lane = static_cast<std::uint8_t>((operation == 2U ? half : 0U) + out / 2U);
            from_right = (out & 1U) != 0U;
        }
        else if (operation == 3U || operation == 4U) // UZP1/UZP2
        {
            source_lane = static_cast<std::uint8_t>(2U * (out % half) + (operation == 4U ? 1U : 0U));
            from_right = out >= half;
        }
        else // TRN1/TRN2
        {
            source_lane = static_cast<std::uint8_t>(2U * (out / 2U) + (operation == 6U ? 1U : 0U));
            from_right = (out & 1U) != 0U;
        }
        write_lane_bits(result, bits, out,
                        read_lane_bits(from_right ? right : left, bits, source_lane));
    }
    return vector_result_mask(bits, count, result);
}

bool is_nan_bits(std::uint64_t bits, std::uint8_t width) noexcept
{
    return (bits & exponent_mask(width)) == exponent_mask(width) &&
           (bits & fraction_mask(width)) != 0U;
}

bool is_signaling_nan_bits(std::uint64_t bits, std::uint8_t width) noexcept
{
    return is_nan_bits(bits, width) && (bits & quiet_bit(width)) == 0U;
}

bool is_infinity_bits(std::uint64_t bits, std::uint8_t width) noexcept
{
    return (bits & (exponent_mask(width) | fraction_mask(width))) == exponent_mask(width);
}

bool is_zero_bits(std::uint64_t bits, std::uint8_t width) noexcept
{
    return (bits & ~sign_mask(width)) == 0U;
}

std::uint64_t quiet_nan_bits(std::uint64_t bits, std::uint8_t width) noexcept
{
    return (bits | quiet_bit(width)) & (width == 32U ? 0xffffffffU : std::numeric_limits<std::uint64_t>::max());
}

std::uint64_t default_nan_bits(std::uint8_t width) noexcept
{
    return width == 32U ? 0x7fc00000U : 0x7ff8000000000000ULL;
}

std::uint64_t fp_binary(CpuState& cpu, FpBinaryOperation operation, std::uint8_t width,
                        std::uint64_t left, std::uint64_t right) noexcept
{
    return apply_fp_binary(cpu, operation, width, left, right);
}

std::uint64_t fp_unary(CpuState& cpu, FpUnaryOperation operation, std::uint8_t width,
                       std::uint64_t value) noexcept
{
    value = flush_input(cpu, value, width);
    if (is_nan_bits(value, width))
    {
        if (is_signaling_nan_bits(value, width)) set_status(cpu, fpsr_invalid_operation);
        return canonical_or_quiet_nan(cpu, width, value);
    }
    if (operation == FpUnaryOperation::Sqrt && (value & sign_mask(width)) != 0U && !is_zero_bits(value, width))
    {
        set_status(cpu, fpsr_invalid_operation);
        return default_nan_bits(width);
    }
    ScopedRounding rounding(rounding_mode_from_fpcr(cpu.fpcr));
    if (width == 32U)
    {
        const auto input = as_float(value);
        volatile float result = operation == FpUnaryOperation::Neg ? -input
                                : operation == FpUnaryOperation::Abs ? std::fabs(input)
                                                                      : std::sqrt(input);
        if (rounding.inexact()) set_status(cpu, fpsr_inexact);
        return flush_result(cpu, from_float(result), width);
    }
    const auto input = as_double(value);
    volatile double result = operation == FpUnaryOperation::Neg ? -input
                                 : operation == FpUnaryOperation::Abs ? std::fabs(input)
                                                                       : std::sqrt(input);
    if (rounding.inexact()) set_status(cpu, fpsr_inexact);
    return flush_result(cpu, from_double(result), width);
}

std::uint32_t fp_compare(CpuState& cpu, std::uint8_t width, std::uint64_t left,
                         std::uint64_t right, bool signaling) noexcept
{
    left = flush_input(cpu, left, width);
    right = flush_input(cpu, right, width);
    if (is_nan_bits(left, width) || is_nan_bits(right, width))
    {
        if (signaling || is_signaling_nan_bits(left, width) || is_signaling_nan_bits(right, width))
            set_status(cpu, fpsr_invalid_operation);
        // unordered: N=0, Z=0, C=1, V=1
        return 0b0011U;
    }
    const auto a = width == 32U ? static_cast<double>(as_float(left)) : as_double(left);
    const auto b = width == 32U ? static_cast<double>(as_float(right)) : as_double(right);
    if (a == b) return 0b0110U; // Z=1, C=1
    if (a < b) return 0b1000U;  // N=1
    return 0b0010U;             // C=1
}

std::uint64_t fp_convert(CpuState& cpu, FpConversion conversion, std::uint8_t source_width,
                         std::uint8_t destination_width, std::uint64_t value) noexcept
{
    if (conversion == FpConversion::Fp32ToFp64)
    {
        value = flush_input(cpu, value, 32U);
        return flush_result(cpu, from_double(static_cast<double>(as_float(value))), 64U);
    }
    if (conversion == FpConversion::Fp64ToFp32)
    {
        value = flush_input(cpu, value, 64U);
        ScopedRounding rounding(rounding_mode_from_fpcr(cpu.fpcr));
        volatile float converted = static_cast<float>(as_double(value));
        const auto result = from_float(converted);
        if (rounding.inexact()) set_status(cpu, fpsr_inexact);
        return flush_result(cpu, result, 32U);
    }

    if (conversion == FpConversion::SignedIntToFp || conversion == FpConversion::UnsignedIntToFp)
    {
        const auto source = conversion == FpConversion::SignedIntToFp
                                ? static_cast<double>(source_width == 32U
                                                          ? std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value))
                                                          : std::bit_cast<std::int64_t>(value))
                                : static_cast<double>(source_width == 32U
                                                          ? static_cast<std::uint32_t>(value)
                                                          : value);
        ScopedRounding rounding(rounding_mode_from_fpcr(cpu.fpcr));
        std::uint64_t result = 0U;
        if (destination_width == 32U)
        {
            volatile float converted = static_cast<float>(source);
            result = from_float(converted);
        }
        else
        {
            volatile double converted = source;
            result = from_double(converted);
        }
        if (rounding.inexact()) set_status(cpu, fpsr_inexact);
        return flush_result(cpu, result, destination_width);
    }

    value = flush_input(cpu, value, source_width);
    if (is_nan_bits(value, source_width) || is_infinity_bits(value, source_width))
    {
        set_status(cpu, fpsr_invalid_operation);
        return conversion == FpConversion::FpToSignedIntTowardZero ?
                   (destination_width == 32U ? 0x80000000U : 0x8000000000000000ULL) :
                   std::numeric_limits<std::uint64_t>::max();
    }
    const auto input = source_width == 32U ? static_cast<long double>(as_float(value))
                                           : static_cast<long double>(as_double(value));
    const auto signed_conversion = conversion == FpConversion::FpToSignedIntTowardZero;
    const auto rounded = std::trunc(input);
    const long double min_value = signed_conversion
                                      ? (destination_width == 32U ? -2147483648.0L : -9223372036854775808.0L)
                                      : 0.0L;
    const long double max_value = signed_conversion
                                      ? (destination_width == 32U ? 2147483647.0L : 9223372036854775807.0L)
                                      : (destination_width == 32U ? 4294967295.0L : 18446744073709551615.0L);
    if (rounded < min_value || rounded > max_value)
    {
        set_status(cpu, fpsr_invalid_operation);
        return signed_conversion ? (destination_width == 32U ? 0x80000000U : 0x8000000000000000ULL)
                                  : (destination_width == 32U ? 0xffffffffU : std::numeric_limits<std::uint64_t>::max());
    }
    const auto unsigned_from_integral = [](long double integral) noexcept {
        constexpr long double base = 4294967296.0L;
        const auto high = static_cast<std::uint64_t>(std::floor(integral / base));
        const auto low = static_cast<std::uint64_t>(integral - static_cast<long double>(high) * base);
        return (high << 32U) | low;
    };
    if (signed_conversion)
    {
        if (rounded < 0.0L)
        {
            return 0U - unsigned_from_integral(-rounded);
        }
        return unsigned_from_integral(rounded);
    }
    return unsigned_from_integral(rounded);
}

std::uint64_t fp_round(CpuState& cpu, std::uint8_t width, std::uint64_t value,
                       FpRoundingMode mode) noexcept
{
    (void)cpu;
    value = flush_input(cpu, value, width);
    if (is_nan_bits(value, width) || is_infinity_bits(value, width)) return value;
    ScopedRounding rounding(mode);
    if (width == 32U)
    {
        volatile float result = std::nearbyint(as_float(value));
        if (from_float(result) != static_cast<std::uint32_t>(value)) set_status(cpu, fpsr_inexact);
        return flush_result(cpu, from_float(result), width);
    }
    volatile double result = std::nearbyint(as_double(value));
    if (from_double(result) != value) set_status(cpu, fpsr_inexact);
    return flush_result(cpu, from_double(result), width);
}

extern "C"
{
std::uint64_t switchrecomp_runtime_fp_binary(CpuState* cpu, std::uint8_t operation,
                                             std::uint8_t width, std::uint64_t left,
                                             std::uint64_t right) noexcept
{
    return cpu == nullptr ? 0U : fp_binary(*cpu, static_cast<FpBinaryOperation>(operation), width, left, right);
}

std::uint64_t switchrecomp_runtime_fp_unary(CpuState* cpu, std::uint8_t operation,
                                            std::uint8_t width, std::uint64_t value) noexcept
{
    return cpu == nullptr ? 0U : fp_unary(*cpu, static_cast<FpUnaryOperation>(operation), width, value);
}

std::uint32_t switchrecomp_runtime_fp_compare(CpuState* cpu, std::uint8_t width,
                                              std::uint64_t left, std::uint64_t right,
                                              std::uint8_t signaling) noexcept
{
    return cpu == nullptr ? 0U : fp_compare(*cpu, width, left, right, signaling != 0U);
}

std::uint64_t switchrecomp_runtime_fp_convert(CpuState* cpu, std::uint8_t conversion,
                                              std::uint8_t source_width,
                                              std::uint8_t destination_width,
                                              std::uint64_t value) noexcept
{
    return cpu == nullptr ? 0U : fp_convert(*cpu, static_cast<FpConversion>(conversion), source_width,
                                             destination_width, value);
}

std::uint64_t switchrecomp_runtime_fp_round(CpuState* cpu, std::uint8_t width,
                                            std::uint64_t value, std::uint8_t mode) noexcept
{
    return cpu == nullptr ? value : fp_round(*cpu, width, value, static_cast<FpRoundingMode>(mode));
}

std::uint64_t switchrecomp_runtime_vector_extract(const Vector128* value,
                                                  std::uint8_t element_bits,
                                                  std::uint8_t lane) noexcept
{
    return value == nullptr ? 0U : read_lane_bits(*value, element_bits, lane);
}

void switchrecomp_runtime_vector_insert(const Vector128* source, std::uint8_t element_bits,
                                        std::uint8_t lane, std::uint64_t value,
                                        Vector128* result) noexcept
{
    if (result == nullptr) return;
    *result = source == nullptr ? Vector128{} : *source;
    write_lane_bits(*result, element_bits, lane, value);
}

void switchrecomp_runtime_vector_broadcast(std::uint64_t value, std::uint8_t element_bits,
                                           std::uint8_t lane_count_value, Vector128* result) noexcept
{
    if (result != nullptr) *result = broadcast_lane(value, element_bits, lane_count_value);
}

void switchrecomp_runtime_vector_binary(CpuState* cpu, std::uint8_t operation,
                                        std::uint8_t arrangement, const Vector128* left,
                                        const Vector128* right, Vector128* result) noexcept
{
    if (cpu == nullptr || left == nullptr || right == nullptr || result == nullptr) return;
    *result = vector_binary(*cpu, operation, arrangement, *left, *right);
}

void switchrecomp_runtime_vector_compare(CpuState* cpu, std::uint8_t operation,
                                         std::uint8_t arrangement, const Vector128* left,
                                         const Vector128* right, Vector128* result) noexcept
{
    if (cpu == nullptr || left == nullptr || right == nullptr || result == nullptr) return;
    *result = vector_compare(*cpu, operation, arrangement, *left, *right);
}

void switchrecomp_runtime_vector_shuffle(std::uint8_t operation, std::uint8_t arrangement,
                                         const Vector128* left, const Vector128* right,
                                         std::uint8_t immediate, Vector128* result) noexcept
{
    if (left == nullptr || right == nullptr || result == nullptr) return;
    *result = vector_shuffle(operation, arrangement, *left, *right, immediate);
}
}

} // namespace switchrecomp::runtime
