# TOTKRecomp

TOTKRecomp is an experimental static-recompilation research project for one exact,
verified build of *The Legend of Zelda: Tears of the Kingdom*. The repository is
split conceptually between reusable `SwitchRecomp` infrastructure and
TOTK-specific target metadata.

## Current status

The repository bootstrap and Milestone 1 are implemented: C++20/CMake build
targets, common bounds-checked binary utilities, SHA-256 file validation,
logging, versioned target-manifest validation, strict fixed-size NSO0 header
parsing, synthetic parser tests, CI, and the `nso-inspect` header report are
present.

No supported TOTK build is committed. The repository contains no game binaries,
keys, firmware, SDKs, or extracted game assets. The committed TOTK manifest is an
explicit `template` and contains no real hashes or Build IDs.

The NSO parser does not decompress sections or verify section hashes. MOD0
parsing, dynamic tables, relocations, the AArch64 decoder, semantic IR, LLVM
lowering, runtime, Horizon compatibility layer, renderer, and playable game
are not implemented.

## Build and test

Prerequisites:

- CMake 3.20 or newer;
- a C++20 compiler;
- Git for the pinned CMake `FetchContent` dependencies;
- OpenSSL development headers/libraries on non-Windows hosts.

Configure, build, and test with Ninja:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

The default generator also works:

```bash
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Inspect an NSO0 header:

```bash
./build/nso-inspect --help
./build/nso-inspect --version
./build/nso-inspect path/to/module.nso
```

On a multi-config generator, use `build/Debug/nso-inspect`.

## Documentation

- [Architecture RFC](docs/ARCHITECTURE.md)
- [Build notes](docs/BUILD.md)
- [Dependency policy](docs/DEPENDENCIES.md)
- [Target manifests and local configuration](docs/TARGETS.md)

## Legal/content boundary

The project does not distribute or require committed Nintendo copyrighted content.
Use only files you are legally entitled to inspect locally. Do not commit game
binaries, title keys, firmware, proprietary SDKs, Nintendo libraries, or extracted
game assets.

## Implemented after Milestone 1

- strict explicit-little-endian parsing of the fixed `0x100`-byte NSO0 header;
- three segment descriptors, module/build ID, and section hashes;
- compression, hash-required, execute-only, and ZBIC flag decoding;
- checked file-range, memory-range, BSS, and RoData-relative metadata validation;
- deterministic human-readable `nso-inspect` output.

Still not implemented are LZ4/ZBIC decompression, section hash verification,
materialized decompressed sections, MOD0/dynamic-table parsing, relocations,
AArch64 decoding, IR, LLVM, runtime/HLE, renderer, exact TOTK target metadata,
and game execution.

## Next milestone

Milestone 2 is NSO segment materialization and integrity verification, using
controlled decompression, exact-size checks, and optional SHA-256 verification.
