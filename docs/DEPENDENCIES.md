# Dependency policy

Bootstrap dependencies are intentionally limited and are acquired reproducibly
through CMake `FetchContent` or the host toolchain.

| Dependency | Version | Purpose | License | Scope | Acquisition |
| --- | --- | --- | --- | --- | --- |
| nlohmann/json | v3.11.3 | Parse and validate target-manifest JSON | MIT | Build/runtime library | CMake `FetchContent`, pinned tag |
| Catch2 | v3.5.4 | Unit-test framework and CTest discovery | BSL-1.0 | Test-only | CMake `FetchContent`, pinned tag |
| OpenSSL | Host-provided | Cross-platform SHA-256 implementation on non-Windows hosts | Apache-2.0 | Build/runtime library | `find_package(OpenSSL COMPONENTS Crypto)` |
| Windows BCrypt | Windows SDK | SHA-256 implementation on Windows | Microsoft platform SDK terms | Build/runtime library | System API, linked as `bcrypt` |

No LLVM, Capstone, Remill, Unicorn, QEMU, Vulkan, SDL, GLFW, or Nintendo SDK is
used by this milestone. New dependencies must have a clear purpose, a pinned
version or stable platform contract, and a recorded license before being added.

The default build may access the network once to fetch the pinned open-source
dependencies. A subsequent build reuses CMake's dependency source cache; the
project itself does not download game content.
