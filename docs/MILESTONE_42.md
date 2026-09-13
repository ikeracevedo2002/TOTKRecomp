# Milestone 42: AdvSIMD structure stores

## Status and provenance

M42 implements the AdvSIMD structure-store family, the store-side counterpart
of the M41 structure-load family.

This document was written **after** the implementation, during repository
cleanup. M42 was never committed on its own: its code lived only as
uncommitted working-tree edits in the same files the later M43 AdvSIMD work
edited, and the two sets of changes are interleaved inside `lifter.cpp` (a
single hunk at the structure-memory branch contains both families). Splitting
them into two commits would create an intermediate commit that was never
compiled or tested, so M42 and M43 are recorded as one checkpoint commit and
this document is the separate record of the M42 scope.

The numbers below are derived from the M41 result recorded in
`docs/MILESTONE_41.md` and the M42 baseline recorded in
`docs/MILESTONE_43.md`. No new measurement was invented to fill the gap.

## Scope

Structure stores write one to four consecutive vector registers to memory in
structure order, interleaving elements on the way out. Implemented forms:

- `ST1` for one through four destination-consecutive operands, including single
  lane stores, across B/H/S/D element widths and both 64-bit and 128-bit
  arrangements.
- `ST2`, `ST3` and `ST4` interleaving, including modulo-32 vector-list wrapping
  through V31 to V0.
- `ST1R` replicating forms.
- Offset forms: no offset, immediate post-index, and register post-index, with
  the base register updated after the data stores. The register form accepts an
  X-form offset register.
- Single-lane store forms preserve the lanes not written.

As in M41, malformed arrangements, non-consecutive destination lists, and
unsupported addressing modes remain structured errors rather than being
accepted through a decoder-only shortcut. The direct lifter, the mirrored
coverage predicate, the IR verifier, the interpreter and the LLVM lowering use
the same explicit composition.

## Tests

`tests/milestone42_structure_store_tests.cpp` contains four cases:

- decoder, lifter and coverage agree on ST1 through ST4 forms;
- ST1 stores sequential vectors for every lane element width;
- ST2, ST3 and ST4 interleave elements and wrap vector lists;
- structure-store writeback uses Xn, register offsets and SP.

These cases run as part of the single `switchrecomp-tests` executable. M42 did
not record an isolated focused assertion count; its assertions are included in
the combined M43 checkpoint figure reported in `docs/MILESTONE_43.md`.

## Coverage contribution

```text
                         decoded    liftable   unsupported   decode failures
M41 final                11180285   11158476        21809              391
M42 baseline (M43 doc)   11180285   11158677        21608              391
```

Measured contribution: 201 additionally liftable instructions, from the
previously misclassified structure-store forms. The prepared-main decode total
and decode-failure count are unchanged, so the improvement is a reclassification
of already-decoded instructions, not a decoding win.

## Known gaps carried out of this milestone

- `docs/MILESTONE_42.md` did not exist until this cleanup. Written retroactively
  here so the sequence has no silent hole.
- The M41 and M42 branch numbering had already drifted from the document
  numbering (see the lineage rules in `AGENTS.md`).
- Branch naming at the time of this checkpoint contradicted its contents: the
  branch carrying M41 through M43 was still named after milestone 39.
- The optional LLVM backend is not compiled by the local build used for the
  checkpoint, so the M42 lowering changes are locally unverified.

## Frontier

No real frontier change is claimed. Real guest execution remains blocked at the
`__nnmusl_init_dso` provider boundary, so the real guest instruction count is
unchanged by this milestone. M42 lowers the static coverage diagnostic only.
