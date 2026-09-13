# AArch64 support matrix

Milestones 8 and 9 expand the Capstone-to-project-owned-decoder-to-Semantic-IR path
without making Capstone types part of the IR. The interpreter is the reference
backend; the optional LLVM backend lowers the same IR primitives.

| Family | Decode | Lift / interpreter | LLVM | Notes |
| --- | --- | --- | --- | --- |
| ADD/SUB, ADDS/SUBS | yes | yes | yes | W/X immediate, shifted and common extended register forms |
| CMP/CMN, NEG/NEGS | yes | yes | yes | NZCV uses ARM carry/no-borrow semantics |
| CCMP/CCMN | yes | yes | yes | NZCV from the comparison, conditionally selected against the fallback immediate using the incoming condition flags |
| AND/ANDS/ORR/ORN/EOR/EON/BIC/BICS/TST | yes | yes | yes | logical flag writes set N/Z and clear C/V |
| MOV/MVN, MOVZ/MOVK/MOVN | yes | yes | yes | W/X aliases and all valid move-wide lanes |
| LSL/LSR/ASR/ROR, UBFM/SBFM/BFM aliases | yes | yes | yes | W/X immediate and register-controlled shifts; width-masked amounts and wrapped bitfield masks |
| EXTR | yes | yes | yes | W/X double-register extract with architectural immediate range checks |
| CSEL family | yes | yes | yes | CSEL/CSINC/CSINV/CSNEG and common aliases |
| MUL/MADD/MSUB/MNEG | yes | yes | yes | modulo-width integer multiplication |
| ADC/ADCS/SBC/SBCS/NGC/NGCS | yes | yes | yes | architectural carry-in, no-borrow carry-out, and signed overflow |
| UMADDL/UMSUBL/SMADDL/SMSUBL | yes | yes | yes | W×W long multiply with X accumulation; signed forms sign-extend operands |
| UMULH | yes | yes | yes | scalar A64 unsigned high multiply; i64/X-register form only; NZCV unchanged |
| CRC32/CRC32C | yes | yes | yes | B/H/W/X reflected CRC forms with architectural polynomials |
| PRFM/PRFUM | yes | yes | yes | non-faulting architectural prefetch hints are explicit no-ops |
| REV/REV16 scalar | yes | yes | yes | byte reversal for W/X and per-halfword reversal; vector REV forms remain separate |
| LDR/STR scalar | yes | yes | yes | byte/half/word/doubleword, sign/zero extension |
| LDUR/STUR | yes | yes | yes | signed unscaled displacement |
| Register-offset memory | yes | yes | yes | LSL, UXTX/UXTW and SXTX/SXTW-style forms |
| LDP/STP | yes | yes | yes | offset, pre-index and post-index scalar pairs |
| ADR/ADRP/literal LDR | yes | yes | yes | guest-PC and page-relative address domain |
| B/B.cond/CBZ/CBNZ/TBZ/TBNZ | yes | yes | yes | internal CFG targets and taken/not-taken paths |
| BL/BLR/BR/RET | yes | partial | partial | LR and direct/indirect guest targets are explicit; no function dispatcher |
| UDIV/SDIV | yes | yes | yes | architectural divide-by-zero (quotient 0) and signed overflow (INT_MIN/-1) semantics; LLVM lowering guards undefined behavior |
| Scalar FP: FMOV/FADD/FSUB/FMUL/FDIV/FNEG/FABS/FCMP/FCCMP/FCSEL/SCVTF/UCVTF/FCVTZS/FCVTZU/FCVT/FRINT | yes | yes | yes | S/D forms; raw IEEE bit patterns and sticky FPSR state; FCCMP/FCCMPE always update FPSR and conditionally select NZCV |
| Scalar FP: FSQRT/FMIN/FMAX | yes | yes | yes | reference runtime semantics, explicit NaN and signed-zero handling |
| AdvSIMD immediate moves | yes | yes | yes | FMOV vector immediate `.2S`/`.4S`/`.2D`; MOVI expanded architectural immediates with exact lane broadcasts |
| NEON DUP/INS/UMOV/SMOV/EXT and ZIP/UZP/TRN | yes | yes | yes | normalized arrangements and lane indices |
| NEON logical/integer/FP vector arithmetic and comparisons | yes | yes | yes | B/H/S/D arrangements; FP vector operations use the reference runtime |
| AdvSIMD structure stores | yes | yes | yes | ST1/ST2/ST3/ST4 full-vector and lane forms with B/H/S/D arrangements, checked little-endian memory, wrapped lists, and writeback |
| AdvSIMD structure loads | yes | yes | yes | LD1/LD1R/LD2/LD2R/LD3/LD3R/LD4/LD4R, lane preservation, interleaving/replication, wrapped consecutive lists, and checked post-index writeback |
| AdvSIMD widening multiply | yes | yes | yes | UMULL/SMULL, UMULL2/SMULL2, UMLAL/UMLSL and signed variants; lower/upper source halves and modulo-width accumulation |
| AdvSIMD fused multiply-add | yes | yes | yes | FMLA/FMLS vector and by-element forms through fused reference-runtime FP operations |
| AdvSIMD table lookup | yes | yes | yes | TBL/TBX one-to-four consecutive 16-byte tables, B8/B16 indexes, bounds checking, and TBX destination preservation |
| AdvSIMD FP zero compares | yes | yes | yes | FCMLT/FCMLE zero-immediate forms with IEEE comparison and FP status behavior |
| S/D/Q LDR/STR and LDP/STP | yes | yes | yes | checked guest memory; Q uses 16-byte vector helpers |
| FP/SIMD reciprocal and rounding estimates | yes | no | no | explicit unsupported behavior; reciprocal estimate families remain deferred |
| LDXR/STXR (B/H/W/X) | yes | yes | yes | per-thread monitor; deterministic 64-byte reservation granules |
| LDAXR/STLXR (B/H/W/X) | yes | yes | yes | acquire load and release store semantics |
| LDAR/STLR (B/H/W/X) | yes | yes | yes | acquire/release ordinary atomic accesses |
| CLREX | yes | yes | yes | clears the current thread's exclusive reservation |
| DMB/DSB/ISB | yes | yes | yes | project-owned barrier kind and option; ISB is an explicit IR boundary |
| MRS/MSR TPIDR_EL0 | yes | yes | yes | maps to per-thread `CpuState` TLS state |
| MRS TPIDRRO_EL0 | yes | yes | yes | read-only per-thread TLS value; writes are rejected |
| LDXP/LDAXP/STXP/STLXP, LSE atomics | yes | no | no | explicitly deferred pair/LSE semantics |

