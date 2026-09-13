# Milestone 39: Coverage-Driven Semantic Convergence

## Status

M39 implements four high-value instruction families selected from the exact
M38 unsupported survey: modified-immediate `MOVI`/`MVNI`, pairwise floating
add `FADDP`, vector bit-select `BIF`/`BIT`/`BSL`, and the architecturally
trapping `UDF` instruction. The implementation is deliberately bounded to
these measured families; no real-runtime frontier or provider work is claimed.

Real TOTK execution remains externally blocked at the unchanged
`__nnmusl_init_dso` provider boundary before useful guest semantic execution.

## Git

```text
Base lineage: milestone-38-coverage-driven-semantic-convergence
Parent: bebe97369c93cdd55ed01864e22e615626e36868
Branch: milestone-39-coverage-driven-semantic-convergence
Merge status: not merged
```

## Measurement contract and selection

The external, non-committed module contract is unchanged from M38:

```text
main    size 36,549,681  decoded 11,180,285
rtld    size      7,140  decoded      1,519
sdk     size  6,132,082  decoded  1,539,998
subsdk0 size  3,428,817  decoded    855,798
```

The accurate M38 independent lifter-driven survey found the following
measured unsupported members worth converging:

```text
module    UDF   MOVI  MVNI  FADDP  BIF  BIT  BSL   family delta
main    18114  17179     9  12445 2928 1768 1606        54049
sdk     22212    756     7      1   17  119   20        23132
subsdk0  5545    575     0      0    0    0    0         6120
rtld       43      0     0      0    0    0    0           43
total   45914  18510    16  12446 2945 1887 1626        83344
```

`UDF` was counted from Capstone's backend mnemonic for the previously
undecoded `unknown` bucket, including immediate variants. `MOVI` was dominated
by the `q=0, op=1, cmode=14` `movi dN` form that the M38 decoder intentionally
left unnormalized. `FADDP` and the three bit-select operations are the largest
remaining semantically coherent FP/SIMD forms. The larger residual `unknown`
and broad `fp_simd:none` buckets remain heterogeneous and are not treated as
one fake family.

## Implemented families

**MOVI/MVNI modified immediates.** The decoder reconstructs the architectural
`imm8` and `cmode` expansion for byte, halfword, word-shift, and MSL forms,
replicates the selected element across the vector, and applies the MVNI
per-element inversion. It recovers `D1`/`D2` when Capstone reports the valid
64-bit MOVI form without an arrangement. The lifter uses the existing typed
`VectorBroadcast` path for exact B/H/S/D lane widths; reserved arrangements are
not claimed as liftable.

**FADDP.** `FADDP` normalizes to its own SIMD operation. The lifter extracts
adjacent f32 or f64 lanes from each source, performs the existing typed FP add,
bitcasts the result back to the corresponding integer lane type, and inserts
pairwise results in architectural order. Supported forms are `2S`, `4S`, and
`2D`; no decomposed fused operation is introduced.

**BIF/BIT/BSL.** The three byte-arrangement bit-select operations normalize to
separate SIMD operations and use the existing lane-aware `VectorBinary` IR:

```text
BIF: D = (D & N)  | (M & ~N)
BIT: D = (D & ~N) | (M & N)
BSL: D = (N & D)  | (M & ~D)
```

Both `8B` and `16B` forms are supported, with the destination's pre-write
value read before the write.

**UDF.** Backend `UDF` instructions normalize to a project-owned instruction
ID and trap control-flow kind. The lifter emits the existing IR trap
terminator, preserving immediate variants and keeping `UDF` distinct from the
still-unsupported `BRK` family. Runtime execution reports a typed trap
boundary rather than fake success.

## Coverage contract

`aarch64-analyze --coverage` continues to use an explicit analysis-side mirror
of `lifter::is_instruction_liftable(DecodedInstruction)` because the analysis
library cannot link the lifter. The mirror now includes the four new families,
the same operand/arrangement predicates, and `UDF`. A focused regression
compares the scanner result to the direct lifter predicate, including an
unsupported `BRK` neighbor.

The post-M39 analyzer and independent scanner agree exactly. Coverage scanning
now has a deterministic bounded worker pool: each worker owns its Capstone
handle, local frequency maps are merged in input-range order, and the CLI
defaults to up to four workers (override with `--coverage-workers N`). The
library API remains serial by default unless `CoverageOptions::workers` is set.
A full main-module run with four workers used 229.73 user seconds in 63.05
wall seconds and produced the same report as the serial scan.


```text
module    decoded   liftable  unsupported  decode failures
main      11180285  11113251        67034              391
sdk        1539998   1529228        10770              282
subsdk0     855798    853517         2281              314
rtld          1519      1510            9                1
total     13577600  13497506        80094              988
```

## Tests and validation

- M39 focused suite: 65 assertions in 6 cases.
- M35/M36/M37/M38/M39 focused regression: 577 assertions in 44 cases.
- Complete direct Catch2 suite: 37,322 assertions in 400 cases.
- Focused build targets `switchrecomp-tests`, `aarch64-analyze`, and
  `aarch64-lift` completed successfully with Ninja.
- Serial and four-worker coverage reports are byte-identical on the focused
  fixture.
- `git diff --check` is clean.

Unsupported coverage moved from the exact M38 total of 163,438 to 80,094:

```text
module    before   after    delta
main      121083   67034   -54049
sdk        33902   10770   -23132
subsdk0     8401    2281    -6120
rtld           52       9       -43
total     163438   80094   -83344
```

Every delta equals the selected-family table above; no unrelated unsupported
category changed.

## Frontier and remaining bottlenecks

The real runtime frontier is unchanged and provider-blocked. After M39, the
largest static residuals are:

1. `unknown`: main 47,051, sdk 4,500, subsdk0 1,971, rtld 5; this remains a
   heterogeneous decode/normalization triage problem.
2. `fp_simd`: main 19,974 and sdk 5,967, with remaining high-count forms such
   as `LD1`/`TBL`/`FMLA` in main and `LD2`/`ST2`/`SQRDMULH` in sdk.
3. Unsupported system and pair-exclusive atomic instructions, including
   `SVC` and `LDXP`/`STXP` variants, remain honest boundaries.

No provider, target, register value, or runtime success was invented to reduce
these counts.

## Privacy

No game assets, private coverage JSON, local configuration, absolute personal
paths, credentials, build outputs, or agent logs are tracked. Survey artifacts
remain under the gitignored `local/` directory. No new dependency was
introduced.
