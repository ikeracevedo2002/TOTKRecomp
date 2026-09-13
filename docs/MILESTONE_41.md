# Milestone 41: AdvSIMD structure loads

## Scope and baseline

M41 continued from the completed M40 semantic-convergence worktree. The local
`main` coverage baseline was:

```text
                         decoded    liftable   unsupported   decode failures
main                     11180285   11154198        26087              391
```

Capstone-backed survey counts identified LD1 (2,424), LD1R (1,713), LD2 (83),
and LD4 (22) dynamic instances: 4,242 measured structure loads in total. LD3
was not observed in this image, but its matching normalized forms were
implemented with the same architectural machinery.

## Semantics implemented

- LD1 supports one-to-four consecutive vector destinations and single-lane
  loads. LD1R, LD2/LD2R, LD3/LD3R, and LD4/LD4R are normalized as explicit
  AdvSIMD operations.
- B/H/S/D arrangements are decoded from Capstone and the architectural fields
  when Capstone leaves a single-lane arrangement unspecified. Both 64-bit and
  128-bit arrangements are handled.
- Non-replicating structures consume memory in element-major structure order
  and deinterleave elements into the destination vectors. Replicating forms
  load one element per destination and broadcast it across that destination.
- Vector lists are validated as consecutive modulo 32, so V31-to-V0 wrapping is
  architectural while malformed lists remain unsupported.
- Single-lane and short 64-bit loads preserve lanes not written by the
  instruction. All element accesses use checked little-endian `GuestLoad`
  operations and the existing address-add path.
- Immediate and register post-index addressing update the base register after
  the data loads. Capstone's omitted structure-load immediate is recovered
  from the arrangement and structure width; register post-index uses the X
  offset register.

The direct lifter, mirrored coverage predicate, IR verifier, interpreter, and
LLVM lowering use the same explicit composition. Unsupported arrangements,
addressing modes, destination lists, and register widths remain structured
errors rather than being accepted through a decoder-only shortcut.

## Tests and validation

`tests/milestone41_structure_load_tests.cpp` covers decoder normalization and
coverage classification, every LD1/LD1R element width, lane and upper-lane
preservation, LD2/LD4 deinterleaving, register-list wrapping, immediate
writeback, and register post-index writeback. The focused result was **120 assertions in 3 cases**.

The complete standard suite passed with **37,527 assertions in 408 cases**.
The optional LLVM target was disabled in this local build; its backend lowering
was updated alongside the interpreter.

## Final coverage

```text
                         decoded    liftable   unsupported   decode failures
main                     11180285   11158476        21809              391
```

The 4,242 surveyed structure-load instances were all classified as liftable.
The decoder normalization shared by single-lane AdvSIMD memory forms also
recovered 36 previously misclassified ST1 lane instances, for a total
unsupported-count improvement of 4,278:

```text
26087 -> 21809  (4,278 fewer; 16.3990% reduction; 0.195067% of decoded instructions remain unsupported)
```

The bounded four-worker and serial coverage JSON reports were byte-identical.

The remaining unsupported ranking is:

```text
fp_simd       15696
unknown        6104
msr               5
mrs               4
```

The remaining `fp_simd` frontier is intentionally deferred. The next measured
families should be selected only after their complete lane, fault, and FP
architectural semantics are specified and tested.
