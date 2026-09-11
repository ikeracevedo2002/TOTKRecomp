# Milestone 26: Progress-Aware, Evidence-Preserving Indirect Refinement Closure

## Status

Complete, subject to the final-head sanitizer and CI checks recorded below.

M26 removes the artificial productive-round ceiling. It does not weaken function-entry certification, add a target seed, force guest control flow, or enlarge the default refinement limits.

## Exact Git lineage

- Stacked base: `milestone-25-resumable-m9-interpreter-parity`
- Base SHA: `b8608e04c7d65e2702cc47ca821b202af248fb2e`
- Branch: `milestone-26-progress-aware-indirect-refinement`
- Implementation commit: `756cd66`
- Final head: the documentation commit at the branch tip
- PR: stacked against `milestone-25-resumable-m9-interpreter-parity`

The source branch was created directly from the exact M25 SHA and was not rebased onto `main`.

## Baseline

The pre-edit local baseline was `274/274` tests, using Apple Clang 17, CMake 4.4.2, Debug configuration, and the existing Unix Makefiles build with LLVM disabled.

## M25 reproduction

The ignored private executable-set configuration and the normal `run-entry --entry dt-init` path reproduced the M25 report byte-for-byte. The M25 report hash was:

`a2e47ac928f07bc35958880e2e5f1c69b38847c7fa29add90e8e73e327590735`

The run stopped at `indirect_target_refinement_budget_exceeded`:

- source: `main:0x7200000148`, `blr x8`
- runtime target: `main:0x72000c6aa0`
- pointer provenance: guest load at `main:0x720456ad38`
- guest instructions: 3832
- refinement rounds: 64
- unique candidates: 64/256
- candidate assessments: 64/512
- promotions: 64/128
- map rebuilds: 64/128
- pending candidates: 0
- productive rounds: 64
- stagnant/retry rounds: 0
- rejected candidates: 0
- reconsiderations: 0

The exhausted dimension was the old total `max_rounds` limit, not a candidate, assessment, promotion, map-rebuild, CFG, or execution limit.

## Forensic root cause

The old `run-entry` driver called `begin_round()` before every complete execution from the original entry. Each execution observed the next indirect candidate, and a successful refinement caused the driver to `continue`, so the next execution charged another round. Consequently, one valid sequential promotion consumed one unit of the total-round limit. At 64 rounds, all 64 rounds had made monotonic progress; no round had repeatedly reconsidered an unchanged rejected candidate, and duplicate observations were coalesced.

The M25 target was assessed and promoted during the last allowed iteration, but the driver could not start the next execution to enter it. Therefore the old round budget, rather than certification, prevented the next guest dispatch.

## Progress-aware refinement model

M26 changes the public refinement semantics to schema 12. Total execution attempts are accounting only. The anti-spin allowance is explicitly `max_stagnant_rounds` (the old `--refinement-max-rounds` remains only as a deprecated CLI alias for that no-progress budget).

The worklist now distinguishes:

- total execution attempts;
- productive rounds, including new candidates, terminal transitions, trusted-entry hits, reconsiderations, and promotions;
- stagnant rounds, which make no monotonic transition;
- observations and coalesced duplicates;
- assessments, terminal resolutions, promotions, and map generations.

Every attempt ends with `end_round()`. Productive attempts do not consume the stagnation allowance. Only unchanged no-progress retries increment it.

## Finite termination argument

- New candidate identity is bounded by `max_unique_candidates`.
- Candidate assessments are bounded by `max_candidate_assessments`.
- Promotions are bounded by `max_promotions`.
- Immutable map generations/rebuilds are bounded by `max_map_rebuilds`.
- Terminal candidate transitions remove unresolved work and are bounded by the finite candidate set.
- Duplicate observations coalesce and do not create new work.
- Any attempt making none of those transitions consumes the finite `max_stagnant_rounds` allowance.

Thus productive refinement is bounded by finite monotonic worklist dimensions, while non-productive reruns are independently bounded. The driver cannot retry indefinitely. The exact configured limits remain 256 unique candidates, 512 assessments, 128 promotions, 128 map rebuilds, and 64 stagnant rounds.

## Immutable refinement

`refine_process_function_map()` remains transactional. It constructs a new frozen process map and publishes it only after validation succeeds; the prior generation remains untouched on failure.

M26 reuses unchanged finalized module maps and rebuilds only the target module. Deterministic module ordering and cross-module ownership/provider validation remain in `ProcessFunctionMap::build()`. The private run recorded 128 module maps rebuilt and 128 unchanged module maps reused.

