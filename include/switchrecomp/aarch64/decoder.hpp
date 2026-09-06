#pragma once

#include "switchrecomp/aarch64/instruction.hpp"
#include "switchrecomp/common/result.hpp"
#include "switchrecomp/memory/guest_memory.hpp"

#include <cstdint>
#include <memory>

namespace switchrecomp::aarch64
{

class AArch64Decoder
{
  public:
    AArch64Decoder(const AArch64Decoder&) = delete;
    AArch64Decoder& operator=(const AArch64Decoder&) = delete;
    AArch64Decoder(AArch64Decoder&&) noexcept;
    AArch64Decoder& operator=(AArch64Decoder&&) noexcept;
    ~AArch64Decoder();

    [[nodiscard]] static Result<std::unique_ptr<AArch64Decoder>> create();

#ifndef SWITCHRECOMP_LEGACY_DECODER_IMPL
    [[nodiscard]] Result<DecodedInstruction> decode(GuestAddress address,
                                                     std::uint32_t opcode) const;
#endif

    // Internal compatibility entry retained so the Milestone 9 normalizer can delegate all
    // Milestone 0-8 decoding to the proven implementation without duplicating it.
    [[nodiscard]] Result<DecodedInstruction> decode_legacy(GuestAddress address,
                                                            std::uint32_t opcode) const;

  private:
    struct Impl;

    explicit AArch64Decoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] Result<std::uint32_t> fetch_instruction(const memory::GuestMemory& memory,
                                                      GuestAddress address);

#ifndef SWITCHRECOMP_LEGACY_DECODER_IMPL
[[nodiscard]] Result<DecodedInstruction> fetch_and_decode(const memory::GuestMemory& memory,
                                                          const AArch64Decoder& decoder,
                                                          GuestAddress address);
#endif
[[nodiscard]] Result<DecodedInstruction> fetch_and_decode_legacy(const memory::GuestMemory& memory,
                                                                 const AArch64Decoder& decoder,
                                                                 GuestAddress address);

} // namespace switchrecomp::aarch64
