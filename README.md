# TOTKRecomp

TOTKRecomp is an experimental static-recompilation research project for one exact,
verified build of *The Legend of Zelda: Tears of the Kingdom*. The repository is
split conceptually between reusable `SwitchRecomp` infrastructure and
TOTK-specific target metadata.

## Current status

Milestones 0, 1, 2A, 2B, 3, 4, 5, 6, 7, 8, 9, and 10 are implemented: C++20/CMake build targets, common
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
Milestone 9 adds joinable native guest threads, per-thread CpuState/TLS and
exclusive monitors, synchronized shared guest memory, acquire/release ordering,
barriers, a stable C runtime ABI, and interpreter/LLVM lowering for the
documented atomic subset. Pair-exclusive/LSE forms, WFE/WFI, and Horizon
services remain explicit unsupported boundaries.
Milestone 10 adds bounded whole-module discovery from prepared NSO metadata and
direct-call evidence, a deterministic finalized function map, per-function CFG →
Semantic IR → verifier translation attempts, strict/diagnostic modes, a validated
guest-address dispatcher, import/runtime boundary accounting, and stable
machine-readable/human coverage reports through `translate-module`.
M10.2 tracks precise non-contiguous function ownership, exact ownership
conflicts, boundary-aware function transfers, and honest startup-entry
provenance. The main `.text` start is not treated as a verified process entry;
rtld-led multi-module startup remains deferred.
Milestone 11 adds bounded controlled execution of a metadata-selected main-module
initialization candidate across supported guest function calls and function
transfers. `run-entry` uses deterministic synthetic CPU/stack state, preserves
unresolved imports as runtime boundaries, and reports the first honest stop.
This is not full Switch process startup, does not model rtld or Horizon, and does
not claim that TOTK boots.
Milestone 12 adds an evidence-driven runtime import registry, reusable AArch64
ABI validation, relocation-backed provenance, checked guest-memory handler
contexts, typed runtime outcomes, and transactional DSO/TLS validation. The
real `__nnmusl_init_dso` boundary is recognized but remains explicitly
unimplemented because its contract and provider are not established; no fake
return value is used.

Milestone 13 adds deterministic multi-module process analysis, module-aware
guest symbol-provider discovery, cross-module relocation planning, and
process-aware guest execution. Runtime/HLE import resolution is consulted only
after guest-module provider resolution. The available local executable set is
currently one `main` NSO, so `__nnmusl_init_dso` remains an evidence-driven
unresolved provider search and the real run still stops at the M12
`runtime_import_unimplemented` boundary.
Milestone 14 adds bounded executable-set ingestion, deterministic directory
inventory, identity/coherence validation, provenance-aware completeness, and
schema-4 process inspection. Milestone 15 adds load-order evidence, strict
complete-search gating, provider-base JUMP_SLOT readback, transactional
runtime-handler precedence, and focused real-set execution. The supplied
four-module prepared set identifies an eligible `sdk` provider for
`__nnmusl_init_dso`; its exact-build completeness remains not manifest-verified
and the focused run stops at an unsupported provider `umulh` instruction. No
host replacement is used.

Milestone 32 replaces the ordinary historical candidate-assessment event ceiling
with generation-aware semantic accounting: first assessments are bounded by the
finite structural candidate universe, while reassessment is permitted only after
a relevant immutable map-generation change. An explicitly configured legacy
ceiling remains available and typed. The real four-module workflow crosses the
former `512` assessment frontier and reaches the next exact transaction frontier;
see [Milestone 32](docs/MILESTONE_32.md).

Milestone 33 retains immutable refinement transactions as diagnostic accounting
but removes their historical ordinary event ceiling. Production map construction
proves that every successful reusable-map transaction consumes at least one
boundary-finalization pass, so ordinary termination remains finite through the
semantic aggregate resources. An explicit finite transaction ceiling remains
available for compatibility/debugging, with typed `null`/`not_configured`
reporting when absent; see [Milestone 33](docs/MILESTONE_33.md).

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
./build/translate-module --diagnostic --json --module-base 0x7100000000 \
  --report build/reports/main.json path/to/prepared-main.nso
./build/run-entry --entry dt-init --module-base 0x7100000000 \
  --report build/reports/m12-dt-init.json path/to/prepared-main.nso

# Multi-module process analysis; paths remain local-only.
./build/run-entry --local-config config/local.json --entry dt-init \
  --report build/reports/m13-process.json

# Inspect every prepared module without guest execution; paths remain local-only.
./build/process-inspect --local-config config/local.json --json \
  --report build/reports/m14-process-inventory.json
./build/process-inspect --directory /path/to/prepared-exefs --json
./build/run-entry --local-config config/local.json --entry dt-init \
  --analysis-focus-symbol __nnmusl_init_dso --report build/reports/m15-process.json
