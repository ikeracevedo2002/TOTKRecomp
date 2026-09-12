# Milestone 36: Scalar Integer Convergence and Refinement Acceleration

## Status

M36 implements the measured scalar shift/bitfield family and the second
refinement performance pass. The exact measured `lsl w8, w8, w19` form now
decodes, lifts, verifies, and executes in the synthetic architectural fixture.

The available local recovery configuration does not reproduce the M35 real
execution frontier: it stops earlier at an unresolved provider search for
`__nnmusl_init_dso`. Consequently, this checkpoint records the implementation
and synthetic convergence evidence, but does not claim that a compatible real
run crossed the M35 frontier.

## Git

```text
Base lineage: infra-agent-orchestration-ci
Parent: ca60575d5989403985d702f72d7fa953856b33ab
Branch: milestone-36-scalar-integer-refinement-acceleration
PR: #44
Merge status: not merged
```

The branch is an isolated stack from the exact M35 final commit. Semantic and
performance changes are separated into reviewable commits; documentation and
validation evidence are kept in the final documentation commit.

## Baseline

The M35 comparable real runs were approximately `1221.28 s` and `1234.68 s`.
The final M35 profile measured `1191.78 s` in refinement across 728 rounds,
727 productive rounds, 726 transactions, and 727 rebuilds. It assessed 727
candidates, analyzed 1,530 functions, reanalyzed 6, and reported 6,797 lift
cache hits, 748 misses, and zero invalidations.

One M36 reproduction attempt with the available local inputs completed in
`80.30 s` wall time (`78.71 s` user, `0.61 s` system), but is not comparable:
it stopped at an unresolved import before the M35 `main` frontier and performed
no refinement transaction. Its profile measured 39.85 ms of guest execution,
117.66 ms of refinement, and 4.25 s of report generation. The resulting report
was 22,164,104 bytes with SHA-256
`8c9ce368c66e8a1aaaa7cff53d6ba684f1cdfa9ce1e363b551cf377ef77374ec`.

## Scalar integer semantics

Capstone's immediate aliases and variable forms are normalized to the
project-owned scalar IDs. `LSLV`, `LSRV`, `ASRV`, and `RORV` normalize to
`Lsl`, `Lsr`, `Asr`, and `Ror`; `EXTR` has its own normalized ID while sharing
the scalar IR shift/rotate primitives. The exact measured word
`0x1ad32108` is therefore a masked register-controlled `Lsl` rather than a
decoder-specific opcode exception.

Implemented forms are:

- W/X register-controlled `LSL`, `LSR`, `ASR`, and `ROR`;
- immediate shift aliases using the same lifter path;
- wrapped `UBFM`, `SBFM`, and `BFM` masks, including sign fill and destination
  preservation;
- W/X `EXTR` with checked width and immediate ranges.

Register amounts are masked to five bits for W operations and six bits for X
operations. W writes zero-extend into X. The supported forms do not update
NZCV, FPCR, FPSR, or V registers, and reserved width/encoding combinations are
rejected. Interpreter and LLVM paths consume the same verified IR; LLVM rotate
lowering masks the inverse amount for the zero-rotate case.

## Performance architecture

Refinement now assesses all currently pending candidates against one immutable
map generation, selects a stable maximal set of structurally independent
candidates, groups them by target module, and builds each touched module once.
A deterministic coordinator publishes the resulting batch. If the combined
build cannot be promoted, stable singleton retries preserve the serial semantic
fallback.

`ProcessFunctionMap` stores immutable shared module records. A publication
reuses untouched module maps by identity and replaces only the touched module.
The `FunctionMapBuilder` reuse path copies validated finalized records and
invalidates a record only when a newly introduced callable boundary intersects
its precise ownership or recorded boundary dependency. Decoded CFG information
and finalized analysis for unaffected functions are consequently retained.

The remaining publication work is rebuilding the process entry index. It is
deliberately retained for deterministic O(log n) lookup and is reported as a
known bottleneck rather than hidden behind an unmeasured cache.

The bounded worker pool parallelizes only independent host-side candidate
assessment. `--analysis-workers 1` is the serial reference; the default uses
the lesser of four and the reported hardware concurrency. Workers read shared
immutable inputs and write fixed-index result slots. The coordinator merges in
input order, so worker completion order cannot affect reports, budgets, errors,
or guest execution. Worker count and timing are diagnostic profile fields, not
stable JSON data.

