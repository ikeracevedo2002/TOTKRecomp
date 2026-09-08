# Milestone 23: scalable bounded indirect-target refinement

## Status

M23 is **Partial**. The generic refinement implementation, deterministic
synthetic validation, and AppleClang standard/sanitizer validation are
complete. The exact
private four-module input set is present outside Git, but the checked M22
executable artifact stops before execution at the existing analysis function
budget, and a retry with a much larger finite analysis envelope did not reach
execution within ten minutes. No new real execution result or real report hash
is claimed.

## Git and baseline

| Field | Value |
| --- | --- |
| Exact finalized M22 base | `0e76cdf14d507fbe413f9d74b962d6aef8f174e0` |
| M22 branch | `milestone-22-indirect-boundary-reconciliation` |
| M22 PR | #27, against `milestone-21-smulh-semantics` |
| M23 branch | `milestone-23-indirect-refinement-scaling` |
| M23 PR | [#28](https://github.com/ikeracevedo2002/TOTKRecomp/pull/28), stacked against M22 |
| M23 implementation/evidence head before this documentation update | `6dce2e45f539d872967dd98a9c018e308d2eec3d` |
| Compiler/configuration | AppleClang 17, C++20, Debug, Unix Makefiles |
| Pre-edit expected baseline | `252/252` |
| Pre-edit observed baseline | `252/252` |
| Final local suite at this handoff | `260/260` |

M22 publication was completed before this branch was created. Both the
push-triggered and pull-request-triggered exact-head workflows passed GCC,
LLVM 18, ASan/UBSan, TSan, and MSVC. M23 has not been rebased or merged with
another lineage.

## Forensic root cause

M22's outer driver used two unrelated global integers:

* `max_refinement_passes = 8` and `refinement_passes`, incremented once for
  every successful promotion;
* `max_new_indirect_candidates = 32` and `new_indirect_candidates`, also
  incremented once for every successful promotion.

Each outer iteration reran the complete execution from the original entry,
recollected the eligible observations, sorted them, and attempted the first
candidate that promoted. The loop therefore had no typed distinction between
an execution round, a unique target, an assessment, a successful promotion,
or an immutable map rebuild. Historical observations were not a worklist and
duplicate/no-op work was not auditable in the report.

The M22 handoff identifies the first stop as the pass gate after the former
frontier. The source accounting is exact for that behavior: the consumed
dimension was `refinement_passes`, consumed `8`, limit `8`; the separate
candidate counter was `8/32` and was not the first exhausted gate. The old
artifact was rerun against the available private files, but this checkout's
artifact stopped earlier with `analysis_budget_exceeded: function discovery
exceeded the function budget`; that discrepancy is recorded rather than
presented as an independent real-trace reproduction.

## M23 refinement model

`IndirectTargetRefinementWorklist` owns a typed, finite ledger with these
defaults:

| Dimension | M22 mechanism | M23 default | Meaning |
| --- | ---: | ---: | --- |
| refinement rounds | 8 coarse promotion passes | 64 | complete bounded execution/rerun cycles |
| unique target candidates | 32 promotion candidates | 256 | distinct target module/address pairs |
| candidate assessments | implicit | 512 | calls that assess a pending work item for refinement |
| successful promotions | implicit in pass count | 128 | newly certified entries installed |
| immutable map rebuilds | implicit in promotion | 128 | process-map reconstructions actually performed |

All defaults are finite. Analysis/CFG budgets remain separate and are still
enforced by `FunctionMapBuilder` on each immutable map construction. The new
limits are explicit CLI options and are serialized in the refinement summary;
they are not hidden constants in the execution loop.

Candidate work-item identity includes target module/address plus source module,
source function, source PC, control-flow kind, target register, pointer
provenance, and guest-load address. The unique-target capacity is charged once
per target module/address, so independently observed provenance for one
function does not consume new-function capacity repeatedly. Distinct provenance
is not discarded: representative observations are chosen by stable guest-side
ordering, observation counts are retained, and all distinct source/pointer
provenance records are serialized.

The pending map is ordered by guest-side identity, never host pointer, hash-table
iteration, filesystem order, timestamp, PID, locale, or scheduling. Trusted
existing entries bypass promotion accounting. A duplicate unchanged observation
is coalesced without a new target-capacity charge or map rebuild. A candidate is
reconsidered only when a later observation reports a certification result that
could have changed after an immutable map generation advanced.

Promotions still call the existing typed assessment and immutable refinement
APIs. Finalized maps are never mutated in place. If a limit is reached, the
execution stop remains `indirect_target_refinement_budget_exceeded` and the
report records the typed dimension, consumed value, limit, pending count, last
processed candidate, and next deterministic pending candidate. No pending
uncertified guest address is dispatched.

## Synthetic/adversarial validation

The M23 test file adds deterministic coverage for:

* a safe chain of twelve promotions, exceeding the historical eight-promotion
  ceiling;
* duplicate observation counts and coalescing without repeated capacity or
  rebuild charges;
* permutation-independent worklist normalization;
* repeated existing trusted entries without promotion charges;
* candidate-assessment, promotion, map-rebuild, round, and unique-target
  exhaustion with exact typed values;
* ownership-change reconsideration of only the affected observed work item;
* terminal rejected certification;
* deterministic schema-10 report serialization with observation provenance.

Existing M18/M20/M22 tests continue to cover insufficient runtime evidence,
ambiguous ownership, unmapped/non-executable/misaligned targets, CFG limits,
unsafe overlap, typed boundary reconciliation, immutable maps, and fail-closed
dispatch. No certification rule was relaxed and no TOTK address is present in
the implementation or synthetic tests.

## Real target at the former frontier

The last established M22 evidence remains:

| Field | Evidence |
| --- | --- |
| source module | `main` |
| source function | not independently recorded in the committed handoff; no value is inferred here |
| source PC / control flow | `main:0x7200000148`, `blr x8` (`0xd63f0100`) |
| target register | `x8` |
| target | `main:0x7200034bd0` |
| pointer provenance | guest load from `main:0x720456ab88` |
| relocation | RELA index `479604`, `R_AARCH64_RELATIVE`, verified readback |
| ownership/certification | bounded 5-block/26-instruction/7-edge candidate; no guest entry under the exhausted old outer budget |

The target was not hardcoded or forced through. The existing function-entry
certification requirements remain the authority for any future promotion.

## Real execution and frontier

The former M22 frontier has **not been demonstrated crossed by M23** in this
checkout. The available old executable was run against the private setup and
stopped during analysis with `analysis_budget_exceeded` before execution. A
retry using explicit finite analysis limits of 100,000 functions, 4,000,000
instructions, 500,000 blocks, 1,000,000 edges, 200,000 seeds, and 64 MiB was
CPU-bound for ten minutes without reaching execution and was stopped. These
are diagnostic outcomes, not passing execution results.

Therefore the only honest real execution metrics remain the established M22
ones: 390 guest instructions, 199 beyond the former M21 blocker, maximum call
depth 3, and zero runtime fallbacks, ending at
`main:0x7200034bd0` with the old typed refinement-budget stop. M23 does not
claim new promotions, rebuilds, instruction counts, final PC, or a new true
frontier until the coherent real-run analysis envelope is resolved.

UMULH naturally reached: **No new M23 real-run result**. The historical M22
controlled run recorded `No`; M23 did not force `sdk:0x7204717c30`.

Bootstrap changes: **No**. No TLS, thread, scheduler, heap, service, loader,
host callback, synthetic import result, or Nintendo-specific runtime state was
added.

## Report, determinism, validation, and privacy

The serialized execution report is schema `10`, bumped from M22 schema `9`
for typed refinement accounting and coalesced observation provenance. Synthetic
report serialization is byte-stable. Two new controlled real-run SHA-256
values were not produced because the run did not reach execution; the M22
historical pair remains
`e93395c32f3ff0dd39dbef81ff9667bf3936f590397445e3664750b7dac29820` for
reference only.

| Validation | Result |
| --- | --- |
| AppleClang standard | `260/260` local suite |
| AppleClang ASan/UBSan | `260/260`, no findings |
| AppleClang TSan | `260/260`, no race reports |
| GCC exact M23 head | pass in both exact-head workflows |
| LLVM 18 exact M23 head | pass in both exact-head workflows |
| MSVC exact M23 head | pass in both exact-head workflows |

New dependency count: `0`. No proprietary executable, private report,
configuration, key, or machine-specific path was committed. The pre-existing
`.gitignore`, `.DS_Store`, and `src/.DS_Store` worktree state remains untouched.

## Exact limitation and next blocker

The demonstrated next engineering blocker is not a refinement certification
decision yet: the available private four-module run cannot reach the M22
execution frontier under the checked analysis envelope, while the larger
finite diagnostic envelope is impractically expensive. The next milestone
must establish a coherent, bounded analysis configuration for the exact
private inputs, then rerun M23 twice and record whether
`main:0x7200034bd0` is safely certified or genuinely rejected. No new
semantic/bootstrap work is justified before that evidence exists.
