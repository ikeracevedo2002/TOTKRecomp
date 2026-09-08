# Milestone 21: signed multiply-high semantics and the real execution frontier

## Status and Git

M21 is complete locally and is submitted as stacked PR #26 against the still
open M20 branch. The branch was created directly from the exact M20 head; no
rebase or lineage change was performed.

| Field | Value |
| --- | --- |
| Starting M20 SHA | `5164329b32b18a56afc1de5d48a388d5683bf6fe` |
| Branch | `milestone-21-smulh-semantics` |
| PR | #26, stacked on `milestone-20-indirect-function-entry-certification` |
| M20 PR | #25, open against `milestone-19-move-wide-umulh-frontier` |
| Final SHA | Reported in the completion handoff; a documenting commit cannot contain its own SHA |

The pre-edit M20 suite was `241/241`, matching the observed M20 baseline.
The M19 baseline was `233/233`. The final local suite is `245/245`.

## SMULH implementation

AArch64 `SMULH Xd, Xn, Xm` computes the upper 64 bits of the conceptual
signed 128-bit product of the signed 64-bit interpretations of `Xn` and
`Xm`. It does not modify NZCV and has ordinary fall-through control flow.

M20's single structural normalization remains the only decoder path. It
already maps Capstone's SMULH form to project-owned `InstructionId::Smulh`.
The lifter now requires exactly three X-register operands and emits the
project-owned `ir::Opcode::MulHighSigned`, with two `i64` operands and an
`i64` result. The destination is written only after both source values have
been read, so source/destination aliases are architectural rather than
host-evaluation artifacts. Fall-through is `pc + 4`.

The IR opcode has a builder, stable printer name, verifier rule, legacy/M9
interpreter dispatch, and legacy/M9 LLVM dispatch. The verifier rejects any
operand or result width other than the established matching `i64` contract.

The portable interpreter reuses the existing 32-bit-limb UMULH primitive. For
unsigned bit patterns `ua` and `ub`, it computes `UMULH(ua, ub)` and subtracts
`ub` when `ua` is negative and `ua` when `ub` is negative, all as wrapping
`uint64_t` arithmetic. This avoids signed overflow, undefined signed shifts,
and a mandatory compiler-specific 128-bit integer type.

The LLVM backend sign-extends each `i64` operand to `i128`, multiplies,
arithmetic-shifts the product right by 64, and truncates to `i64`. No `nsw` or
`nuw` assumptions are attached.

## Semantic validation

The M21 test file contains 20 deterministic fixed vectors and 123 focused
assertions across four test cases. Coverage includes zero, unit values,
`-1`, `INT64_MIN`, `INT64_MAX`, both sign-bit-only operands, nonzero positive
high halves, populated high/low halves, all sign combinations, and destination
aliasing of the left operand, right operand, and both operands.

Expected vector values were generated independently using arbitrary-precision
signed-product calculations and stored as constants; the production helper
is not used to create expected values. The report-time real-execution oracle
is a separate 32-bit-partial-product implementation plus the same unsigned
two's-complement correction relation. When LLVM 18 is enabled, the focused
vectors also compare LLVM execution with the portable interpreter bit-for-bit,
including NZCV. The local machine did not have the required LLVM 18.1.3
installation, so that conditional test is a CI check.

The exact real opcode `0x9b4c7d4a` is covered, as is an independently encoded
`smulh x3, x3, x3` form. Decoder tests verify destination/lhs/rhs extraction,
X-register widths, normalized identity, fall-through, the Semantic IR opcode,
and source/destination aliasing.

## Real SMULH frontier

The same private, controlled four-module execution setup used by M20 was run
with explicit finite limits: 100000 IR operations, 256 function transitions,
16 call depth, 4096 events, and 10000 guest blocks. No proprietary input or
private configuration was added to Git.

The typed schema-8 report records the first real SMULH execution:

