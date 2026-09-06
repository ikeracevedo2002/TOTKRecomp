# TOTKRecomp

TOTKRecomp is an experimental static-recompilation research project for one exact,
verified build of *The Legend of Zelda: Tears of the Kingdom*. The repository is
split conceptually between reusable `SwitchRecomp` infrastructure and
TOTK-specific target metadata.

## Current status

Milestones 0, 1, 2A, 2B, 3, 4, 5, 6, 7, and 8 are implemented: C++20/CMake build targets, common
bounds-checked binary utilities, SHA-256 validation, logging, versioned
target-manifest validation, strict fixed-size NSO0 parsing, bounded section
materialization, a checked host-backed guest memory map, MOD0/dynamic/RELA
metadata parsing, AArch64 decoding, bounded CFG analysis, synthetic tests, CI,
and deterministic inspection reports are present. Milestone 5 adds bounded dynamic
strings/symbols, semantic AArch64 RELA relocations, deterministic import resolution,
loader-time atomic relocation application, and `nso-dynamic-inspect`. Milestone 6 adds
an LLVM-independent Semantic IR, AArch64 lifting for a documented scalar subset, a
reference interpreter, and an optional LLVM lowering/JIT backend for synthetic functions.
Milestone 7 expands integer, flag, memory, bitfield, conditional-select, and branch
semantics, adds deterministic synthetic coverage fixtures, and provides the
`aarch64-analyze --coverage` report path.
Milestone 8 adds a project-owned AArch64 FP/SIMD state model, scalar IEEE-754
reference semantics, required NEON lane operations, S/D/Q memory forms, and
interpreter/LLVM lowering through the same typed IR.

No supported TOTK build is committed. The repository contains no game binaries,
keys, firmware, SDKs, or extracted game assets. The committed TOTK manifest is an
explicit `template` and contains no real hashes or Build IDs.

The runtime here is limited to the explicit CPU state and guest-memory boundary
needed by synthetic fixtures. Horizon compatibility, renderer, and playable game
support are not implemented.

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
./build/aarch64-analyze --help
./build/aarch64-analyze --version
./build/aarch64-analyze --base 0x1000 --entry 0x1000 path/to/raw-aarch64-code.bin
./build/aarch64-analyze --coverage --json path/to/module.nso
```

On a multi-config generator, use `build/Debug/nso-inspect`.

## Documentation

- [Architecture RFC](docs/ARCHITECTURE.md)
- [Milestone 3 metadata design](docs/MILESTONE_3.md)
- [Milestone 4 AArch64 analysis design](docs/MILESTONE_4.md)
- [Semantic IR and expanded AArch64 lifting](docs/SEMANTIC_IR.md)
- [Milestone 8 FP/SIMD design and support boundary](docs/MILESTONE_8.md)
- [Milestone 9 threads, TLS, atomics, and memory ordering](docs/MILESTONE_9.md)
- [AArch64 support matrix and coverage workflow](docs/AARCH64_SUPPORT.md)
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
- MOD0 discovery from the loaded module-start slot, checked signed relative
  offsets, ELF64 dynamic metadata, bounded RELA/JMPREL parsing, and immutable
  module metadata aggregation;
- deterministic human-readable `nso-inspect` output with materialization status.
- bounded dynamic string and symbol parsing from validated hash-derived table bounds;
- typed AArch64 RELA conversion, generic symbol resolution, explicit unresolved imports,
  and transactional loader-authorized relocation writes;
- deterministic `nso-dynamic-inspect` text and JSON reports.
- Capstone-backed, SwitchRecomp-owned AArch64 instruction decoding with checked
  PC-relative targets, normalized operands, and explicit control-flow status;
- bounded deterministic basic-block/CFG analysis with typed edges, direct call
  candidates, unresolved indirect-flow diagnostics, executable-memory checks,
  and block splitting.
- expanded AArch64 integer, NZCV, conditional-select, bitfield, scalar-memory,
  pair-memory, and test-branch semantics through the project-owned Semantic IR;
- reference interpreter execution and optional LLVM lowering share the same
  typed IR primitives, with deterministic synthetic coverage reports.

The default materialization limits are 256 MiB per segment and 512 MiB for the
combined `.text`, `.rodata`, `.data`, and BSS buffers. Library callers can pass
smaller or larger limits explicitly; limits are checked before allocation.

ZBIC decoding, REL tables, lazy PLT binding, symbol versioning, function-map
dispatch, atomics, Horizon/runtime HLE, renderer, exact TOTK target metadata, and
game execution remain unimplemented. LLVM is optional and is enabled with
`-DTOTKRECOMP_ENABLE_LLVM=ON` when a pinned LLVM installation is available.
`nso-inspect` accepts `--header-only` when a caller needs
to inspect a ZBIC-marked header without claiming materialization succeeded.

## Milestones

- Milestone 0 — repository foundation: implemented.
- Milestone 1 — strict NSO0 inspection: implemented.
- Milestone 2A — NSO image materialization and integrity: implemented.
- Milestone 2B — checked guest memory mappings and NSO guest loader: implemented.
- Milestone 3 — MOD0 and dynamic metadata discovery: implemented.
- Milestone 4 — AArch64 decoding and control-flow analysis: implemented.
- Milestone 5 — Dynamic symbols, import resolution, and AArch64 RELA application: implemented.
- Milestone 6 — Semantic IR and minimal AArch64 lifting: implemented.
- Milestone 7 — Expanded AArch64 semantics and real-code coverage tooling: implemented.
- Milestone 8 — AArch64 FP/SIMD state, semantics, and required vector memory: implemented.

Materialization consumes a legally obtained, already prepared local NSO. The
repository does not decrypt, extract, or distribute Nintendo content.