```

`translate-module` consumes an already prepared, legally supplied NSO. It
produces auditable function discovery, translation status, unsupported records,
import/runtime boundaries, and coverage; a blocked report is returned with a
non-zero status and is never treated as a successful game translation.

On a multi-config generator, use `build/Debug/nso-inspect`.

## Documentation

- [Architecture RFC](docs/ARCHITECTURE.md)
- [Milestone 3 metadata design](docs/MILESTONE_3.md)
- [Milestone 4 AArch64 analysis design](docs/MILESTONE_4.md)
- [Semantic IR and expanded AArch64 lifting](docs/SEMANTIC_IR.md)
- [Milestone 8 FP/SIMD design and support boundary](docs/MILESTONE_8.md)
- [Milestone 9 threads, TLS, atomics, and memory ordering](docs/MILESTONE_9.md)
- [Milestone 10 whole-main translation](docs/MILESTONE_10.md)
- [Milestone 11 controlled entry execution](docs/MILESTONE_11.md)
- [Milestone 12 runtime import boundary](docs/MILESTONE_12.md)
- [Milestone 13 multi-module provider resolution](docs/MILESTONE_13.md)
- [Milestone 14 complete executable-set ingestion and provider closure](docs/MILESTONE_14.md)
- [Milestone 15 real executable-set closure and `__nnmusl_init_dso`](docs/MILESTONE_15.md)
- [Milestone 19 move-wide semantics and real UMULH frontier](docs/MILESTONE_19.md)
- [Milestone 29 resumable IR execution](docs/MILESTONE_29.md)
- [Milestone 30 generation-scoped controlled stack memory](docs/MILESTONE_30.md)
- [Milestone 31 semantic function-transition frontier](docs/MILESTONE_31.md)
- [Milestone 32 semantic candidate-assessment resource](docs/MILESTONE_32.md)
- [Milestone 33 semantic refinement-transaction resource](docs/MILESTONE_33.md)
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
- joinable native guest threads with stable project-owned IDs and captured
  worker errors;
- per-thread CpuState/TLS and exclusive monitor state, synchronized shared guest
  memory, acquire/release ordering, deterministic 64-byte reservations, and
  DMB/DSB/ISB runtime barriers;
- normalized AArch64 M9 IR, reference interpretation, LLVM 18 helper lowering,
  and decoder-to-backend end-to-end fixtures.
- deterministic whole-module function discovery with provenance, confidence,
  bounded fixed-point growth, boundary-conflict records, and a validated
  finalized function map;
- precise normalized function ownership ranges, exact conflict intersections,
  boundary-aware function transfers, and explicit text-start/initialization
  candidate provenance;
- per-function whole-module translation attempts through the existing CFG,
  Semantic IR, verifier, interpreter-compatible lifter, and optional LLVM 18
  backend, with strict and diagnostic modes;
- a frozen guest-address function dispatcher with checked alignment,
  executable-range, module-ownership, ambiguity, and unknown-target failures;
- versioned deterministic JSON and human-readable module reports covering
  identity, calls, imports, relocations, translation states, semantic families,
  and every unsupported instruction record.

The default materialization limits are 256 MiB per segment and 512 MiB for the
combined `.text`, `.rodata`, `.data`, and BSS buffers. Library callers can pass
smaller or larger limits explicitly; limits are checked before allocation.

ZBIC decoding, REL tables, lazy PLT binding, symbol versioning, pair-exclusive/LSE
atomics, WFE/WFI, Horizon/runtime HLE, renderer, exact TOTK target metadata, and
game execution remain unimplemented. Whole-module analysis and translation are
available for prepared inputs, but do not imply complete ISA coverage or game
boot. LLVM is
optional and is enabled with
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
- Milestone 9 — Native threads, TLS, atomics, barriers, and memory ordering: implemented for the documented synthetic subset.
- Milestone 10 — Whole-main translation: implemented for prepared modules and
  exercised locally against one legally supplied prepared TOTK main NSO with
  bounded diagnostic reporting; real-game boot remains deferred.
- Milestone 11 — Controlled entry-path execution: implemented for bounded
  metadata-selected initialization targets across supported guest function
  boundaries; full process startup and game boot remain deferred.
- Milestone 12 — Evidence-driven runtime import boundary: implemented with
  explicit ABI/provenance/continuation handling and deterministic diagnostics;
  `__nnmusl_init_dso` remains a typed unimplemented boundary pending evidence.
- Milestone 13 — Multi-module guest linking and provider resolution: implemented
  with deterministic process layouts, module-aware provider candidates,
  transactional cross-module relocation planning, process-aware execution, and
  schema-3 reports. The currently supplied one-module local set is explicitly
  incomplete for real `__nnmusl_init_dso` provider discovery, so the M12
  runtime boundary remains the honest next stop.
- Milestone 14 — Complete executable-set ingestion and provider closure:
  infrastructure implemented with schema-4 inventory/inspection, explicit
  completeness provenance, and synthetic cross-module execution coverage.
  Real local evidence is now available, but exact-build manifest verification
  remains open; the guest `sdk` provider is resolved and controlled execution
  stops at an unsupported provider instruction.
- Milestone 15 — Real executable-set closure and `__nnmusl_init_dso`:
  implemented with deterministic load-order evidence, complete-search gating,
  transactional provider-base relocation readback, runtime-fallback precedence,
  and focused real-set reporting. The next blocker is provider instruction
  coverage and, if observed, faithful rtld/process bootstrap.

Materialization consumes a legally obtained, already prepared local NSO. The
repository does not decrypt, extract, or distribute Nintendo content.
