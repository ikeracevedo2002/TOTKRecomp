# Milestone 22: indirect function-boundary reconciliation

## Status and Git

M22 is locally validated on a branch created directly from the completed M21
head. No rebase or lineage change was performed. The implementation commit is
`10399266db5242af38abceebb15266f6f35a9348`; the documentation commit is the
final branch commit reported in the completion handoff. A documenting commit
cannot contain its own SHA.

| Field | Value |
| --- | --- |
| Exact M21 base SHA | `e21b340261a504d4ff5ab5369be304cd54f4bdc8` |
| Branch | `milestone-22-indirect-boundary-reconciliation` |
| PR | Not created in this environment; pending publication and CI |
| Stack/base relationship | M22 is stacked directly on M21; M21 remains stacked on M20 |
| Commits | `10399266db5242af38abceebb15266f6f35a9348` implementation; final documentation commit reported at handoff |
| Final head SHA | Reported in the completion handoff |

## Baseline

The expected pre-edit M21 suite was `245/245`. The exact starting checkout
produced that observed baseline: `245/245` passed. The final local suite is
`252/252` passed, including seven M22 tests and 129 M22 assertions.

## Real conflict investigation

The existing trusted canonical entry was `main:0x7200001520`. Its independent
pointer evidence is slot `main:0x720456ab68`, RELA index `479600`, relocation
type `R_AARCH64_RELATIVE`, with applied slot-value readback verified. The
candidate was `main:0x7200016830`, observed at `main:0x7200000148` through
`blr x8` (`0xd63f0100`). Its guest-load slot is `main:0x720456ab70`, RELA
index `479601`, also `R_AARCH64_RELATIVE`, with applied readback verified.
Neither relocation declares function metadata; relocation remains rebasing
provenance only.

Before boundary-aware reanalysis, the `0x1520` record had 12 blocks, 59
instructions, 17 accounted CFG edges, no unresolved flow, and precise owned
ranges:

* `main:0x7200000170` with size `136` bytes (`34` instructions);
* `main:0x7200001520` with size `100` bytes (`25` instructions).

Its display envelope was `[main:0x7200000170, main:0x7200001584)`. Block starts
were `0x1520, 0x153c, 0x1544, 0x154c, 0x170, 0x17c, 0x1b8, 0x1c0,
0x1d0, 0x1d8, 0x1ec, 0x1f0`; block ends were `0x153c, 0x1544, 0x154c,
0x1584, 0x17c, 0x1b8, 0x1c0, 0x1d0, 0x1d8, 0x1ec, 0x1f0, 0x1f8`.
Direct calls were `0x1538 -> 0x712b90`, `0x1540 -> 0xed5a54`, and
`0x1548 -> 0xed5a6c`. The helper's direct branch edges were
`0x178 -> 0x1ec`, `0x1b4 -> 0x1ec`, `0x1cc -> 0x1c0`, and
`0x1d4 -> 0x1f0`; returns were at `0x1ec` and `0x1f4`. The caller's terminal
tail transfer was `0x1580 -> 0x170`. There was no unresolved or indirect CFG
flow.

The candidate began with `stp x29, x30, [sp, #-0x30]!` at `0x16830`, opcode
`0xa9bf3bfd`. Its initial bounded CFG had 12 blocks, 74 instructions, 17
accounted edges, no unresolved flow, and precise ranges for the same helper
plus `[main:0x7200016830, size 160]`. Its four private prefix blocks were:

| Block | Instructions | Termination/evidence |
| --- | ---: | --- |
| `0x16830..0x16878` | 18 | direct call `0x16874 -> 0x2aa42a0` |
| `0x16878..0x16898` | 8 | direct call `0x16894 -> 0x170` |
| `0x16898..0x168b4` | 7 | direct call `0x168b0 -> 0x2aa42a0` |
| `0x168b4..0x168d0` | 7 | unconditional branch `0x168cc -> 0x170` |

The first instruction of the candidate is therefore a valid decoded entry,
but its initial ownership was not independently safe. The real execution
limits used for the bounded run were 100000 IR operations, 256 function
transitions, call depth 16, 4096 events, and 10000 guest blocks.

The exact precise intersection was deterministic:

| Measure | Result |
| --- | --- |
| Intersecting precise ranges | 1 |
| Range | `[main:0x7200000170, main:0x72000001f8)`; 136 bytes |
| Intersecting instructions | 34 |
| First overlapping instruction | `main:0x7200000170`, `mov x8, x0` |
| Last overlapping instruction | `main:0x72000001f4`, `ret` |
| Is `0x16830` owned by `0x1520`? | No |
| Does overlap begin later than `0x16830`? | Yes, at `0x170` |
| Full-set strict subset? | No; the helper set is a strict subset of each pre-fix set |
| Shared suffix/tail? | The common region is exactly the helper body, but this is analyzer over-claim, not legitimate shared ownership |
| Causal branch/call | Existing `B 0x1580 -> 0x170`; candidate `BL 0x16894 -> 0x170` and `B 0x168cc -> 0x170` |
| BL followed as intraprocedural flow? | No; direct calls retain fall-through and are recorded as calls |
| Fall-through cause? | No |
| Already-known target traversed? | No; `0x170` became known during boundary fixed-point refinement |
| Envelope artifact? | No; the witness is exact instruction ownership |

The report-time independent range-set calculation reproduced the same one
range and 34-instruction intersection without calling the production overlap
helper.

## Classification and model