## Architectural state

## M36 scalar integer convergence

The measured `lsl w8, w8, w19` encoding (`0x1ad32108`) is normalized as the
canonical `Lsl` instruction with a register-controlled shift amount. The same
semantic primitive covers `LSL`, `LSR`, `ASR`, and `ROR` register forms plus
their immediate aliases. Register amounts are masked to five bits for W
registers and six bits for X registers; immediate amounts are range-checked.

The scalar bitfield family uses the architectural wrapped `wmask`/`tmask`
model for `UBFM`, `SBFM`, and `BFM`, including sign fill and destination
preservation. `EXTR` is represented by the same scalar IR shift/rotate
operations. These forms write only their scalar destination and leave NZCV,
FPCR, FPSR, and V-register state unchanged. Reserved width/encoding forms are
rejected by the lifter. The interpreter and optional LLVM lowering consume the
same verified IR; LLVM rotate lowering masks the inverse amount so a zero
rotate does not create a host shift-by-width poison value.

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

## M40 scalar convergence

M40 adds normalized ADC/ADCS/SBC/SBCS and NGC/NGCS carry-in semantics,
UMADDL/UMSUBL/SMADDL/SMSUBL long multiply forms, CRC32/CRC32C B/H/W/X
forms, scalar PRFM/PRFUM hints, and scalar REV/REV16. Carry result and
overflow use project-owned `AddWithCarry` IR operations; CRC uses the
reusable `Crc32` IR operation. The interpreter and LLVM lowering implement the
same result, while NZCV, FP state, and unrelated registers remain unchanged.
PRFM/PRFUM are explicit no-ops because no guest cache-state model exists.
M41 adds the measured AdvSIMD structure-load subset and M42 adds the matching
structure-store subset. M43 adds
scalar UMULL/SMULL aliases and the high-impact AdvSIMD widening multiply,
FMLA/FMLS, FCMLT/FCMLE, and TBL/TBX families described in its milestone note.

