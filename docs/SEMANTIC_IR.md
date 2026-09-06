# Semantic IR and minimal AArch64 lifting

Milestone 6 adds the first executable recompilation path for synthetic,
standalone AArch64 functions:

```text
AArch64 bytes → decoder → CFG → Semantic IR → verifier
                                      ├── interpreter
                                      └── LLVM IR → native JIT
```

This is infrastructure for SwitchRecomp, not a claim that TOTK executes. No
Nintendo files, firmware, keys, SDKs, or game-specific addresses are required
by the tests or tools.

## Why a custom IR exists

The Semantic IR is the architectural contract between guest analysis and
execution backends. It keeps guest registers, flags, guest addresses, memory
accesses, source locations, and unsupported instructions explicit. LLVM is an
optional backend and is not exposed by `switchrecomp-ir`; the interpreter can
therefore validate semantics without LLVM.

The initial IR is intentionally small. It has integer types `i1`, `i8`, `i16`,
`i32`, `i64`, stable `ValueId`/`BlockId` identities, typed instructions, explicit
basic-block terminators, and deterministic textual printing. It does not yet
perform SSA optimization, constant folding, register allocation, or inlining.

Every lifted operation carries a `SourceLocation` containing the guest PC,
original opcode, and (when enabled) the decoder's disassembly. The printer
uses stable vector order and never emits host pointers.

## CPU state and ABI

`runtime::CpuState` contains X0–X30, SP, PC, and the four NZCV flags. W reads
use the low 32 bits; W writes zero the upper half of the corresponding X
register. XZR/WZR always read as zero and discard writes. SP is represented
separately and is never treated as the zero register.

Generated functions use the portable internal ABI:

```cpp
std::uint32_t generated(CpuState* cpu, RuntimeContext* runtime);
```

The guest stack remains a field in `CpuState`; it is not the host call stack.
LLVM-generated memory operations call the C-linkage runtime helpers. A guest
address is checked for overflow and mapping/permission validity before
`GuestMemory` is accessed. Bytes are assembled explicitly as little-endian.

## Verifier and interpreter

`ir::verify` returns the project's structured `Result<void>` errors. It checks
entry blocks, stable IDs, value definitions, operand types and arity, integer
cast widths, register widths, memory sizes, terminators, and branch targets.
Malformed IR is rejected before interpretation or JIT compilation.

The interpreter is the clear reference backend. It enforces a configurable
operation limit, reports guest memory faults and arithmetic/address failures,
and returns execution counters plus the final guest PC on success.

NZCV is represented using primitive IR operations. For ADD/SUB flag-setting
instructions the lifter emits the result, N/Z calculations, and dedicated
carry/overflow primitives. The same formulas are lowered independently by the
interpreter and LLVM backend and are covered by boundary tests.

## Initial instruction coverage

Only the documented forms are supported. Other forms fail explicitly with an
unsupported-instruction diagnostic containing the guest PC, opcode, and
disassembly.

| Instruction | Forms supported | Flags | Memory | Status |
| --- | --- | --- | --- | --- |
| NOP | scalar no-operand | no | no | supported |
| MOV | W/X register or immediate alias | no | no | supported for documented forms |
| MOVZ | W/X, imm16, LSL 0/16/32/48 as valid for width | no | no | supported |
| MOVK | W/X, imm16, LSL 0/16/32/48 as valid for width | no | no | supported |
| ADD/SUB | W/X register or immediate | no | no | supported for documented forms |
| ADDS/SUBS | W/X register or immediate | NZCV | no | supported |
| CMP | register/immediate subtraction form | NZCV | no | supported |
| AND/ORR/EOR | W/X register or immediate | no | no | supported for documented forms |
| ADR | X destination, validated decoder PC-relative value | no | no | supported |
| ADRP | X destination, page-relative decoder value | no | no | supported |
| LDR/STR | W/X, base X/SP plus signed displacement, no writeback | no | read/write | partial |
| LDUR/STUR | W/X, base X/SP plus signed displacement, no writeback | no | read/write | partial |
| B | internal direct CFG target | no | no | supported |
| B.cond | all normal condition codes, internal target | no | no | supported |
| CBZ/CBNZ | W/X register, internal target | no | no | supported |
| RET | standalone function termination | no | no | supported |
| BL/BLR/BR | — | — | — | future / explicit rejection |
| FP/SIMD, atomics, system instructions | — | — | — | explicit rejection |

Loads and stores currently reject literal, register-offset, pre-index, and
post-index forms. This is deliberate: a smaller correct subset is preferable to
silently mis-lifting an addressing mode.

## LLVM backend

The backend is optional and is built as `switchrecomp-codegen-llvm` when CMake
finds LLVM. The tested integration is LLVM 18.1.3 from the Ubuntu Noble
distribution package (`1:18.1.3-1ubuntu1`),
consumed with `find_package(LLVM 18.1.3 CONFIG)` and the `core`, `orcjit`, `native`,
and `support` components. LLVM is not fetched or built by this repository.

```bash
cmake -S . -B build -G Ninja \
  -DTOTKRECOMP_ENABLE_LLVM=ON \
  -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
cmake --build build
ctest --test-dir build --output-on-failure
build/aarch64-lift --hex 0000018bc0035fd6 \
  --show-disassembly --show-ir --show-llvm --execute-ir --execute-native
```

Without LLVM, configure with `-DTOTKRECOMP_ENABLE_LLVM=OFF`. The IR, lifter,
verifier, interpreter, and `aarch64-lift --execute-ir` remain available. The
LLVM backend verifies both generated functions and modules before JIT lookup.
LLVM runtime symbols are explicitly registered with the JIT; no exceptions
cross the generated-function boundary.

## Fixtures and future work

Fixtures use recorded 32-bit AArch64 words and deterministic expected state.
The differential tests compare CPU registers, SP, NZCV, guest memory, and
execution status between the interpreter and LLVM native backend when LLVM is
enabled. The next milestone is **Milestone 7 — Differential Instruction Suite**.

Explicitly deferred are BL/BLR/BR dispatch and function maps, FP/SIMD, exclusive
atomics and memory ordering, system-call/Horizon behavior, whole-module
execution, renderer integration, XCI/NSP/NCA extraction, and all TOTK-specific
metadata or patches.