| Field | Observed value |
| --- | --- |
| Module | `main` |
| PC | `0x72000001a4` |
| Raw opcode | `0x9b4c7d4a` |
| Instruction | `smulh x10, x10, x12` |
| Destination | `x10` |
| LHS | `x10`, raw `0x0000000000000000`, signed `0` |
| RHS | `x12`, raw `0x2aaaaaaaaaaaaaab`, signed `3074457345618258603` |
| Independent expected high64 | `0x0000000000000000` |
| Actual high64 | `0x0000000000000000` |
| Match | `true` |
| Next guest PC | `0x72000001a8` |

The operands are captured from the pre-instruction state. The destination is
then written by the project interpreter, and the independent expected/result
comparison is retained in `execution.smulh_frontier` and the executed guest
instruction evidence. The former M20 blocker was crossed: yes.

## Execution frontier and M20 regression check

After SMULH, execution continued for 9 additional guest instructions, for a
total of 191 guest instructions. Maximum guest call depth was 3. The final
typed stop was:

`unknown_guest_function` at `main:0x7200000148`, diagnostic instruction
`blr x8` (`0xd63f0100`). Execution remained in guest dispatch until this
typed indirect-boundary stop; no runtime/import boundary was reached and the
runtime fallback count was zero.

The former M20 transfer was still exercised and remained certified:

* callsite: `main:0x7200000148`, `blr x8`;
* target: `main:0x7200001520`;
* provenance: guest load from `main:0x720456ab68`;
* relocation: RELA index `479600`, `R_AARCH64_RELATIVE`, addend `0x1520`,
  verified by readback;
* certification: high-confidence certified target, canonical owner `main`;
* guest entry: successful, beginning with the target's stack-frame prologue;
* runtime fallback count: `0`.

One newly encountered transfer at the same callsite was deliberately not
accepted. A later guest-loaded value targeted `main:0x7200016830` from pointer
slot `main:0x720456ab70` (RELA index `479601`, `R_AARCH64_RELATIVE`). Its
bounded candidate overlapped the precise ownership of the already certified
`main:0x7200001520` function, so certification failed closed with
`overlaps_existing_function` and no guest code was entered. This is the next
real frontier, not a new heuristic or a fabricated execution result.

Real UMULH reached: no. The historical `sdk:0x7204717c30` UMULH frontier was
not forced and did not occur on this ordinary path.

## Bootstrap and scope

No new bootstrap model was required. The trace used the existing controlled
execution setup and reached concrete guest code without adding TLS, thread,
heap, scheduler, service, host-pointer, import-result, or synthetic-return
state. This milestone does not claim that TOTK boots or that arbitrary
SMULH-heavy code has been validated; the real evidence is for the recorded
path and operand state above.

## Determinism, validation, dependencies, and privacy

The serialized execution report schema is `8`, bumped from M20's schema 7 for
the typed SMULH frontier and related evidence. Two identical real runs with
the explicit limits above produced byte-identical reports:

* SHA-256 #1: `c77a6b9dcb1dc9292fa98f401dea5ab74a13bf82be264423e3aa9ac62d617581`
* SHA-256 #2: `c77a6b9dcb1dc9292fa98f401dea5ab74a13bf82be264423e3aa9ac62d617581`
* identical: yes

Local validation:

| Configuration | Result |
| --- | --- |
| AppleClang standard build | `245/245` passed |
| AppleClang ASan/UBSan | `245/245` passed |
| AppleClang TSan | `245/245` passed |
| GCC | CI check pending at this documentation revision |
| LLVM 18.1.3 | CI check pending; not installed locally |
| MSVC | CI check pending; not available locally |

New dependencies: `0`. No proprietary game files, private reports, private
configuration, or machine-specific absolute paths were committed. Local
execution reports remain ignored workspace artifacts.

## Explicit limitations

M21 adds only signed multiply-high semantics and the evidence/reporting needed
to audit this frontier. It does not add unrelated AArch64 instructions, alter
UMULH semantics, broaden indirect-call certification, add bootstrap stubs, or
claim full-game compatibility. The exact next frontier is the failed
certification of the indirect `blr x8` target `main:0x7200016830` because its
bounded candidate overlaps existing precise function ownership.