## M43 AdvSIMD coverage collapse

M43 normalizes the high-impact AdvSIMD families that dominated the remaining
measured frontier. Widening multiplies use explicit lane extraction, signed or
unsigned extension, modulo-width multiply, and optional add/subtract
accumulation. FMLA/FMLS uses a dedicated fused FP IR operation backed by the
reference runtime's IEEE bit-pattern and FPSR handling; by-element forms select
the encoded source lane. FCMLT/FCMLE lower zero-immediate comparisons through
the existing FP vector comparison path. TBL/TBX use a table-lookup IR operation
with one-to-four consecutive 16-byte tables; out-of-range indexes produce zero
for TBL and preserve the destination for TBX.

The decoder, detailed coverage predicate, lifter, verifier, interpreter, and
optional LLVM backend agree on these normalized forms. Malformed register
lists, incompatible arrangements, and unsupported estimates remain explicit
boundaries. Scalar UMULL/SMULL aliases are normalized separately from their
AdvSIMD operation names and require an X destination with W sources.

The M42 baseline measured 21,608 unsupported instructions in the prepared main
image. With the same 12,000,000-instruction bound, M43 measures:

```text
                         decoded    liftable   unsupported   decode failures
main                     11180285   11168628        11657              391
```

This is 9,951 fewer unsupported instructions, reducing the unsupported count by
46.05% and meeting the stretch target of at most 12,000. The serial and bounded
four-worker JSON coverage reports are byte-identical. The remaining frontier is
primarily deferred FP/SIMD estimates and unknown/SVE instructions; no provider,
register, or runtime success is fabricated to improve the count.

## M41 AdvSIMD structure loads

LD1 and LD1R are supported for B/H/S/D arrangements, including single-lane
LD1, 64-bit and 128-bit vectors, and immediate or register post-index forms.
LD2/LD2R, LD3/LD3R, and LD4/LD4R use the architectural structure order:
non-replicating loads deinterleave successive memory elements into consecutive
vector registers, while replicating loads repeat one element across each
destination. Destination lists wrap from V31 to V0 only at the architectural
register-list boundary; malformed or non-consecutive lists remain rejected.

Only the loaded lanes are changed. Unloaded upper lanes of 64-bit forms and
single-lane LD1 destinations are preserved. Every element access uses checked
little-endian `GuestLoad` semantics. Immediate post-index increments are
normalized from the structure width when Capstone omits the encoded immediate;
register post-index uses the X offset register. Pre/post writeback and memory
faults remain observable through the existing checked guest-memory runtime.
The interpreter and LLVM backend consume the same verified composition of
`GuestLoad`, `VectorInsertLane`, and address-add IR operations.

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
build/aarch64-analyze --coverage --coverage-workers 4 --json module.nso
```

The scanner reports decoded, liftable, unsupported and decode-failure counts,
sorted opcode frequencies, and the first unsupported guest addresses. Coverage
uses a deterministic bounded worker pool by default (up to four workers); use
`--coverage-workers N` to override it. JSON uses schema version 1 and only
prints the input basename, not a private absolute path. The tool performs all
analysis locally and does not upload or hash-report the input. XCI/NSP/NCA
extraction remains an external local workflow.

Coverage is an instruction-family baseline, not a correctness claim: an
instruction only belongs in the support matrix when its normalized form is
handled by the lifter and both execution backends.
