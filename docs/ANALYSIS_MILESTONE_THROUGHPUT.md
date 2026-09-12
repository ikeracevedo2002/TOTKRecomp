# Milestone Throughput Analysis

Status: analysis only. No production code, semantic behavior, budget, or
frontier was changed. This document is a planning artifact and is not part of
the milestone stack.

Scope: explain where milestone wall-clock time is going, why capability
progress per milestone is small, and what changes would most reduce
time-to-frontier. All numbers below are derived from the committed milestone
documents, the Git history, and the GitHub Actions run history.

## 1. Executive summary

The project has produced a large, rigorously validated codebase in a very short
calendar window (36+ milestones in about 8 days), but the *measured guest
execution frontier* advances far slower than the process volume suggests. The
last reproducible real frontier recorded before the input-set drift was about
`18,897` guest instructions (M34), and M36 could not reproduce it at all.

The five dominant losses, in order of impact:

1. **The real workload is refinement-bound, not execution-bound.** In the M35
   real run, refinement consumed `1191.78 s` of roughly `1221 s` total
   (~97.6%). Guest execution is a rounding error. Every real measurement
   milestone therefore inherits a ~20-minute wall time, doubled for the
   required byte-identical A/B determinism run.
2. **The private real input set is not stable across milestones.** M36 documents
   that the available local recovery configuration no longer reproduces the M35
   frontier and stops earlier at an unresolved `__nnmusl_init_dso` provider
   search (63 guest instructions instead of ~18,897). A full milestone of work
   was then left unmeasured and unverified.
3. **The frontier is converged one blocker at a time.** Each milestone typically
   implements the single next unsupported instruction/event and stops. M34
   crossed a resource ceiling and advanced ~250 instructions before stopping at
   one `fmov v1.2s, #imm`. M35 then implemented `Fmov`, `St1`, and `Movi`.
   Family-level coverage gaps are discovered only at runtime.
4. **Milestone inflation and accounting-only milestones.** The authoritative
   roadmap defined 20 milestones. The working history contains 36. M25–M34 are
   dominated by termination proofs and resource-accounting conversions
   (transactions, events, candidate cardinality, transitions) rather than new
   capability.
5. **Verification ceremony scales linearly with milestone count, not with
   risk.** Each milestone carries a 100–460-line document, a deterministic
   double real run, a five-job CI matrix, full ASan/UBSan, TSan (~18 minutes),
   and up to two Pi review rounds.

Highest-leverage actions: stabilize and freeze the measurement input, drive
refinement cost down with measured profiling (M36 already started this), batch
semantic families using a static unsupported-opcode survey of the real module,
and separate "capability" milestones from "verification/resource" work.

## 2. Baseline metrics

### 2.1 Timeline and volume

| Metric | Value |
| --- | --- |
| First architecture commit | 2026-09-05 14:15 |
| Latest milestone work observed | 2026-09-12 15:55 |
| Calendar span | ~7 days, 2 hours |
| Milestones referenced by branch/doc | 0 through 36 |
| Milestone branch tips ahead of `origin/main` | most are 0 (merged), M35 is +14 |
| Commits on the M35 lineage | 165 |
| Merged PRs | 22 |
| Open PRs | 18 (including M3–M16 from 2026-09-06) |
| Milestone documentation | 6,839 lines across `docs/MILESTONE_*.md` |
| Operating contract | `AGENTS.md` (76) + `docs/ENGINEERING_WORKFLOW.md` (114) |

Note: `origin/main` only merged the M23/M24 chain plus infrastructure on
2026-09-11, even though branches up to M35 existed. The stack is effectively a
long linear chain, not a trunk-based flow.

### 2.2 Real-run wall time

| Milestone | Real-run observation |
| --- | --- |
| M23 | Analysis retry was CPU-bound for 10+ minutes and was stopped before execution; no new real frontier recorded |
| M26 | Standard suite 282 tests in 54.98 s |
| M34 | Real run reached 18,897 guest instructions, stop `unsupported_instruction` (`fmov v1.2s, #imm`) |
| M35 | Two comparable real runs at ~1221.28 s and ~1234.68 s; refinement 1191.78 s (~97.6%); 728 rounds, 727 rebuilds |
| M36 | Available input stops at 63 instructions (`unresolved_import`); 80.30 s wall; zero refinement transactions; no compatible measurement |
| M36 tests | Standard 382 tests in 97.19 s; ASan/UBSan 377.34 s; TSan 1068.97 s |

The M36 document states the target explicitly: the private run is "preferred
sub-600-second", which confirms the team already knows wall time is the gate.

### 2.3 CI cost

From 259 `CI` workflow runs (2026-09-05 → 2026-09-12):

