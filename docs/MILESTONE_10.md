# Milestone 10 — Whole-main translation

Milestone 10 integrates the loader, metadata, analysis, Semantic IR, verifier,
interpreter-compatible lifter, optional LLVM backend, and runtime boundaries at
module scale. Its input is an already prepared NSO that the caller is legally
entitled to inspect. Its output is an auditable translation attempt, not a
playable game executable.

## Overview

The implemented pipeline is:

```text
prepared NSO
  → strict NSO0 parse and bounded materialization
  → checked GuestMemory loading
  → MOD0/dynamic metadata, symbols, imports, and RELA
  → executable ranges and initial seeds
  → bounded fixed-point function discovery
  → finalized canonical function map
  → CFG per function
  → AArch64 decode and Semantic IR lifting
  → IR verification
  → optional LLVM 18 lowering
  → deterministic human/JSON report
```

Every discovered function receives a structured translation state. Unsupported
instructions, malformed control flow, unresolved indirect flow, imports, and
runtime boundaries remain visible blockers; none are silently replaced with a
NOP, zero return, host pointer, or fake success.

## Scope

M10 adds:

- `analysis::FunctionMapBuilder` and an immutable `FinalizedFunctionMap`;
- provenance and confidence for function seeds and evidence;
- deterministic fixed-point discovery from module entry, dynamic symbols,
  relocation references, analyst seeds, and validated direct `BL` targets;
- checked analysis budgets for functions, blocks, instructions, edges, seeds,
  and analyzed bytes;
- explicit overlapping-boundary conflicts and per-function diagnostics;
- `analysis::translate_module` orchestration over the existing CFG, lifter,
  Semantic IR, verifier, and optional LLVM backend;
- strict and diagnostic translation modes;
- a frozen sorted guest-address function registry for safe indirect dispatch;
- module identity and coverage metrics, including semantic-family and top-
  unsupported breakdowns;
- the `translate-module` CLI and deterministic JSON/human-readable reports;
- synthetic end-to-end and concurrency tests.

## Non-goals

M10 does not implement:

- game boot or execution of the real TOTK entry path;
- Horizon services, filesystem bring-up, graphics, audio, input, renderer, or
  save data;
- XCI/NSP extraction or decryption, keys, firmware, SDKs, or DRM bypass;
- complete AArch64 coverage, C++ class recovery, or source decompilation;
- pair-exclusive/LSE atomics, WFE/WFI, full exception/unwind behavior, or a
  complete Switch scheduler;
- dynamic module unload/reload or a final linked commercial executable.

Milestone 11 is the first milestone whose goal is entering game initialization.
M10 prepares the function and reporting boundaries needed by M11 but does not
attempt that entry path.

## Architecture

`load_prepared_nso` composes the existing NSO parser, materializer, guest
loader, MOD0 parser, dynamic symbol parser, relocation parser, and transactional
relocation processor. It records a reproducible `ModuleIdentity` containing the
logical module name, NSO module ID, input SHA-256, guest base, executable ranges,
translator version, metadata schema version, LLVM availability, and feature
flags.

The whole-module driver first finalizes ownership in the function map. Codegen
does not discover new functions. Each record is processed independently so that
diagnostics identify the module, stable function ID, guest entry, guest PC, raw
opcode, instruction text, semantic family, failure category, and required next
action.

## Function discovery

The supported seed sources are represented by `FunctionDiscoverySource`:

```text
module_entry, dynamic_symbol, export, direct_call,
relocation_reference, analyst_seed, manual_override,
jump_table, heuristic
```

Each seed also has a `FunctionConfidence`. Automatic direct-call discovery is
`high` confidence, module entries and defined function symbols are `confirmed`,
and manually supplied metadata is represented explicitly rather than silently
overwriting evidence.

The current automatic fixed point is intentionally conservative:

1. validate initial seeds against aligned executable GuestMemory ranges;
2. analyze one seed with the existing bounded CFG analyzer;
3. record direct `BL` calls and unresolved/indirect call sites;
4. add only aligned, executable direct targets as new seeds;
5. repeat until the pending set is empty or a configured budget is reached.

Executable bytes are not scanned as functions merely because they decode as
valid AArch64. Jump-table and heuristic sources are represented for future
metadata providers but are not guessed by this driver. A target outside an
executable range is rejected or retained as a structured CFG failure by the
existing analyzer.

## Canonical function map

`FinalizedFunctionMap` is sorted by canonical guest entry, validated, frozen,
and safe for concurrent read-only access. A record contains:

- module ownership and a stable `sub_<guest-entry>` ID when no symbol name is
  available;
- canonical and additional entries;
- the derived CFG range;
- primary discovery source, confidence, and all evidence;
- CFG, basic blocks, direct calls, indirect call sites, and unresolved flow;
- translation state, unsupported records, and diagnostics.

Overlapping analyzed ranges produce `FunctionBoundaryConflict` records with both
function identities, ranges, sources, and confidence values. The builder never
chooses silently. A canonical entry is always aligned and belongs to a checked
executable mapping.

