# TOTKRecomp

TOTKRecomp is an experimental static-recompilation research project for one exact,
verified build of *The Legend of Zelda: Tears of the Kingdom*. The repository is
split conceptually between reusable `SwitchRecomp` infrastructure and
TOTK-specific target metadata.

## Current status

Milestones 0, 1, 2A, and 2B are implemented: C++20/CMake build targets, common
bounds-checked binary utilities, SHA-256 validation, logging, versioned
target-manifest validation, strict fixed-size NSO0 parsing, bounded section
materialization, a checked host-backed guest memory map, synthetic tests, CI,
and the deterministic `nso-inspect` report are present.

No supported TOTK build is committed. The repository contains no game binaries,
keys, firmware, SDKs, or extracted game assets. The committed TOTK manifest is an
explicit `template` and contains no real hashes or Build IDs.

MOD0 parsing, dynamic tables, relocations, the AArch64 decoder, semantic IR,
LLVM lowering, runtime, Horizon compatibility layer, renderer, and playable
game are not implemented.

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

Inspect and materialize an NSO0 input:

```bash
./build/nso-inspect --help
./build/nso-inspect --version
./build/nso-inspect path/to/module.nso
./build/nso-inspect --header-only path/to/module.nso
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
- bounded raw-LZ4 materialization with exact decompressed-size checks;
- mandatory SHA-256 verification when requested by the header;
- explicit zero-filled BSS ownership and configurable allocation limits;
- logical guest-memory mappings with checked 64-bit addresses, R/W/X permissions,
  owned backing storage, bounded reads/writes, and an atomic NSO loader;
- deterministic human-readable `nso-inspect` output with materialization status.

The default materialization limits are 256 MiB per segment and 512 MiB for the
combined `.text`, `.rodata`, `.data`, and BSS buffers. Library callers can pass
smaller or larger limits explicitly; limits are checked before allocation.

ZBIC decoding, MOD0/dynamic-table parsing, relocations, AArch64 decoding, IR,
LLVM, runtime/HLE, renderer, exact TOTK target metadata, and game execution
remain unimplemented. `nso-inspect` accepts `--header-only` when a caller needs
to inspect a ZBIC-marked header without claiming materialization succeeded.

## Milestones

- Milestone 0 — repository foundation: implemented.
- Milestone 1 — strict NSO0 inspection: implemented.
- Milestone 2A — NSO image materialization and integrity: implemented.
- Milestone 2B — checked guest memory mappings and NSO guest loader: implemented.
- Milestone 3 — MOD0 and dynamic metadata discovery: future.
- Milestone 4 — AArch64 decoding: future.

Materialization consumes a legally obtained, already prepared local NSO. The
repository does not decrypt, extract, or distribute Nintendo content.