| Metric | Value |
| --- | --- |
| Total runs | 259 |
| Successful | 196 |
| Failed | 63 |
| Total runner time | ~20.6 hours |
| Median run | ~4.8 minutes |
| Longest run | ~9.1 minutes |
| Runs on 2026-09-06 alone | 127 (54 failed) |
| Failure concentration | M6 minimal-lifting (19), M9 threads-atomics (19), M6 semantic-ir (12), M0 bootstrap (8) |
| Distinct SHAs with CI | 141 |
| SHAs with >2 runs | 10 (12 redundant runs) |
| Push vs PR runs | 146 push / 113 PR |

The 54 failures on day one were early bootstrap churn and are not representative
of steady state. The steady-state cost is the five-job matrix on every push and
PR, plus duplicate push/PR execution of the same SHA (documented as an accepted
duplicate in M34).

### 2.4 Frontier progression (capability per milestone)

| Point | Guest instructions | Notes |
| --- | --- | --- |
| M22 | 390 | `main:0x7200034bd0`, refinement-budget stop |
| M23 | 390 (no new) | Analysis never reached execution |
| M26 | 3,832 → 6,186 | Refinement scaling milestones |
| M32/M33 | — | Crossed an indirect-target refinement frontier |
| M34 | 18,897 | Stop at single `fmov v1.2s, #imm` instruction |
| M36 | 63 | Input drift; frontier not reproduced |

Broad strokes: M22 → M34 moved the frontier from 390 to 18,897 instructions
over roughly a dozen milestones. That is real progress, but it is still an
extremely small fraction of a shipping title's instruction count, and the last
milestone could not reproduce it.

## 3. Where the time actually goes

### 3.1 Refinement engine rebuild-per-candidate (largest single cost)

The M35 profile shows `727` productive rounds and `727` rebuilds. Every
promoted indirect target triggered a target-module CFG revalidation and
immutable map publication. The M26/M27/M28/M32/M33 milestones repeatedly
converted *one* refinement dimension at a time from a fixed ceiling into a
semantic resource. The M36 work (batch publication, identity reuse of untouched
maps, parallel assessment workers, `--analysis-workers`) directly attacks this
and is the correct direction, but its real effect is currently unmeasured.

### 3.2 Unstable/undocumented measurement input

The real run depends on a locally recovered four-module set under `local/`
(untracked) plus `config/local.m17.local.json`. M36 reports that the recovery
set changed provenance and stops earlier. Consequences:

- A milestone can implement a correct semantic improvement and still be unable
  to prove it advanced the real frontier.
- Reviewers cannot reproduce the frontier without the same private files.
- Whole milestones get consumed re-establishing the input (M23 and M36 are both
  examples of "ran, could not reach the frontier").

### 3.3 One-blocker-at-a-time semantic convergence

The decoder/lifter already have broad coverage, but gaps surface only when the
real run reaches them. Recent examples that each cost milestone time:
`umulh` (M19), `smulh` (M21), `move-wide` (M19), vector `fmov` immediate,
`st1`, `movi` (M35), scalar `lsl/lsr/asr/ror/extr/ubfm/sbfm/bfm` (M36). Each is
found serially at the end of a 20-minute run.

### 3.4 Process ceremony

For one milestone the required loop is: implement → focused tests → full suite
(97 s) → ASan/UBSan (377 s) → TSan (1069 s) → two byte-identical real runs
(~40 min) → 100–460-line milestone document → Pi round 1 → fixes → possibly Pi
round 2 → five-job remote CI. The ceremony is correctness-oriented and largely
justified, but its cost is paid per milestone, so shrinking milestone
granularity multiplies overhead.

### 3.5 CI and merge mechanics

- Five-job matrix on every push and PR of the same SHA.
- 18 open PRs, including M3–M16 superseded by the chain that was eventually
  merged; the linear stack means each merge depends on the previous branch.
- `origin/main` lagged the real work by ~12 milestones, so "latest" is
  ambiguous and integration risk accumulates.

## 4. Root causes

1. **Measurement before optimization:** there was no fixed, checksummed workload
   contract that every milestone must reproduce. Without it, regressions and
   input drift are invisible until a milestone fails to reach the frontier.
2. **Resource-accounting framing:** milestones were scoped around removing
   arbitrary budget constants rather than around reaching the next *game*
   capability. The proofs are rigorous but do not move the game forward.
3. **Runtime-driven semantic discovery:** coverage is discovered by hitting a
   wall, not by statically enumerating what the exact binary actually uses.
4. **Serial milestone stack:** one branch stacked on the previous one forked
   from a stale `main` prevents parallel workstreams and clean integration.
5. **Averaging over risk:** the same validation weight is applied to a
   documentation-only accounting milestone and to a new instruction decoder.

## 5. Recommendations (prioritized)

### P0 — Freeze the measurement workload (this week)

Create a committed *contract* for the private input without committing the
binary: a manifest of module names, sizes, and SHA-256 hashes plus the recovery
procedure version, validated by the existing manifest tooling. Make
"reproduces frontier X" a precondition for starting a milestone. If the input
cannot be recovered, the milestone is blocked, not reinterpreted.
Expected impact: eliminates M23/M36-style lost milestones. Effort: low.

### P0 — Finish and measure refinement throughput

