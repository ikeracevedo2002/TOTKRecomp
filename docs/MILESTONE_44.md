# Milestone 44 — Deterministic frontier continuation and reuse

**Status:** implementation and real validation checkpoint; final CI/A-B sign-off remains
pending.

## Contract and provider state

Before the implementation change, the private four-module manifest (`rtld`, `main`,
`subsdk0`, `sdk`) was validated for the exact build, identity, load order, coherence,
and expected frontier. The unchanged baseline reproduced 32,265 executed guest
instructions and `unknown_guest_function` at `main + 0x3a2650`.

The provider/completeness policy was not weakened. Current evidence resolves
`__nnmusl_init_dso` to the guest `sdk` module; runtime fallback remains forbidden
unless the existing complete-provider checks authorize it.

## Changes

- Classify only proven identical closed CFG suffixes as legal shared code. Other
  precise overlaps remain typed ownership conflicts.
- Preserve the stopped CPU/interpreter/frame state and continue an exact indirect
  boundary after immutable map publication; the guest prefix is not replayed.
- Share immutable finalized CFG objects across map generations. Boundary-dependent
  records are still invalidated and reanalyzed.
- Incrementally validate only records structurally identical to an already frozen
  map when their complete invariants are available; the ordinary public validator
  remains full.
- Keep process-map lookups authoritative after module replacement; no dangling
  module-map pointer is retained by an execution session.

## Real result under the unchanged contract

| measurement | baseline | post-fix |
| --- | ---: | ---: |
| wall time | 1,201.59 s | 564.45 s |
| executed guest instructions | 32,265 | 74,803 |
| stop reason | `unknown_guest_function` | `memory_fault` |
| stop location | `main + 0x3a2650` target frontier | `main + 0x62c1a4` |

The post-fix run completed below the declared 600 s budget. Its exact terminal
frontier is 74,803 guest instructions at `main + 0x62c1a4`, with diagnostic
`atomic guest access is not naturally aligned`. The original target was promoted
at generation 743 as `shared_tail`; the next shared-tail target was promoted at
generation 744. The real run then crossed 1,517 consecutive productive
promotions before the later typed memory boundary.

Profiled post-fix wall time was 563.408 s: refinement 472.653 s (83.9%), candidate
assessment 53.416 s, map publication 42.065 s, guest execution 45.309 s, and report
generation 15.397 s. `/usr/bin/time -lp` measured 564.45 s real, 671.42 s user, and
16.65 s system time.

The run accounted for 1,517 map rebuilds, 1,517 promotions, 1,147,097 candidate
assessments, 1,145,580 generation-dependent reassessments, 4,492 boundary
reconciliations, 3,112 analyzed functions, 6 reanalyzed functions, 3 invalidated
records, and 1,180,449 reused finalized CFGs. There were no failed or rolled-back
promotions. The worklist ended with no pending candidate and no budget exhaustion.

## Structural and regression validation

The non-proprietary M22 fixtures cover legal shared tails, illegal overlap, strong
entry splitting, false/internal candidates, convergent shared fragments, seed and
worker determinism, incremental/clean equivalence, exact continuation without
prefix replay, and immutable lift reuse. M22, M27, and M37 focused tests pass;
the complete normal suite passes 420 test cases and 37,802 assertions.

Private manifests, executable content, local configuration, real-run reports, and
machine-specific timing files remain outside version control.
