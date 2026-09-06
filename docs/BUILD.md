# Build instructions

TOTKRecomp uses an out-of-source CMake build and requires C++20. A clean
configure fetches the pinned nlohmann/json, Catch2, and LZ4 sources through
CMake `FetchContent`; no game files are downloaded. Capstone v5.0.3 is fetched
at the pinned commit listed in `docs/DEPENDENCIES.md` for the AArch64 decoder.

## Configure, build, and test

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

The decoder/CFG CLI accepts a legally obtained raw AArch64 code blob. For a
smoke test without target content, a caller can provide a synthetic four-byte
instruction file and run:

```bash
build/aarch64-analyze --base 0x1000 --entry 0x1000 path/to/raw-aarch64-code.bin
```

Ninja is preferred but not mandatory. With a multi-config generator such as
Visual Studio, select the configuration explicitly:

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

The default build enables tests. Disable them only when needed with
`-DTOTKRECOMP_BUILD_TESTS=OFF`.

For development diagnostics, sanitizers can be enabled on GCC/Clang builds:

```bash
cmake -S . -B build -G Ninja \
  -DTOTKRECOMP_ENABLE_SANITIZERS=ON
```

## Semantic IR and LLVM backend

The core build does not require LLVM:

```bash
cmake -S . -B build -G Ninja -DTOTKRECOMP_ENABLE_LLVM=OFF
cmake --build build
ctest --test-dir build --output-on-failure
build/aarch64-lift --hex 0000018bc0035fd6 --show-disassembly --show-ir --execute-ir
```

For the tested LLVM 18.1.3 integration, install the host LLVM development
package and point CMake at its package directory:

```bash
cmake -S . -B build -G Ninja \
  -DTOTKRECOMP_ENABLE_LLVM=ON \
  -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
cmake --build build
ctest --test-dir build --output-on-failure
build/aarch64-lift --hex 0000018bc0035fd6 \
  --show-disassembly --show-ir --show-llvm --execute-ir --execute-native
```

`aarch64-lift` accepts little-endian AArch64 bytes as hexadecimal pairs. It
does not load or extract XCI/NSP/NCA content. LLVM lowering is isolated in
`switchrecomp-codegen-llvm` and uses ORC `LLJIT`; the generated ABI is
`uint32_t (CpuState*, RuntimeContext*)`.

The normal build never accesses game files and never requires Nintendo keys or
other proprietary content.
