# Milestone 44 — Deterministic frontier continuation and reuse

**Status:** implementation, real validation, final A/B, sanitizer, and remote CI
validation complete.

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
  records are still invalidated and reanalyzed, and process-map lookup remains
  authoritative after module replacement.
- Incrementally validate only records structurally identical to an already frozen
  map when their complete invariants are available; the ordinary public validator
  remains full.
- Bound analysis and refinement CPU parallelism at ten workers. Independent CFG
  waves, relocation validation, and module metadata work publish indexed results
  in stable order; overlapping relocation writes retain the serial transactional
  path.
- Use transactional batch guest-memory mapping, bulk dynamic-symbol/RELA reads,
  immutable preverified lift artifacts, and move-only continuation results. These
  remove measured backing-storage and large diagnostic-copy costs without changing
  the map or report contract.
- Treat the exact-build legacy ARM-state UDF sentinel `0xe7ffdefe` as an explicit
  non-fallthrough trap rather than as a generic decode success. A malformed direct
  callee remains an honest failed record; it does not prevent a separately
  validated observed candidate from being published. Exact failed records are
  never dereferenced as though they had CFGs.
- Clear boundary-only diagnostic fields before resuming a stopped session so a
  later memory fault cannot inherit an earlier indirect-branch description.

## Real frontier chain under the unchanged contract

Every run below used the same manifest-verified four-module set, `dt-init`, ten
workers, and a declared 600 s wall budget. The baseline and each transition are
path-free compact reports in the private runtime directory.

| state | guest instructions | terminal frontier | next stop |
| --- | ---: | --- | --- |
| frozen baseline | 32,265 | `main + 0x3a2650` | `unknown_guest_function` |
| optimized continuation | 74,803 | `main + 0x62c1a4` | `memory_fault` |
| frontier 1 | 76,392 | `main + 0x62cd34` | unsupported `fneg v0.4s, v1.4s` |
| frontier 2 | 78,408 | `main + 0x148` | `unknown_guest_function` target `main + 0x2478740` |
| frontier 3 | 78,491 | `main + 0x233f088` | unsupported failed callee `main + 0xf6948` |
| frontier 4 | 87,919 | `sdk + 0x131870` | `memory_fault` reading TLS-relative `0x1f8` |
| frontier 5 replay | 87,919 | same as frontier 4 | no movement: missing faithful TLS/bootstrap evidence |

The optimized continuation reduced comparable wall time from 1,201.59 s to
564.45 s. Frontier 1 took 490.79 s, frontier 2 took 487.47 s, frontier 3 took
490.18 s, frontier 4 took 316.02 s, and the byte-identical frontier-5 replay
took 316.20 s. The last two historical runs were byte-identical (`912ee0…`); the final
checkpoint A/B pair is also byte-identical with report SHA
`34f1480812eb616e4da5a9cb4fa1f6e6b1eaabbe4630ba198aef82c1067ef848`. The
repeated runs are evidence of deterministic terminal state, not a claimed
frontier advance. Final A/B wall times were 317.08 s and 319.83 s, both below
budget; user/system times were 588.88/5.79 s and 596.48/5.86 s.

The baseline decomposition remains: 851.70 s execution replay, 122.71 s map
builds, 46.17 s conflict processing, 61.64 s validation, 9.65 s report
publication, with the remainder in lifting/verification, candidate handling,
CFG/ownership work, and map reconstruction/copying. The optimized runs also
record bounded worker counts, reuse/copy counters, relocation pair checks, and
zero pending candidates in their private reports. The ten-worker deterministic
A/B pair produced identical frontier/refinement evidence.

Frontier 3 resolves the ownership/function-boundary case without forcing the
call: the candidate at `main + 0x2478740` is promoted from its validated CFG,
while the unrelated malformed direct callee at `main + 0xf6948` stays failed.
Frontier 4 reaches a later, honest TLS/bootstrap boundary. No chosen TLS address,
register value, host stub, or completeness weakening is used.

## Structural and regression validation

The non-proprietary M22 fixtures cover legal shared tails, illegal overlap, strong
entry splitting, false/internal candidates, convergent shared fragments, seed and
worker determinism, incremental/clean equivalence, exact continuation without
prefix replay, and immutable lift reuse. M22, M27, and M37 focused tests pass;
the complete normal suite passes all 424 registered test cases. The complete
TSan suite also passes all 424 cases.

Private manifests, executable content, local configuration, real-run reports, and
machine-specific timing files remain outside version control.
