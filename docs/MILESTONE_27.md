# Milestone 27: Proof-Preserving Aggregate Refinement Scaling

## Status

Complete locally; final status is contingent on the required remote Linux,
LLVM 18, and Windows CI jobs recorded below. M27 addresses the M26 productive
promotion/map-rebuild ceiling architecturally. It does not force a guest PC,
seed a private address, weaken certification, or enlarge the ordinary event
limits.

## Git lineage

- Exact M26 base: `f6c9b4bd07df90878a138d38cdf8204440052235`
- Expected parent branch: `milestone-26-progress-aware-indirect-refinement`
- M27 branch: `milestone-27-aggregate-refinement-resource`
- Final head and commits: recorded in the handoff after the final commits
- PR: stacked against `milestone-26-progress-aware-indirect-refinement`, not `main`

The branch was created directly from the exact M26 HEAD. No rebase onto
`main` or unrelated merge was used.

## Baseline and M26 reproduction

The pre-edit configure/build workflow used the repository's existing Debug
Unix Makefiles build with LLVM disabled. The expected and observed baseline
was `282/282` tests.

Before production edits, the ordinary ignored private executable-set workflow
reproduced the M26 report exactly:

- report size: 28,413,832 bytes;
- SHA-256: `366bf1fd6ecfe15e880f919501980e9b06e75ea71e8d84b4596199cb063f4a45`;
- guest instructions: 6,186;
- productive rounds: 129;
- unique candidates: 129;
- successful promotions/map rebuilds: 128/128;
- module maps rebuilt/reused: 128/128;
- pending work: one candidate.

The exact blocker was `main:0x72000f04f0`, observed by `blr x8` at
`main:0x7200000148` with guest-load pointer provenance at
`main:0x720456af40`. It was assessed and certified, but the old
`max_promotions = 128` and `max_map_rebuilds = 128` gates prevented the
immutable promotion. No target seed, forced PC, forced jump, or budget
override was used.

## Forensic resource analysis

The old `FunctionMapBuilder::build` path reconstructed and revalidated every
function record in the target module for each immutable generation. In the
M26 final `main` generation, one such rebuild accounted for:

| Work | M26 final target-module generation |
| --- | ---: |
| canonical functions | 145 |
| CFG analyses | 290 |
| instructions | 8,614 |
| blocks | 1,486 |
| edges | 2,080 |
| analyzed bytes | 34,456 |
| boundary-finalization passes | 1 |
| conflict-processing phase units | 10,585 |

This answers the core question: before M27, one newly certified target caused
work proportional to the whole target-module execution closure, not only the
logically affected region.

M27 keeps deterministic structural accounting and measures the replacement
work across the ordinary private run. After 256 promotions, the cumulative
aggregate ledger was:

| Dimension | Consumed | Default limit |
| --- | ---: | ---: |
| functions newly CFG-analyzed | 586 | 200,000 |
| functions reanalyzed | 6 | 100,000 |
| instructions | 19,250 | 8,000,000 |
| blocks | 3,708 | 2,000,000 |
| edges | 5,586 | 4,000,000 |
| analyzed bytes | 77,000 | 268,435,456 |
| boundary-finalization passes | 256 | 2,048 |
| invalidated records | 3 | 100,000 |
| immutable refinement transactions | 255 | 512 |
| finalized records reused | 36,574 | not a charge limit |

The final `main` map had 274 canonical records, 2 newly analyzed records,
273 reused records, and no invalidated records in that final transaction.
The cumulative counters therefore distinguish retained validated state from
new/recomputed work rather than hiding all work under a promotion count.

## M27 resource model

`IndirectTargetRefinementAnalysisBudgets` is a finite typed ledger layered on
the existing M24 `AnalysisBudgets` dimensions. It charges cumulative work that
is specifically attributable to immutable refinement: functions analyzed,
functions reanalyzed, instructions, blocks, edges, bytes, boundary
finalization, invalidated records, and transactions. Each limit has a stable
library-default or CLI/local-configuration provenance and is serialized with
the report.

The ordinary default profile retains `max_promotions = 128` and
`max_map_rebuilds = 128` as visible compatibility fields, but does not charge
successful monotonic progress to them. The old event guards remain available
only when an old CLI option is explicitly supplied, or when a library caller
explicitly selects the compatibility mode. They remain finite and typed; no
unlimited sentinel exists. New CLI options reject zero, overflow, and
unrepresentable values. Local JSON may use a `refinement_analysis` object;
CLI values take precedence and all provenance is reported.

Aggregate exhaustion is reported with the dimension, consumed value, limit,
module, immutable generation, pending count, and deterministic next candidate
when one is known. M27 increments the report schema from 12 to 13 because
configured aggregate limits, provenance, consumption, and typed exhaustion
context are externally visible.

## Termination argument

Termination does not depend on successful promotions alone. New target
identities are bounded by `max_unique_candidates`; candidate assessments are
bounded by `max_candidate_assessments`; terminal resolutions remove pending
work; generation-dependent reconsideration is finite; and unchanged
no-progress attempts consume the finite stagnation allowance. Every immutable
refinement transaction is charged against finite structural ledger dimensions.
The compatibility event guards, when enabled, are also finite. Duplicate
observations coalesce by stable guest-side identity. Therefore neither a
productive sequence nor a stagnant retry loop can continue without exhausting
a deterministic finite dimension.

