# Milestone 43: high-impact AdvSIMD coverage collapse

## Scope and baseline

M43 continued the dirty M42 structure-store work without resetting unrelated
changes. The prepared `main` coverage baseline, using a 12,000,000-instruction
bound, was:

```text
                         decoded    liftable   unsupported   decode failures
M42 baseline             11180285   11158677        21608              391
```

The largest actionable portion of the unsupported frontier was normalized
AdvSIMD arithmetic and table operations. Unsupported FP/SIMD instances were
15,495 before this milestone.

## Semantics implemented

- Scalar UMULL/SMULL aliases are separated from AdvSIMD names and lifted as
  unsigned or signed W-by-W products producing an X result.
- AdvSIMD UMULL/SMULL, upper-half variants, and UMLAL/UMLSL signed and unsigned
  forms extract the correct source half, widen explicitly, and perform
  modulo-width multiply/accumulate/subtract operations.
- FMLA/FMLS vector and by-element forms use a dedicated fused FP IR operation.
  The reference runtime handles IEEE bit patterns, NaNs, signed zero,
  rounding, flush-to-zero, and sticky FPSR state without exposing host FP
  state. LLVM calls the same runtime helper.
- FCMLT/FCMLE zero-immediate forms are normalized as reversed FP vector
  comparisons, preserving the existing comparison and status behavior.
- TBL/TBX accept one through four consecutive 16-byte table registers. TBL
  writes zero for out-of-range byte indexes; TBX preserves the destination for
  those indexes. B8 and B16 result/index forms are supported.
- Decoder predicates, detailed coverage, IR verification, interpreter
  execution, and optional LLVM lowering share the same operand and arrangement
  boundaries. Reciprocal/rounding estimates, unknown/SVE operations, and
  malformed forms remain unsupported.

## Tests and validation

`tests/milestone43_advsimd_coverage_tests.cpp` covers decoder and coverage
agreement, scalar and upper-half widening multiplication, fused FP lane
results, zero comparisons, one/four-table TBL/TBX bounds, and TBX preservation.
The focused result was **91 assertions in 5 cases**.

The complete standard suite passed with **37,737 assertions in 416 cases**.
The optional LLVM target was disabled in this local build; its lowering and
runtime symbols were updated alongside the interpreter.

Serial and bounded four-worker coverage reports were byte-identical.

## Final coverage

```text
                         decoded    liftable   unsupported   decode failures
M43 prepared main        11180285   11168628        11657              391
```

M43 removes 9,951 unsupported instructions:

```text
21608 -> 11657  (46.05% reduction; 0.104257% of decoded instructions remain unsupported)
```

The resulting count meets both the required 14,000 ceiling and the 12,000
stretch target. Remaining unsupported categories are:

```text
fp_simd        8399
unknown        3249
msr               5
mrs               4
```