## Tests and validation

The focused M36 suite passes 142 assertions in 8 cases. It covers the exact
measured decode, W/X masking, immediate aliases, rotate and arithmetic-shift
edges, wrapped bitfield masks, EXTR, state preservation, malformed IR, and
conditional LLVM parity. The synthetic worker test compares serial, two-worker,
and four-worker assessment outputs byte-for-byte. The M26 refinement suite
passes 600 assertions and exercises actual two-module batch publication and
untouched-map identity reuse.

The standard CTest matrix at the final code checkpoint passes `382/382` tests
in `97.19 s`; the direct Catch2 run reports `37,047` assertions. ASan/UBSan
passes `382/382` in `377.34 s`, and TSan passes `382/382` in `1068.97 s`, with
no sanitizer or race diagnostics. LLVM is disabled on this host and remains
covered by conditional tests and the remote matrix.

## Before and after counters

M35's refinement baseline was rebuild-per-productive-round: 727 rebuilds for
727 productive rounds. M36 adds these stable report counters:

```text
batching: batch_count, candidates, singleton_batches,
          average_batch_width, max_batch_width, rebuilds_avoided
incremental_reuse: finalized_functions_reused, cfgs_reused,
                   functions_rebuilt, modules_touched,
                   incremental_updates, full_rebuilds
```

The synthetic two-candidate worklist publishes one logical batch and avoids
one rebuild. The non-comparable available real run had zero candidates,
transactions, rebuilds, batches, and reuse events, so it cannot establish a
real wall-clock speedup. A compatible private input is required to measure the
targeted reduction toward the preferred sub-600-second run.

## Real convergence and final frontier

The implementation crosses the measured shift frontier in the exact synthetic
fixture. No new real guest blocker was crossed in the available local run. Its
first stop remains:

```text
stop reason: unresolved_import
module: main
function: 0x0000007202aa4210
PC: 0x0000007202aa421c
opcode: 0xd61f0220
instruction: br x17
classification: unresolved import / incomplete provider search
```

The run counters were 63 guest instructions, 3 blocks, 319 IR operations, 2
slices, 1 direct call, 0 indirect calls, 0 returns, 0 transfers, maximum call
depth 1, and 0 runtime fallbacks. Event accounting was 8 generated, 8 retained,
and 0 omitted.

## Determinism

The worker design has exact synthetic serial/multi-worker report equality. The
final profiled worker-4 run measured `79.33 s` wall time (`78.44 s` user,
`0.73 s` system, approximately 99% host CPU utilization), with 119.36 ms of
refinement, 8 microseconds of assessment, and no batch candidates because the
input stopped before refinement. Its schema-20 report is 22,164,104 bytes and
has the SHA-256 recorded above. Final ordinary A/B runs on the available local
configuration were byte-identical with that same size and hash. A/B evidence
for the historical M35-compatible frontier remains unavailable because the
supplied recovery set does not reach it.

## Pi verification and remote CI

The repository Pi verifier was launched against the exact final code
checkpoint in a clean detached worktree. It remained in a silent HTTPS wait for
18 minutes and was terminated without producing a report or finding. This is
recorded as verifier unavailable; it is not treated as a review approval.

The heavy remote CI lane passed on the final code checkpoint: Change
classification, Linux/GCC, Linux/GCC with LLVM 18, ASan/UBSan, TSan,
Windows/MSVC, and CI Gate all succeeded. The light documentation lane was
correctly skipped for the executable checkpoint. The allowlisted
documentation-only follow-up then classified the delta as light, passed its
documentation and repository-invariant checks, skipped all five heavy jobs,
and passed CI Gate.

## Privacy and dependencies

No game assets, private reports, credentials, local configuration, absolute
personal paths, or build outputs are tracked. No new project dependency was
introduced. The worker pool uses the C++ standard library only.

## Documentation

M36 updates `docs/AARCH64_SUPPORT.md` and `docs/performance-profiling.md` with
the scalar semantic boundary, batching, reuse, worker, and determinism rules.

## Remaining bottleneck and recommended M37 target

The measured M36 real bottleneck cannot be established until a compatible
private module set reaches refinement. On the synthetic/refinement path, the
next measured target is process entry-index reconstruction and any remaining
large allocations during publication. M37 should first address that index and
publication cost, then resume real scalar-integer convergence only after a
compatible input set crosses the current unresolved-provider boundary. M37
implementation is intentionally not started here.