## Persistent reuse, invalidation, and transactionality

A refinement copies a frozen prior `FinalizedFunctionMap` into candidate state
and publishes only after CFG, ownership, boundary, conflict, provider, and
budget validation succeeds. The prior generation is never mutated and remains
usable on failure.

Reuse is permitted only for matching stable module identity, build ID, input
hash, guest base, and executable layout. New certified callable entries and
their validated direct-call targets become explicit boundary inputs. A copied
record is invalidated when the new entry intersects its precise owned ranges
or its recorded callable-boundary dependencies. Invalidated records are
reanalyzed and their ownership is normalized before publication. Unchanged
module maps are carried through process refinement, while provider lookup is
still resolved against the current process-wide state; no host pointer,
unordered iteration order, path, PID, or timestamp is a cache key.

This is reuse of validated immutable state, not blind copying of stale
`FunctionRecord`s. The boundary-dependency field makes the M22 analyzer
over-claim case explicit and auditable.

## Synthetic validation

The dedicated M27 test file contains five cases and passed 812 assertions in
the focused run:

- 192 independently certified sequential promotions under ordinary defaults;
- exact instruction-ledger exhaustion with module, generation, pending count,
  and next candidate;
- disjoint immutable refinement with retained/reanalyzed counters;
- newly introduced callable-boundary invalidation and ownership recomputation;
- finite default dimensions and deterministic dimension names.

The complete suite additionally covers M22 boundary reconciliation, immutable
rollback, cross-module map reuse/provider lookup, stale-provider prevention,
duplicate coalescing, generation-dependent reconsideration, permutation
determinism, and M18/M20/M23/M24/M25/M26 regressions. All 287 standard tests,
including those focused regression cases, passed.

## Former M26 frontier under M27

- source: `main:0x7200000148`, `blr x8`;
- target: `main:0x72000f04f0`;
- pointer provenance: guest load at `main:0x720456af40`;
- assessment: reached;
- certification: certified, high confidence;
- promotion: normal immutable promotion;
- guest entry: yes, through guest dispatch;
- first instruction: `stp x29, x30, [sp, #-0x20]!` at the target;
- next guest PC: `main:0x72000f04f4`;
- forced jump/seed/PC: no;
- budget override: no.

The report's certification record also states `guest_code_entered: true` and
retains relocation/readback evidence as rebasing provenance rather than as
function proof.

## Real execution and next frontier

The ordinary private workflow was run twice. Both runs reached the same next
typed frontier after the former target was entered:

| Metric | M27 result | Delta versus M26 |
| --- | ---: | ---: |
| guest instructions | 14,843 | +8,657 |
| guest blocks | 2,679 | +1,595 |
| function transitions | 65 | +23 |
| maximum call depth | 3 | 0 |
| direct calls | 468 | +322 |
| indirect calls | 252 | +128 |
| returns | 719 | +450 |
| runtime fallbacks | 0 | 0 |
| execution attempts | 257 | +128 |
| productive rounds | 256 | +127 |
| stagnant rounds | 1 | +1 |
| unique observations | 1,008 | +572 |
| unique candidates | 256 | +127 |
| candidate assessments | 256 | +127 |
| terminal resolutions | 0 | 0 |
| successful promotions | 256 | +128 |
| immutable map rebuilds | 256 | +128 |
| module maps rebuilt/reused | 256 / 256 | +128 / +128 |
| reconsiderations | 0 | 0 |
| pending work | 1 | 0 |

The actual next frontier is `main:0x7200130180`, observed from the same
`blr x8` at `main:0x7200000148`, with guest-load provenance at
`main:0x720456b340`. It was left pending because the finite
`max_unique_candidates = 256` dimension was exhausted. No target-specific
behavior was added and execution was not forced through this candidate.

## Determinism

The two ordinary private reports were byte-identical:

- report 1: 35,114,732 bytes;
- report 2: 35,114,732 bytes;
- SHA-256 1: `2ee4ba4598a4d6aee1d9458e478488303e1d46232fa22671afe7077d7bf3426b`;
- SHA-256 2: `2ee4ba4598a4d6aee1d9458e478488303e1d46232fa22671afe7077d7bf3426b`;
- byte comparison: identical.

## Validation matrix

- standard Debug suite: 287/287;
- focused M27 suite: 812 assertions in 5 cases;
- ASan/UBSan: 287/287;
- TSan: 287/287;
- Linux/GCC CI: recorded after push;
- LLVM 18 CI: recorded after push;
- Windows/MSVC CI: recorded after push.

No sanitizer suppression was added. The local host does not provide a genuine
GCC, LLVM 18 package, or MSVC toolchain; remote workflow results are therefore
the authoritative platform evidence.

## Dependencies and privacy

New dependency count: `0`. The existing C++20, CMake, Catch2, nlohmann/json,
LZ4, and Capstone infrastructure is unchanged.

No proprietary executable data, extracted instruction dump, key, report,
private configuration, or machine-specific path is committed. Private
addresses appear only in this milestone evidence and are not production
constants or synthetic fixtures.

## Exact recommendation for Milestone 28

Begin only at the observed `main:0x7200130180` candidate and determine whether
the finite candidate-cardinality frontier should be refined without bypassing
certification. Do not raise that limit speculatively, force the target, or
claim broader compatibility until the next evidence-backed frontier is
measured.
