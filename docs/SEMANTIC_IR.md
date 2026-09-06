# Semantic IR and expanded AArch64 lifting

Milestones 6, 7, and 8 provide an executable recompilation path for synthetic,
standalone AArch64 functions, with expanded integer, memory, and control-flow
semantics:

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

The IR now includes orthogonal integer, bitwise, rotate, extension, flag,
selection, checked-address, guest-memory, scalar FP, and 128-bit vector
primitives. It has integer types `i1`, `i8`, `i16`, `i32`, `i64`, floating types
`f32`/`f64`, and `v128`, plus stable `ValueId`/`BlockId` identities, typed instructions, explicit
basic-block terminators, and deterministic textual printing. It does not yet
perform SSA optimization, constant folding, register allocation, or inlining.

FP operations use raw-bit constants and typed `BitCast` instructions. Vector
operations carry an explicit arrangement and lane index; vector values are
never represented as host pointers in the IR. The verifier checks arrangement,
lane width, conversion direction, and operation enum validity before either
backend runs.

Every lifted operation carries a `SourceLocation` containing the guest PC,
original opcode, and (when enabled) the decoder's disassembly. The printer
uses stable vector order and never emits host pointers.

## CPU state and ABI

`runtime::CpuState` contains X0–X30, SP, PC, the four NZCV flags, FPCR, FPSR,
and V0–V31 as 128-bit `Vector128` values. S and D are low-bit views of V; Q
accesses the full value. W reads
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

## Milestone 8 instruction coverage

Only the documented forms are supported. Other forms fail explicitly with an
unsupported-instruction diagnostic containing the guest PC, opcode, and
disassembly.

| Instruction family | Forms supported | Flags | Memory | Status |
| --- | --- | --- | --- | --- |
| NOP | scalar no-operand | no | no | supported |
| MOV/MVN/MOVZ/MOVK/MOVN | W/X aliases, move-wide lanes valid for width | no | no | supported |
| ADD/SUB/ADDS/SUBS | W/X immediate, shifted, and common extended-register forms | NZCV for S forms | no | supported |
| CMP/CMN/NEG/NEGS | register/immediate arithmetic forms | NZCV | no | supported |
| CCMP/CCMN | — | fallback NZCV | no | explicitly deferred |
| AND/ANDS/ORR/ORN/EOR/EON/BIC/BICS/TST | W/X logical forms and aliases | NZ for S/test forms; C/V cleared | no | supported |
| CSEL/CSINC/CSINV/CSNEG aliases | CSET/CSETM/CINC/CINV/CNEG included | no | no | supported |
| LSL/LSR/ASR/ROR and UBFM/SBFM/BFM aliases | immediate shift and common bitfield aliases | no | no | supported |
| MUL/MADD/MSUB/MNEG | modulo-width integer multiply and accumulate | no | no | supported |
| ADR/ADRP | validated guest-PC and page-relative values | no | no | supported |
| LDR/STR scalar | byte/half/word/doubleword; sign extension; base, offset, and writeback forms | no | read/write | supported forms |
| LDP/STP | scalar pairs with offset, pre-index, and post-index forms | no | read/write | supported forms |
| B/B.cond/CBZ/CBNZ/TBZ/TBNZ | internal CFG targets and all normal conditions | reads NZCV where applicable | no | supported |
| BL/BLR/BR/RET | LR write and explicit PC handoff; no general dispatcher yet | no | no | partial |
| Scalar FP and required conversions/rounding | FMOV, FADD/FSUB/FMUL/FDIV, FNEG/FABS/FSQRT, FCMP/FCSEL, SCVTF/UCVTF, FCVTZS/FCVTZU/FCVT, FRINTN/P/M/Z, FMIN/FMAX | FPCR/FPSR runtime state | read/write | supported forms |
| NEON lane/data operations | DUP, INS, UMOV, SMOV, EXT, ZIP/UZP/TRN, logical/integer/FP vector arithmetic and comparisons | no | read/write | supported arrangements |
| FP/SIMD memory | S/D/Q LDR/STR and LDP/STP | no | read/write | checked guest memory |
| FP/SIMD fused multiply-add, atomics, system instructions | — | — | — | explicit rejection |

Literal loads remain supported only when their validated guest target is mapped.
Indirect calls and branches return through the explicit guest `CpuState::pc`
handoff; a full function-pointer dispatcher is deferred.

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
enabled. Milestone 7 adds deterministic instruction fixtures and a whole-range
coverage scanner. The scanner is an instruction-family baseline: operand-form
validation still happens during lifting and unsupported forms remain structured
errors.

Explicitly deferred are BL/BLR/BR function-map dispatch, exclusive atomics and
memory ordering, system-call/Horizon behavior, whole-module
execution, renderer integration, XCI/NSP/NCA extraction, and all TOTK-specific
metadata or patches.
