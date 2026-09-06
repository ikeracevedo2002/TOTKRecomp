# AArch64 support matrix

Milestones 8 and 9 expand the Capstone-to-project-owned-decoder-to-Semantic-IR path
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
| FP/SIMD fused multiply-add | yes | no | no | explicit unsupported behavior |
| LDXR/STXR (B/H/W/X) | yes | yes | yes | per-thread monitor; deterministic 64-byte reservation granules |
| LDAXR/STLXR (B/H/W/X) | yes | yes | yes | acquire load and release store semantics |
| LDAR/STLR (B/H/W/X) | yes | yes | yes | acquire/release ordinary atomic accesses |
| CLREX | yes | yes | yes | clears the current thread's exclusive reservation |
| DMB/DSB/ISB | yes | yes | yes | project-owned barrier kind and option; ISB is an explicit IR boundary |
| MRS/MSR TPIDR_EL0 | yes | yes | yes | maps to per-thread `CpuState` TLS state |
| MRS TPIDRRO_EL0 | yes | yes | yes | read-only per-thread TLS value; writes are rejected |
| LDXP/LDAXP/STXP/STLXP, LSE atomics | yes | no | no | explicitly deferred pair/LSE semantics |

## Architectural state

`runtime::CpuState` stores X0-X30, SP, PC, independent N/Z/C/V fields, FPCR,
FPSR, 32 shared 128-bit V registers, and TPIDR_EL0/TPIDRRO_EL0. Each native
guest thread owns one `CpuState` and one exclusive monitor. S/D views are the
low 32/64 bits of the corresponding V register; scalar writes clear the unused
upper bits under the Milestone 8 scalar policy. `Vector128` is two explicitly
ordered 64-bit words and does not depend on host SIMD types or byte order.
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
Shared and atomic accesses pass through `SharedRuntimeState`, which serializes
the checked guest-memory operation and invalidates overlapping exclusive
reservations. No `atomic_ref` or raw host pointer is used for guest memory.

## Milestone 9 runtime boundary

`GuestThread` maps one guest thread to one joinable native `std::thread`. The
`ThreadManager` assigns stable project-owned IDs and keeps lifecycle state and
thread exceptions observable. `RuntimeContext` carries the current `CpuState`,
shared memory coordinator, TLS identity, and exclusive reservation.

The implemented atomic subset uses project-owned `ir::MemoryOrder` values. A
normal synchronized write invalidates every reservation whose 64-byte granule
overlaps the written range. Invalid widths, alignment, overflow, mapping, and
permission failures remain structured runtime errors. Pair-exclusive and LSE
instructions are decoded and reported as explicit unsupported forms until their
two-register and compare/exchange semantics have their own evidence.

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
