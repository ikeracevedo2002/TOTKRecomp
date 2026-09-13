# Structural debt and the next milestone

Status: proposal only. No production code, budget, or frontier claim. This
records the two structural defects found during the 2026-09-13 repository
cleanup that were deliberately **not** implemented in that cleanup, so they are
not lost and are not smuggled into a hygiene change.

## Why these were not implemented during cleanup

The cleanup PR carried a semantic checkpoint (M42/M43), the operating-contract
rewrite, and the measurement-contract verifier. Folding a production-code
change into that range would have mixed a risk-carrying edit with a hygiene
change, which is exactly the mixing that made earlier milestones hard to review.
Both items below are scoped so they can be taken independently.

## Debt 1 — coverage duplicates the lifter predicate with no drift detector

`analysis/scan_coverage` decides liftability through two local predicates,
`common_liftable` and `fp_simd_liftable`, in `src/switchrecomp/analysis/coverage.cpp`.
They duplicate `lifter::is_instruction_liftable` in
`src/switchrecomp/lifter/lifter.cpp`. The file states the reason: the analysis
library must not depend on the lifter, because the lifter links analysis.

That duplication already cost a milestone. MILESTONE_38.md records that the
scanner "used a stale liftability heuristic that disagreed with the real lifter"
and omitted UMULH/SMULH, atomics, barriers and MRS/MSR. The whole of M38's
scanner repair exists because of this coupling.

The part that makes the debt recur is the missing guard. Milestone tests assert
`lifter::is_instruction_liftable(decoded)` for a hand-picked list of encodings
and separately assert `report.liftable == words.size()` from the scanner. Both
pass for the chosen encodings and neither compares the two predicates over the
opcode space, so the two implementations can disagree on any form that no test
enumerates. The suite therefore cannot detect the exact regression class that
M38 had to repair.

**Scoped fix.** Enumerate every `InstructionId` and every `SimdOperation` with a
representative decoded form and assert, per form, that the coverage predicate
and the lifter predicate return the same value. Test-only: it changes no
behaviour and cannot introduce a semantic regression. It converts an
M38-class drift from "discovered after a full scan" into a failing assertion.

**Better fix, larger.** Move liftability into a layer both analysis and lifter
depend on (decoder, or a new leaf library) so there is one predicate instead of
two. This breaks the dependency cycle rather than guarding it. It is a real
refactor touching `analysis`, `lifter`, and the target graph, so it should be
its own milestone with a full A/B, not a side edit.

Recommendation: take the scoped fix first; it is cheap, and it is also the
safety net the better fix needs.

## Debt 2 — the provider/completeness boundary is the real blocker

Real guest execution stops at `__nnmusl_init_dso`. This is not a missing
instruction: `ProcessSymbolNamespace::lookup` returns
`ProviderSearchIncomplete` before inspecting candidates whenever the module set
is `Incomplete`, and the local configuration declares
`provider_search_complete: false` with `coherence: "unverified"`. A candidate in
`sdk` is already found and discarded by policy.

Refusing the candidate is **correct** behaviour for an unverified module set and
must not be weakened, stubbed, or routed around. The way to clear the boundary is
evidence, not code: a manifest-verified complete module set (sizes, digests, load
order, exact-build coherence) that promotes completeness to
`ManifestVerifiedComplete` on proof rather than on assertion.

`scripts/verify_measurement_contract.py` now fails a milestone whose input
drifts from a frozen contract. It verifies the input; it does not, and should
not, certify that the module set is complete. Completeness proof is the missing
piece and is the highest-value capability work available.

Until that boundary moves, instruction-family work lowers the static diagnostic
only. Per the progress metric in `AGENTS.md`, that is coverage work, not
capability work, and must not be reported as a frontier advance.

## Measured costs worth keeping in view

| Item | Measured |
| --- | --- |
| `ctest --test-dir build -j 6` | 35.74 s, 416/416 passed |
| Serial full suite reported in milestones | ~97 s |
| Cold configure+build, 251 targets | ~60 s wall |
| Coverage scan `main`, parallel / serial | ~60 s / ~230 s |
| Documentation-only CI via light lane | ~7 s, all five heavy jobs skipped |
| Heavy CI matrix | ~25 min runner time |

The light lane was observed working during the cleanup: a documentation-only
merge skipped all five heavy jobs and passed `CI Gate`, because the predecessor
SHA had a completed successful gate on the same PR.

## Environment repairs recorded here

- The ignored local content directory was untracked but not ignored; a broad add
  could have staged private binaries and private real-execution reports. It is
  now ignored, and the working tree is protected from destructive cleanup by
  rule.
- `cmake` and `ctest` were broken: the pip `cmake` package had been removed
  while its wrapper scripts remained in `/tmp/TOTKRecomp-tools/bin`, so every
  invocation raised `ModuleNotFoundError`. Restored (CMake 4.4.3). The directory
  is not on the default `PATH`; export
  `PATH="/tmp/TOTKRecomp-tools/bin:$PATH"` before building.
- The local toolchain has no LLVM, so `TOTKRECOMP_ENABLE_LLVM=OFF` and
  `codegen/llvm_backend.cpp` is never compiled locally. LLVM lowering changes are
  validated only by the remote LLVM 18 job. Treat "backend updated" as unverified
  until that job has run.
