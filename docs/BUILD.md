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

## Whole-module translation

Milestone 10 adds `translate-module` for an already prepared, legally supplied
NSO. It emits a deterministic human-readable report by default; add `--json`
for machine-readable output and `--diagnostic` to continue past unsupported
functions.

```bash
build/translate-module --help
build/translate-module --diagnostic --json \
  --module-base 0x7100000000 \
  --report build/reports/main.json \
  path/to/prepared-main.nso
```

The command does not decrypt or extract Nintendo content, and a blocked report
does not claim successful game translation.

The normal build never accesses game files and never requires Nintendo keys or
other proprietary content.

## Indirect-refinement budgets

The ordinary refinement profile uses a finite structural candidate universe,
finite assessment/stagnation budgets, and aggregate-analysis budgets. The
candidate universe is derived from checked aligned executable instruction
slots in the loaded process image and is represented sparsely; it is not an
eager address table. The M27 aggregate dimensions are configured by
the `--refinement-max-analysis-*` options shown by `run-entry --help`:
functions analyzed, functions reanalyzed, instructions, blocks, edges, bytes,
boundary-finalization passes, invalidated records, and transaction accounting.
Ordinary termination does not configure a transaction ceiling: every
successful reusable-map transaction performs at least one boundary-finalization
pass, so `transactions <= boundary_finalization_passes`. Zero, overflowed, or
otherwise unrepresentable explicitly configured values are rejected. The
`--refinement-max-analysis-transactions` option remains an explicitly
configured finite compatibility/debug ceiling when needed. The
legacy `--refinement-max-candidates` option remains an explicit compatibility
ceiling; it is not used by ordinary defaults.

The older `--refinement-max-promotions` and
`--refinement-max-rebuilds` options are retained as explicit deprecated legacy
event guards. They are not charged by the ordinary default profile; passing
either option intentionally enables its old finite guard. The report records
that compatibility mode as `legacy_event_limits`.

Local-only JSON configuration may set aggregate limits without putting private
paths or target data in the repository:

```json
{
  "refinement_analysis": {
    "max_functions_analyzed": 200000,
    "max_functions_reanalyzed": 100000,
    "max_instructions": 8000000,
    "max_blocks": 2000000,
    "max_edges": 4000000,
    "max_bytes_analyzed": 268435456,
    "max_boundary_finalization_passes": 2048,
    "max_invalidated_records": 100000
  }
}
```

An intentional finite transaction compatibility ceiling can be added as
`"max_transactions": N` (or supplied with
`--refinement-max-analysis-transactions N`). Its report provenance is explicit;
when absent, the typed limit is `null` and provenance is `not_configured`.

Each configured value is finite and is reported with its provenance. Generated
execution reports use schema 20 for the structural candidate bound,
legacy-limit distinction, sparse-record accounting, aggregate limits,
consumption, typed-exhaustion context, and immutable-generation accounting.
Schema 20 also reports live versus cumulative guest-memory mapping activity,
generation-scoped controlled-stack ownership/reclamation, and the checked
virtual stack allocation high-water mark. It additionally reports bounded
function-transition forensics: all successful function entries are classified,
while the unchanged `max_function_transitions` value charges only successful
non-call function transfers. The diagnostic history is explicitly bounded by
`max_guest_blocks + 1`. Execution-event accounting is separate: ordinary
execution has no event-count guard, while `--max-events N` remains a positive
finite explicit compatibility/debug guard. Every logical event receives a
checked sequence and increments `event_resource.total_generated`; the
serialized history is bounded by `event_history_limit` using the deterministic
first-prefix plus recent-window policy. The report retains per-kind counts,
omitted-event reconciliation, execution-limit provenance, and a structured
terminal attempt when an explicit event guard is reached. CLI values override
the equivalent local `execution`/`budgets` configuration values.

## Execution slicing

`run-entry` ordinary execution uses a finite internal Semantic IR scheduling
quantum and exact resumable interpreter state. The quantum is not an execution
capacity and is not the ordinary termination guard. The existing finite guest
block, function-transition, call-depth, refinement, and memory resources remain
the aggregate semantic termination model. Event accounting is diagnostic unless
an explicit compatibility guard is configured. `--max-ir-operations N` is an explicit
global hard limit; it is reported separately with explicit CLI provenance and
cannot be bypassed by internal slices. Ordinary BL/BLR call entries are not
charged to the function-transfer resource; call depth, guest blocks, and
refinement limits remain finite independent guards.
