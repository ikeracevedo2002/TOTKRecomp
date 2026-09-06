# Milestone 6 — Semantic IR and minimal AArch64 lifting

## Status

Implemented on `milestone-6-minimal-lifting` from Milestone 5 commit
`378e61ff84b8e5a5fe405e0458e5c587a87ea4bb`.

This milestone is the first executable recompilation slice. It translates synthetic
AArch64 functions through the existing decoder and CFG into a SwitchRecomp-owned
Semantic IR, then executes that IR either with the interpreter or, when LLVM is
installed, through LLVM host code.

## Delivered pipeline

```text
GuestMemory
    ↓
Capstone-backed decoder
    ↓
existing bounded CFG
    ↓
AArch64 semantic lifter
    ↓
IR verifier and deterministic printer
    ├── IR interpreter
    └── LLVM lowering → ORC LLJIT / native object
```

The lifter does not decode bytes a second time, and LLVM never consumes
`DecodedInstruction` directly.

## Scope

Implemented scalar operations are `NOP`, `MOV`, `ADD`, `SUB`, `AND`, `ORR`, `EOR`,
`LDR`, `STR`, `LDUR`, `STUR`, `B`, `CBZ`, `CBNZ`, and standalone `RET`. Arithmetic
wraps modulo the guest operand width. W/X, SP and ZR semantics are centralized in
`CpuState` helpers. Guest memory remains checked and address-domain separated.

The current standalone function ABI is intentionally small: `RET` exits to the
host harness. Calls, NZCV-producing instructions, FP/SIMD, atomics, exceptions,
threads, Horizon, and TOTK execution remain future work.

## Validation

The test suite includes deterministic IR printing, malformed-IR verifier cases,
CPU register rules, arithmetic wraparound, checked memory accesses, branch paths,
unsupported-instruction diagnostics, and a test-only raw AArch64 reference executor
for the synthetic scalar fixtures. The reference executor is independent of both
Capstone and the Semantic IR.

LLVM tests are enabled automatically when a configured LLVM development package is
found. A build without LLVM reports `llvm_unavailable` for LLVM-specific requests;
it never falls back to the interpreter. The preferred LLVM release is 22.1.8.

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

For LLVM, point CMake at the installed package configuration, for example:

```bash
cmake -S . -B build -DLLVM_DIR=/path/to/lib/cmake/llvm
```

The development tool `aarch64-lift` accepts a raw little-endian AArch64 image and
can print Semantic IR, print verified LLVM IR, or emit a host object. No XCI, NSP,
NSO, keys, firmware, SDK, or Nintendo asset is required.