This is **Case A: existing-function analyzer over-claim**. The existing and
candidate analyses both incorporated the helper because the helper entry was
not yet in the finalized known-entry set. The candidate's `BL` was not
mistakenly followed; instead, a terminal unconditional branch into a
separately discoverable helper was not yet treated as a function boundary.

The fix adds a narrow typed `analyzer_over_claim` reconciliation. It requires
all of the observed structural witnesses: the existing unconditional branch,
the candidate direct call, the candidate unconditional branch, and a
complete, bounded helper CFG. Boundary-aware fixed-point reanalysis then gives
the helper canonical ownership of `[0x170, 0x1f8)`, the existing function
ownership of `[0x1520, 0x1584)`, and the candidate ownership of
`[0x16830, 0x168d0)`. The helper has eight blocks and 34 instructions; the
candidate has four blocks, 40 instructions, seven edges, direct calls to
`0x170` and `0x2aa42a0`, and a terminal function-transfer edge at `0x168cc`.

No alternate-entry or shared-tail ownership was introduced. The function-map
API now separates exact callable-entry lookup, canonical lookup, and precise
instruction-owner lookup. Reconciliation is proposed, fully validated,
finalized, and atomically installed; failed validation leaves the old map
unchanged. Repeated observation is idempotent. Exact entries and explicit
precise ownership remain distinct from display envelopes, and arbitrary or
partially explained overlaps still reject with a typed incompatible-overlap
result.

## Safety and synthetic validation

The evidence chain for `0x16830` retains the observed indirect callsite,
runtime target, guest-load provenance, exact slot, source and target modules,
RELA source/index/type, addend/resolution, applied readback, executable
ownership, bounded CFG, exact overlap witness, reconciliation classification,
and accepted/rejected evidence. `R_AARCH64_RELATIVE` is not promoted to
function-pointer metadata.

The M22 tests cover exact-entry idempotence, disjoint transactional promotion,
unexplained overlap rejection, display-envelope-only overlap, alignment and
executable gates, unresolved/budget-exhausted CFGs, relocation-only evidence,
runtime observation with invalid CFG, boundary reconciliation, helper-owner
queries, loop preservation, execution from the repaired boundary, and
deterministic evidence ordering. They use synthetic maps and CFGs rather than
the private TOTK addresses.

## Real execution

The existing controlled execution naturally reached the original `blr x8` and
resolved it to `main:0x7200016830`. Certification succeeded for the typed
analyzer-over-claim reconciliation, not for relocation or observation alone.
This is a separate canonical function, so its canonical owner and actual entry
are both `main:0x7200016830`; the helper reached by its internal call/tail
transfer has canonical owner `main:0x7200000170`. The first guest instruction
executed at the target was:

`main:0x7200016830: stp x29, x30, [sp, #-0x30]!` (`0xa9bf3bfd`)

and the next guest PC was `main:0x7200016834`. Execution remained in guest
dispatch, preserved register provenance and call/return behavior, and invoked
no runtime fallback.

The run used the finite limits above and produced:

| Metric | Result |
| --- | ---: |
| Total guest instructions | 390 |
| Instructions beyond the former M21 blocker | 199 |
| Guest blocks | 71 |
| Maximum call depth | 3 |
| Direct calls | 11 |
| Indirect calls | 5 |
| Function transitions | 7 |
| Returns | 15 |
| Runtime fallback count | 0 |

The next genuine typed frontier was another indirect target at
`main:0x7200034bd0`, observed from the same `blr x8`, with guest-load slot
`main:0x720456ab88`, RELA index `479604`, readback verified, and a bounded
5-block/26-instruction/7-edge CFG. It was not forced: the run stopped at
`indirect_target_refinement_budget_exceeded` at `main:0x7200000148`
(`blr x8`, `0xd63f0100`) after the configured refinement passes. The target's
candidate was structurally validated but not entered on that budget boundary.

The historical `sdk:0x7204717c30` UMULH frontier was not reached naturally.
No bootstrap state was added; no TLS, thread, heap, scheduler, service,
host-pointer, import-result, callback, register, or return-address fabrication
was needed.

## Determinism

The externally observable report schema is `9` (incremented from M21's `8`
because the typed boundary-reconciliation and independent-overlap report
contract was added). Two identical controlled executions produced byte-
identical reports:

* SHA-256 #1: `e93395c32f3ff0dd39dbef81ff9667bf3936f590397445e3664750b7dac29820`
* SHA-256 #2: `e93395c32f3ff0dd39dbef81ff9667bf3936f590397445e3664750b7dac29820`
* Byte-identical: yes

## Validation, CI, dependencies, and privacy

| Configuration | Result |
| --- | --- |
| AppleClang standard build/tests | `252/252` passed |
| AppleClang ASan/UBSan | `252/252` passed |
| AppleClang TSan | `252/252` passed |
| GCC | Not installed locally; final-head CI pending |
| LLVM 18 | Not installed locally; final-head CI pending |
| MSVC | Not available locally; final-head CI pending |
| CI workflow | Existing matrix unchanged |

New dependency count: `0`.

No proprietary artifacts, extracted binary data, private execution reports,
private configuration, or machine-specific absolute paths were committed.
Existing local ignore-file and `.DS_Store` changes were not part of the M22
commits.

## Limitations and next blocker

M22 does not claim that TOTK boots, that arbitrary indirect calls are solved,
that arbitrary overlapping functions are supported, that all executable code
has been discovered, full ABI/runtime correctness, or full-game compatibility.
The exact next blocker is the refinement-budget stop while assessing
`main:0x7200034bd0`; cross-platform final-head CI and PR publication remain
pending.
