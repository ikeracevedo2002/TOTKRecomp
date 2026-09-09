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

The ordinary refinement profile uses finite candidate, assessment, stagnation,
and aggregate-analysis budgets. The M27 aggregate dimensions are configured by
the `--refinement-max-analysis-*` options shown by `run-entry --help`:
functions analyzed, functions reanalyzed, instructions, blocks, edges, bytes,
boundary-finalization passes, invalidated records, and immutable transactions.
Zero, overflowed, or otherwise unrepresentable values are rejected.

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
    "max_invalidated_records": 100000,
    "max_transactions": 512
  }
}
```

Each configured value is finite and is reported with its provenance. Generated
execution reports use schema 13 for the aggregate limits, consumption,
typed-exhaustion context, and immutable-generation accounting.