## Translation pipeline

For every finalized record:

1. use the stored CFG rather than rediscovering ownership;
2. enumerate decoded instructions and record unsupported operations with their
   guest context;
3. lift supported instructions through the existing Semantic IR;
4. run the existing IR verifier;
5. optionally lower the verified IR through the optional LLVM 18 backend;
6. assign a state such as `Translated`, `Unsupported`, `Failed`, `Conflict`, or
   `Excluded`.

`Translated` means the function passed the Semantic IR verifier and all enabled
translation stages. M10 does not add a parallel whole-module LLVM semantic
path. Existing M9 thread, TLS, atomic, barrier, interpreter, and LLVM helper
semantics remain the source of truth. Pair-exclusive instructions are surfaced
as explicit unsupported records until a complete decoder-to-backend chain is
implemented.

Strict mode stops after the first blocking function while preserving the
function map and the blocker. Diagnostic mode continues through the remaining
functions so a full report can be produced. In both modes an unsupported
function is not counted as translated.

## Dispatcher

`runtime::GuestFunctionRegistry` is a small correctness-first boundary for
indirect guest calls. It stores guest addresses and generated-function ABI
targets separately. Registration validates:

- AArch64 alignment;
- non-empty range and entry containment;
- executable GuestMemory ownership;
- non-empty module identity;
- duplicate entries and overlapping ranges.

After `freeze`, lookup uses a sorted binary search and is read-only. It requires
an exact registered function entry, validates executable memory again, and
returns `UnknownGuestFunction` or another structured error for invalid targets.
It never casts a guest address to a host function pointer. Dispatch updates the
runtime CPU context and calls only the registered ABI-compatible target.

## Imports and runtime boundaries

Existing dynamic symbols, defined functions, imports, and RELA/JMPREL records
are carried into the module result and report. Defined function symbols and
relocation references contribute discovery seeds. Unresolved imports remain in
the report and prevent strict success; no implicit runtime implementation is
created for them. Relocation planning distinguishes resolved writes,
unresolved external bindings, and hard relocation failures. For a supported
symbol-backed relocation whose global or weak symbol is undefined, diagnostic
mode retains the relocation index, RELA/JMPREL source, raw and semantic type,
symbol identity, target guest address, and addend as an unresolved boundary.
It does not write zero, an addend, a module base, or any synthetic pointer.
Invalid symbol indices, unknown relocation types, checked-arithmetic failures,
and target permission/range failures remain fatal. The strict low-level
`apply_relocations` API still rejects an unresolved boundary; whole-module
diagnostic loading applies only the resolved portion of a validated plan.

The module result reports both the complete unresolved import list and the
unresolved relocation-binding list, including separate non-PLT and JMPREL
totals. Horizon and other runtime behavior are reported as boundaries by the
existing unsupported/error taxonomy and remain future work.

## Coverage

Coverage is intentionally split into distinct measures rather than one opaque
percentage:

- analysis: executable bytes, decoded instructions, functions discovered,
  functions analyzed, basic blocks, and CFG edges;
- instruction semantics: supported and unsupported instructions, decode
  failures, and counts by semantic family;
- translation: fully lifted, verified, translated, unsupported, failed, and IR
  verification failures;
- calls and boundaries: direct calls, indirect calls, resolved/unresolved
  indirect flow, imports, unresolved relocation bindings, and runtime
  boundaries.

The report also includes a deterministic top-unsupported list with instruction,
semantic family, occurrence count, function count, and example guest PCs. These
numbers are measured per input and are never presented as estimates for a real
TOTK build.

## Reporting

`translate-module` supports:

```text
--strict / --diagnostic
--module-name NAME
--module-base ADDR
--llvm
--no-relocations
--emit-ir
--json
--report PATH
--emit-function-map PATH
--max-functions N
--max-instructions N
--max-blocks N
--max-edges N
--max-seeds N
--max-bytes N
```

JSON output is schema-versioned and ordered through project-owned sorted
containers. It contains no input path, host pointer, timestamp, or random
identifier. The logical module name, build ID, hash, guest addresses, feature
flags, function map, functions, symbols, imports, relocations, and coverage are
the reproducible identity and result surface.

## Strict vs diagnostic behavior

| Condition | Strict mode | Diagnostic mode |
| --- | --- | --- |
| Unsupported instruction | Mark function unsupported and stop | Record it and continue |
| IR verification failure | Mark failed and stop | Record failure and continue |
| Function boundary conflict | Mark conflicting records and stop | Report all conflicts |
| Unresolved indirect flow | Preserve CFG diagnostic; no fake edge | Preserve diagnostic and continue |
| Unresolved import relocation | Prevent strict success | Keep reportable boundary; apply other resolved writes |
| Analysis budget exhaustion | Return `AnalysisBudgetExceeded` | Return `AnalysisBudgetExceeded` |

Diagnostic continuation never upgrades a blocker to a translation success.