M36's batching + incremental reuse + parallel assessment is the right lever and
addresses ~98% of real-run wall time. Block the next capability milestone until
this is measured against the frozen workload and the run is under a stated
target (the M36 doc suggests sub-600 s). Add the remaining known bottleneck
(process entry-index reconstruction, large publication allocations) to the same
pass. Expected impact: potentially 2–4× on every future real measurement.
Effort: medium.

### P1 — Static unsupported-opcode survey of the real module

Before the next run, scan the exact `main` (and providers) for every decoded
instruction whose lifter currently traps. Emit a ranked report of opcode
families by frequency and by distance from the entry. Then implement whole
families (all vector-immediate forms, all single-structure loads/stores, all
scalar shift/bitfield forms) in one milestone instead of one instruction per
milestone. Expected impact: removes most serial frontier stops.
Effort: medium, high payoff.

### P1 — Separate capability milestones from verification milestones

Define two milestone classes:

- **Capability milestones** advance the real frontier or a subsystem, and
  require the full real A/B + review.
- **Verification/resource milestones** (ledger conversions, observability,
  termination proofs) do not open a new milestone number; they are checkpoints
  inside a capability milestone. This directly reverses the 20→36 inflation.

Expected impact: fewer documents, fewer CI cycles, clearer progress narrative.
Effort: process-only.

### P2 — Right-size validation to risk

- Run the full byte-identical real A/B only once per capability milestone, at
  the final checkpoint; use one profiling run earlier.
- Run TSan nightly or on semantic changes, not on every checkpoint.
- Extend the existing light CI lane so docs/accounting deltas skip the heavy
  matrix (already designed; verify it is being used).
- Remove the duplicate push+PR run for the same SHA (the M34 handoff already
  acknowledges it exists).

Expected impact: several hours per week of runner time. Effort: low.

### P2 — Drain the PR/merge debt

Close superseded M3–M16 PRs explicitly (they are already contained in the
merged chain) and record the lineage. Move to short-lived branches based on
`origin/main`, with at most a one-milestone stack. This restores a single
"latest" and allows parallel safe work (renderer/runtime scaffolding can start
in parallel with semantic convergence once the CPU path is stable).

Expected impact: lower integration risk, enables parallelism. Effort: low.

### P3 — Throughput dashboard

Add a generated, non-committed report from `run-entry` profiling with: wall
time, refinement vs execution split, frontier PC/instruction, rounds/rebuilds,
batch width, reuse counts. Track it per milestone so regressions are obvious.

## 6. Proposed milestone operating changes

1. One capability outcome per milestone, phrased as "reach X" or "make family Y
   execute", never "remove budget Z".
2. Workload contract frozen and hashed before implementation starts.
3. Frontier measured with one profiling run after implementation; A/B
   determinism only at final checkpoint.
4. Accounting/proof work rides inside the capability milestone.
5. Real A/B target: state a wall-time budget and treat exceeding it as a
   blocking defect, not a note.
6. No more than one milestone on a branch stack; branch from current `main`.

## 7. KPIs to track

| KPI | Current | Near-term target |
| --- | --- | --- |
| Real-run wall time | ~1221 s (M35) | < 600 s |
| Refinement share of wall time | ~97.6% | < 80% |
| Rebuilds per productive round | 1.0 (M35) | < 0.2 (batch) |
| Guest instructions to frontier | 18,897 (M34) | monotonic; ≥ 100k |
| Milestones per capability outcome | ~1.8 | ≤ 1.0 |
| Docs lines per milestone | ~250 avg | ≤ 80 |
| Redundant CI runs (same SHA >2) | 12 | 0 |
| Open PRs | 18 | < 5 |
| Sanitizer minutes per checkpoint | ~24 min (ASan+TSan) | overnight/on-demand |

## 8. Appendix: evidence and reproduction

Data sources:

- `docs/MILESTONE_23.md`, `_26.md`, `_33.md`, `_34.md`, `_35.md`, `_36.md`
  (wall times, frontier PCs, profiles, input drift).
- `git log origin/main` and `git rev-list --count` per milestone branch.
- GitHub Actions: `gh run list` / `gh api .../actions/runs`, 259 `CI` runs.
- `AGENTS.md`, `docs/ENGINEERING_WORKFLOW.md` (required ceremony).

Key commands used:

```bash
gh api "repos/ikeracevedo2002/TOTKRecomp/actions/runs?per_page=100&page=N"
git log origin/main --first-parent --date=iso
git rev-list --count origin/milestone-35-fp-simd-semantic-convergence
wc -l docs/MILESTONE_*.md
gh pr list --state open|merged --limit 100
```

Caveats:

- Wall-clock figures are self-reported in milestone documents; CI run durations
  combine all five jobs and therefore overstate the critical path per job.
- The M36 input-set drift means the most recent real-frontier number (18,897)
  is from M34, not M36.
- This analysis deliberately does not evaluate semantic correctness; the
  existing suite and review process remain authoritative.