Stable process-image indexes for function-target and relocation references avoid repeatedly scanning unrelated process metadata while preserving guest-side provenance. A trusted existing target can use its frozen function evidence without bypassing the normal certification path.

## Synthetic validation

`tests/milestone26_progress_refinement_tests.cpp` adds eight deterministic cases:

- 96 independently certified productive promotions under default limits;
- exact no-progress/stagnation exhaustion;
- productive progress without stagnation charging;
- terminal rejection without promotion or endless reruns;
- duplicate observation coalescing and visible provenance/counting;
- map-generation-dependent reconsideration;
- permutation-stable serialized state;
- transactional target-only map rebuilding, unchanged-module reuse, provider lookup, and rollback on failure.

The focused M26 test run passed 588 assertions in 8 cases. The M18, M20, and M23 focused regressions also passed.

## Real executable result

The final private workflow was run twice with the ordinary defaults and no refinement CLI overrides. Both runs completed through 129 total execution attempts before the next genuine resource boundary:

- guest instructions: 6186 (`+2354` versus M25)
- guest blocks: 1084
- function transitions: 42
- maximum call depth: 3
- direct calls: 146
- indirect calls: 124
- returns: 269
- runtime fallbacks: 0
- total execution attempts: 129
- productive rounds: 129
- stagnant rounds: 0
- observations received: 22491
- unique observations: 436
- unique candidates: 129
- candidate assessments: 129
- terminal resolutions: 0
- successful promotions: 128
- map rebuilds: 128
- final map generation: 128
- module maps rebuilt: 128
- module maps reused: 128
- reconsiderations after generation changes: 0
- pending work: 1

No forced jump, forced PC, target seed, or budget enlargement was used.

## Former M25 frontier

- source module: `main`
- source PC: `0x7200000148`
- instruction: `blr x8`
- target: `main:0x72000c6aa0`
- pointer provenance: guest load at `main:0x720456ad38`
- assessment reached: yes
- certification result: certified, high confidence; observed indirect call plus verified rebased guest slot and bounded CFG
- promotion result: normal immutable promotion, generation 63 to 64
- guest target entered: yes
- first guest instruction: target PC `0x72000c6aa0`, opcode `2847833085`
- forced jump: no
- forced seed: no
- budget override: no

The target was dispatched through the normal guest path. `R_AARCH64_RELATIVE` provenance was used as rebasing evidence, not as automatic function metadata.

## New exact frontier

The next frontier is the distinct configured promotion/map-rebuild resource, not the former total-round ceiling:

- source module: `main`
- source PC: `0x7200000148`
- instruction: `blr x8`
- target: `main:0x72000f04f0`
- pointer provenance: guest load at `main:0x720456af40`
- assessment reached: yes
- certification result: certified, high confidence
- promotion result: not attempted because `max_promotions` and `max_map_rebuilds` were both exactly exhausted at 128/128
- guest target entered: no
- forced jump: no
- forced seed: no
- budget override: no
- pending work: one candidate

This is not a second arbitrary productive-round cap: every preceding promotion required a normal target-module CFG revalidation and immutable generation advance, while unchanged module maps were reused. M27 should address that aggregate revalidation resource rather than merely raising a number.

## Determinism

The two final reports were byte-identical, each 28,413,832 bytes:

- report 1 SHA-256: `366bf1fd6ecfe15e880f919501980e9b06e75ea71e8d84b4596199cb063f4a45`
- report 2 SHA-256: `366bf1fd6ecfe15e880f919501980e9b06e75ea71e8d84b4596199cb063f4a45`
- byte comparison: identical

No host timing, PID, pointer address, filesystem order, or temporary path enters report identity.

## Validation

Before edits: `274/274`.

After edits: the standard suite contains 282 tests and passes in 54.98 seconds. ASan/UBSan and TSan each pass all 282 tests with zero findings; the instrumented runs reported no sanitizer or race diagnostics. Final-head CI results are recorded here after completion. The focused M18/M20/M23/M26 runs passed before the full validation pass.

## Dependencies

Expected new dependency count: `0`. M26 uses the existing C++20 implementation and test infrastructure.

## Privacy

No proprietary executable, instruction dump, key, firmware, ignored private configuration, local report, or absolute local path is committed. Private addresses appear only as frontier evidence in this document and are not production constants or synthetic fixtures.

## Exact recommendation for Milestone 27

Do not raise the 128 promotion or map-rebuild limits. Measure and design a proof-preserving aggregate CFG/revalidation resource, with persistent per-module analysis reuse or a batched refinement transaction where correctness permits. The next milestone should begin from the `main:0x72000f04f0` candidate and first determine whether its certification evidence can be completed within that resource model; do not speculate beyond this observed frontier.