## Testing

`tests/milestone10_whole_main_tests.cpp` covers:

- module-entry and direct-`BL` fixed-point discovery;
- duplicate-safe stable function ownership and code/data separation;
- invalid aligned-address checks and analysis budget exhaustion;
- conflicting function ranges;
- deterministic normalized conflict identity, adjacency/containment, exact
  duplicate-range, three-way, chain-overlap, permutation, and late direct-call
  discovery regressions;
- diagnostic relocation planning for resolved relative/symbol-backed values,
  unresolved JumpSlot/GlobDat/Abs64 imports, hard invalid relocation cases,
  transactional writes, and deterministic boundary ordering;
- strict versus diagnostic handling of an unsupported `ldxp` function;
- dispatcher registration, freezing, unknown/misaligned targets, ABI dispatch,
  and concurrent read-only lookup;
- a synthetic NSO that crosses parsing, materialization, GuestMemory, MOD0,
  dynamic metadata, discovery, CFG, lifting, verification, translation, and
  deterministic JSON reporting.

The fixture contains no Nintendo bytes or private paths. LLVM remains optional;
the existing LLVM-enabled CI job exercises the project’s LLVM/JIT tests, while
the M10 orchestration itself remains LLVM-independent unless `--llvm` is
requested.

## Real-module local workflow

After the synthetic suite is green, a developer may analyze a legally supplied,
already prepared local module without adding it to the repository:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DTOTKRECOMP_ENABLE_LLVM=ON
cmake --build build --parallel

build/translate-module --diagnostic --json \
  --module-name main --module-base 0x7100000000 \
  --report build/reports/main.json \
  --emit-function-map build/generated/metadata/main.functions.json \
  path/to/prepared-main.nso
```

Review the module identity, function evidence, conflicts, unsupported records,
indirect calls, imports, runtime boundaries, and top semantic gaps. Keep the
binary and report local unless the report itself is cleared for sharing. A real
module is not required by public CI.

## Real-module stabilization

The M10 stabilization was exercised locally against one legally supplied,
prepared TOTK `main` NSO. The private binary was hash-verified before each
validation phase and was not committed, copied into a fixture, or included in
the repository. Its observed identity was:

- SHA-256: `faf81f8a609ff61a6322d04948132b5929af9f6e2398e0e0ebbe819b8ba3eaab`;
- module/build ID:
  `082ce09b06e33a123cb1e2770f5f9147709033db000000000000000000000000`;
- analysis-selected guest base: `0x7100000000` (not verified as the actual
  TOTK process load address).

With the exact bounded configuration of 5,000 functions, 200,000 instructions,
50,000 blocks, 100,000 edges, 10,000 seeds, and 16 MiB analyzed bytes, both
diagnostic runs reached local report generation. The relocation-enabled run
parsed 504,436 relocations, applied 502,345, and retained 2,091 unresolved
bindings (1,451 non-PLT and 640 JMPREL); 675 unresolved imports were listed.
It discovered/analyzed 613 functions, translated 8, marked 1 unsupported and
604 failed, with 4,437 blocks, 4,958 CFG edges, 126 decoded instructions,
125 lifted instructions, 1 unsupported instruction, 820 direct calls, 675
indirect calls, and 792 unresolved indirect-flow observations. The finalized
map contained 16,103 explicit boundary conflicts involving 604 functions.
No configured analysis budget was exhausted.

`__cxa_pure_virtual` remains unresolved as symbol index 13. The report records
its RELA `R_AARCH64_ABS64` binding at relocation index 502349 and also records
its other unresolved sites; no ABI/runtime implementation was added. The
repeated bounded diagnostic run produced byte-identical `main.json` and
`main.functions.json`. These reports remain local derived artifacts and are
not committed.

Remaining blockers are expected diagnostic boundaries in unsupported
instruction semantics, failed/conflicting function translations, unresolved
indirect flow, and missing runtime/import bindings. This validation does not
claim game execution, boot, a verified process load address, or whole-game
translation.

## Known limitations

- Only the function seed sources currently backed by existing metadata or
  validated direct calls automatically grow the map.
- Indirect targets are recorded, but M10 does not infer arbitrary vtables or
  jump tables without reviewed metadata.
- Overlapping CFG ranges are reported as conflicts and therefore block a clean
  strict result.
- Complete AArch64, system-call, exception, and runtime coverage is not claimed.
- The optional LLVM flag reports a codegen failure when LLVM 18 is unavailable;
  parsing, analysis, Semantic IR, verification, and reports do not require it.
- The real-module reports are local validation artifacts; public CI continues
  to use synthetic fixtures only.

## Deferred work

M11 starts entry-path execution and stops at the first known unsupported runtime
dependency. Later milestones may add runtime bring-up, filesystem, graphics,
renderer, audio, input, broader ISA support, richer analyst metadata, optimized
dispatch, multi-module linking, and performance work. These are deliberately
outside M10’s correctness and auditability boundary.
