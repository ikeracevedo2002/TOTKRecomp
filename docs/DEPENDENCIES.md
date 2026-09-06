# Dependency policy

Bootstrap dependencies are intentionally limited and are acquired reproducibly
through CMake `FetchContent` or the host toolchain.

| Dependency | Version | Purpose | License | Scope | Acquisition |
| --- | --- | --- | --- | --- | --- |
| nlohmann/json | v3.11.3 | Parse and validate target-manifest JSON | MIT | Build/runtime library | CMake `FetchContent`, pinned tag |
| Catch2 | v3.5.4 | Unit-test framework and CTest discovery | BSL-1.0 | Test-only | CMake `FetchContent`, pinned tag |
| LZ4 | v1.9.4 (`5ff839680134437dbf4678f3d0c7b371d84f4964`) | Safe decompression of raw NSO section blocks | BSD 2-Clause | `switchrecomp-format` only | CMake `FetchContent`, pinned upstream commit |
| Capstone | v5.0.3 (`5cca00533dadfe53181f1de3525f859769f69b65`) | AArch64 decoding backend only | BSD 3-Clause | `switchrecomp-aarch64` only | CMake `FetchContent`, pinned upstream tag/commit |
| LLVM | 22.1.8 preferred | Semantic-IR lowering, LLVM verification, ORC LLJIT, and host object emission | Apache-2.0 with LLVM Exceptions | `switchrecomp-codegen` only | Host installation via `find_package(LLVM CONFIG)` and `LLVM_DIR` |
| OpenSSL | Host-provided | Cross-platform SHA-256 implementation on non-Windows hosts | Apache-2.0 | Build/runtime library | `find_package(OpenSSL COMPONENTS Crypto)` |
| Windows BCrypt | Windows SDK | SHA-256 implementation on Windows | Microsoft platform SDK terms | Build/runtime library | System API, linked as `bcrypt` |

LZ4 is linked only to `switchrecomp-format`; Capstone is linked only to
`switchrecomp-aarch64` and is used solely to decode one AArch64 instruction at
a time. Capstone types and IDs do not cross the SwitchRecomp public abstraction;
the project owns the normalized instruction, operand, register, and control-flow
models. LLVM is linked only to `switchrecomp-codegen`; it consumes Semantic IR
and does not replace Capstone. The project uses the bounded
`LZ4_decompress_safe` API for raw blocks and does not add zstd for unsupported
ZBIC data. Unicorn, Remill, QEMU, Vulkan, SDL, GLFW, and Nintendo SDKs are not
runtime dependencies. The test-only reference executor is a small independent
raw-opcode harness so normal builds do not require GPL-licensed Unicorn; a
future differential expansion may add Unicorn 2.1.4 behind a dedicated test
option. New dependencies must have a clear purpose, a pinned version or stable
platform contract, and a recorded license before being added.

The default build may access the network once to fetch the pinned open-source
dependencies. A subsequent build reuses CMake's dependency source cache; the
project itself does not download game content.
