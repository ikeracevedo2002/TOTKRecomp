# Build instructions

TOTKRecomp uses an out-of-source CMake build and requires C++20. A clean
configure fetches the pinned nlohmann/json, Catch2, and LZ4 sources through
CMake `FetchContent`; no game files are downloaded. Capstone v5.0.3 is fetched
at the pinned commit listed in `docs/DEPENDENCIES.md` for the AArch64 decoder.
The LLVM backend is discovered with `find_package(LLVM CONFIG)` and prefers
LLVM 22.1.8. Set `LLVM_DIR` to the installed LLVM CMake package directory to
enable lowering, ORC JIT execution, and native object emission. The core and
interpreter remain buildable with `-DSWITCHRECOMP_ENABLE_LLVM=OFF`.

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
build/aarch64-lift --base 0x1000 --entry 0x1000 path/to/raw-aarch64-code.bin
build/aarch64-lift --print-llvm --base 0x1000 --entry 0x1000 path/to/raw-aarch64-code.bin
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

The normal build never accesses game files and never requires Nintendo keys or
other proprietary content.

LLVM-specific smoke tests and object emission run only when LLVM is found. A
missing LLVM package is reported as `llvm_unavailable`; there is no implicit
interpreter fallback for a requested LLVM operation.
