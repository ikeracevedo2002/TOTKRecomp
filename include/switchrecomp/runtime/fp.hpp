#pragma once

#include <cstdint>
#include <type_traits>

namespace switchrecomp::runtime
{

struct CpuState;

// The guest V registers are represented as two explicitly ordered 64-bit words.
// lo contains bits [63:0] and hi contains bits [127:64], independent of host
// SIMD types or host byte order.
struct Vector128
{
    std::uint64_t lo = 0U;
    std::uint64_t hi = 0U;

    friend bool operator==(const Vector128&, const Vector128&) = default;
};

static_assert(std::is_standard_layout_v<Vector128>);
static_assert(std::is_trivially_copyable_v<Vector128>);
static_assert(sizeof(Vector128) == sizeof(std::uint64_t) * 2U);

enum class FpRoundingMode : std::uint8_t
{
    NearestEven,
    PlusInfinity,
    MinusInfinity,
    TowardZero,
};

enum class FpBinaryOperation : std::uint8_t
{
    Add,
    Sub,
    Mul,
    Div,
    Min,
    Max,
};

enum class FpUnaryOperation : std::uint8_t
{
    Neg,
    Abs,
    Sqrt,
};

enum class FpConversion : std::uint8_t
{
    SignedIntToFp,
    UnsignedIntToFp,
    FpToSignedIntTowardZero,
    FpToUnsignedIntTowardZero,
    Fp32ToFp64,
    Fp64ToFp32,
};

inline constexpr std::uint32_t fpsr_invalid_operation = 1U << 0U;
inline constexpr std::uint32_t fpsr_divide_by_zero = 1U << 1U;
inline constexpr std::uint32_t fpsr_overflow = 1U << 2U;
inline constexpr std::uint32_t fpsr_underflow = 1U << 3U;
inline constexpr std::uint32_t fpsr_inexact = 1U << 4U;
inline constexpr std::uint32_t fpsr_cumulative_saturation = 1U << 27U;

inline constexpr std::uint32_t fpcr_rounding_mask = 0x3U << 22U;
inline constexpr std::uint32_t fpcr_flush_to_zero = 1U << 24U;
inline constexpr std::uint32_t fpcr_default_nan = 1U << 25U;

[[nodiscard]] FpRoundingMode rounding_mode_from_fpcr(std::uint32_t fpcr) noexcept;
[[nodiscard]] std::uint32_t rounding_mode_bits(FpRoundingMode mode) noexcept;

[[nodiscard]] std::uint64_t read_lane_bits(Vector128 value, std::uint8_t element_bits,
                                           std::uint8_t lane) noexcept;
void write_lane_bits(Vector128& value, std::uint8_t element_bits, std::uint8_t lane,
                     std::uint64_t bits) noexcept;
[[nodiscard]] Vector128 broadcast_lane(std::uint64_t bits, std::uint8_t element_bits,
                                       std::uint8_t lane_count) noexcept;
[[nodiscard]] Vector128 clear_unused_lanes(Vector128 value, std::uint8_t element_bits,
                                           std::uint8_t lane_count) noexcept;
[[nodiscard]] Vector128 vector_binary(CpuState& cpu, std::uint8_t operation,
                                      std::uint8_t arrangement, Vector128 left,
                                      Vector128 right) noexcept;
[[nodiscard]] Vector128 vector_compare(CpuState& cpu, std::uint8_t operation,
                                       std::uint8_t arrangement, Vector128 left,
                                       Vector128 right) noexcept;
[[nodiscard]] Vector128 vector_shuffle(std::uint8_t operation, std::uint8_t arrangement,
                                       Vector128 left, Vector128 right,
                                       std::uint8_t immediate) noexcept;

[[nodiscard]] bool is_nan_bits(std::uint64_t bits, std::uint8_t width) noexcept;
[[nodiscard]] bool is_signaling_nan_bits(std::uint64_t bits, std::uint8_t width) noexcept;
[[nodiscard]] bool is_infinity_bits(std::uint64_t bits, std::uint8_t width) noexcept;
[[nodiscard]] bool is_zero_bits(std::uint64_t bits, std::uint8_t width) noexcept;
[[nodiscard]] std::uint64_t quiet_nan_bits(std::uint64_t bits, std::uint8_t width) noexcept;
[[nodiscard]] std::uint64_t default_nan_bits(std::uint8_t width) noexcept;

// All scalar FP entry points consume and return raw IEEE bit patterns. They
// update sticky FPSR bits in the supplied guest state and do not expose the
// host floating-point environment as guest state.
[[nodiscard]] std::uint64_t fp_binary(CpuState& cpu, FpBinaryOperation operation,
                                      std::uint8_t width, std::uint64_t left,
                                      std::uint64_t right) noexcept;
[[nodiscard]] std::uint64_t fp_unary(CpuState& cpu, FpUnaryOperation operation,
                                     std::uint8_t width, std::uint64_t value) noexcept;
[[nodiscard]] std::uint32_t fp_compare(CpuState& cpu, std::uint8_t width,
                                       std::uint64_t left, std::uint64_t right,
                                       bool signaling) noexcept;
[[nodiscard]] std::uint64_t fp_convert(CpuState& cpu, FpConversion conversion,
                                        std::uint8_t source_width, std::uint8_t destination_width,
                                        std::uint64_t value) noexcept;
[[nodiscard]] std::uint64_t fp_round(CpuState& cpu, std::uint8_t width,
                                     std::uint64_t value, FpRoundingMode mode) noexcept;

// Stable C ABI used by the optional LLVM JIT. These wrappers keep all FP/SIMD
// semantics in the reference runtime instead of silently delegating to host
// instructions or host FP environment state.
extern "C"
{
std::uint64_t switchrecomp_runtime_fp_binary(CpuState*, std::uint8_t operation,
                                             std::uint8_t width, std::uint64_t left,
                                             std::uint64_t right) noexcept;
std::uint64_t switchrecomp_runtime_fp_unary(CpuState*, std::uint8_t operation,
                                            std::uint8_t width, std::uint64_t value) noexcept;
std::uint32_t switchrecomp_runtime_fp_compare(CpuState*, std::uint8_t width,
                                              std::uint64_t left, std::uint64_t right,
                                              std::uint8_t signaling) noexcept;
std::uint64_t switchrecomp_runtime_fp_convert(CpuState*, std::uint8_t conversion,
                                              std::uint8_t source_width,
                                              std::uint8_t destination_width,
                                              std::uint64_t value) noexcept;
std::uint64_t switchrecomp_runtime_fp_round(CpuState*, std::uint8_t width,
                                            std::uint64_t value, std::uint8_t mode) noexcept;
std::uint64_t switchrecomp_runtime_vector_extract(const Vector128*, std::uint8_t element_bits,
                                                  std::uint8_t lane) noexcept;
void switchrecomp_runtime_vector_insert(const Vector128*, std::uint8_t element_bits,
                                        std::uint8_t lane, std::uint64_t value,
                                        Vector128*) noexcept;
void switchrecomp_runtime_vector_broadcast(std::uint64_t value, std::uint8_t element_bits,
                                           std::uint8_t lane_count, Vector128*) noexcept;
void switchrecomp_runtime_vector_binary(CpuState*, std::uint8_t operation,
                                        std::uint8_t arrangement, const Vector128*,
                                        const Vector128*, Vector128*) noexcept;
void switchrecomp_runtime_vector_compare(CpuState*, std::uint8_t operation,
                                         std::uint8_t arrangement, const Vector128*,
                                         const Vector128*, Vector128*) noexcept;
void switchrecomp_runtime_vector_shuffle(std::uint8_t operation, std::uint8_t arrangement,
                                         const Vector128*, const Vector128*, std::uint8_t immediate,
                                         Vector128*) noexcept;
}

} // namespace switchrecomp::runtime
