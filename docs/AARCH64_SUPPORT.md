# Milestone 8 AArch64 support

Milestone 8 expands the Capstone-to-project-owned-decoder-to-Semantic-IR path
without making Capstone types part of the IR. The interpreter is the reference
backend; the optional LLVM backend lowers the same IR primitives.

| Family | Decode | Lift / interpreter | LLVM | Notes |
| --- | --- | --- | --- | --- |
| ADD/SUB, ADDS/SUBS | yes | yes | yes | W/X immediate, shifted and common extended register forms |
| CMP/CMN, NEG/NEGS | yes | yes | yes | NZCV uses ARM carry/no-borrow semantics |
| CCMP/CCMN | yes | no | no | explicit deferred fallback-NZCV semantics |
| AND/ANDS/ORR/ORN/EOR/EON/BIC/BICS/TST | yes | yes | yes | logical flag writes set N/Z and clear C/V |
| MOV/MVN, MOVZ/MOVK/MOVN | yes | yes | yes | W/X aliases and all valid move-wide lanes |
| LSL/LSR/ASR/ROR, UBFM/SBFM/BFM aliases | yes | yes | yes | common immediate bitfield forms |
| CSEL family | yes | yes | yes | CSEL/CSINC/CSINV/CSNEG and common aliases |
| MUL/MADD/MSUB/MNEG | yes | yes | yes | modulo-width integer multiplication |
| LDR/STR scalar | yes | yes | yes | byte/half/word/doubleword, sign/zero extension |
| LDUR/STUR | yes | yes | yes | signed unscaled displacement |
| Register-offset memory | yes | yes | yes | LSL, UXTX/UXTW and SXTX/SXTW-style forms |
| LDP/STP | yes | yes | yes | offset, pre-index and post-index scalar pairs |
| ADR/ADRP/literal LDR | yes | yes | yes | guest-PC and page-relative address domain |
| B/B.cond/CBZ/CBNZ/TBZ/TBNZ | yes | yes | yes | internal CFG targets and taken/not-taken paths |
| BL/BLR/BR/RET | yes | partial | partial | LR and direct/indirect guest targets are explicit; no function dispatcher |
| UDIV/SDIV | yes | no | no | deferred until a shared divide-by-zero model is added |
| Scalar FP: FMOV/FADD/FSUB/FMUL/FDIV/FNEG/FABS/FCMP/FCSEL/SCVTF/UCVTF/FCVTZS/FCVTZU/FCVT/FRINT | yes | yes | yes | S/D forms; raw IEEE bit patterns and sticky FPSR state |
| Scalar FP: FSQRT/FMIN/FMAX | yes | yes | yes | reference runtime semantics, explicit NaN and signed-zero handling |
| NEON DUP/INS/UMOV/SMOV/EXT and ZIP/UZP/TRN | yes | yes | yes | normalized arrangements and lane indices |
| NEON logical/integer/FP vector arithmetic and comparisons | yes | yes | yes | B/H/S/D arrangements; FP vector operations use the reference runtime |
| S/D/Q LDR/STR and LDP/STP | yes | yes | yes | checked guest memory; Q uses 16-byte vector helpers |
| FP/SIMD fused multiply-add, atomics, system instructions | yes | no | no | explicit unsupported behavior |

## Architectural state

`runtime::CpuState` stores X0-X30, SP, PC, independent N/Z/C/V fields, FPCR,
FPSR, and 32 shared 128-bit V registers. S/D views are the low 32/64 bits of
the corresponding V register; scalar writes clear the unused upper bits under
the Milestone 8 scalar policy. `Vector128` is two explicitly ordered 64-bit
words and does not depend on host SIMD types or byte order.
Reading Wn observes the low 32 bits and writing Wn zero-extends into Xn.
XZR/WZR are immutable zero aliases. Register 31 is normalized as SP or ZR by
the decoder operand role rather than globally.

Addition computes unsigned carry and signed overflow independently. Subtraction
uses `C = NOT borrow`; CMP and CMN only write flags. Condition evaluation is
centralized in `runtime::evaluate_condition` and represented in the IR through
`EvaluateCondition`.

## Memory safety

Every guest load/store goes through `GuestMemory` via the runtime ABI. Address
addition is checked for both immediate and register-offset forms. Guest memory
is never exposed as a host pointer to lifted code. Byte and halfword accesses
are typed in the IR as i8 and i16, so extension and truncation are explicit.

## Local coverage workflow

The public repository contains no game data. After legally extracting an NSO
locally, scan either a raw little-endian `.text` image or the NSO itself:

```text
build/aarch64-analyze --coverage module.nso
build/aarch64-analyze --coverage --json module.nso
```

The scanner reports decoded, liftable, unsupported and decode-failure counts,
sorted opcode frequencies, and the first unsupported guest addresses. JSON uses
schema version 1 and only prints the input basename, not a private absolute
path. The tool performs all analysis locally and does not upload or hash-report
the input. XCI/NSP/NCA extraction remains an external local workflow.

Coverage is an instruction-family baseline, not a correctness claim: an
instruction only belongs in the support matrix when its normalized form is
handled by the lifter and both execution backends.
