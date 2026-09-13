# Milestone 40: Coverage-Driven Semantic Convergence II

## Scope and baseline

M40 used the locally available `main` module and the default bounded parallel
coverage mode. The baseline was measured from commit
`bebe97369c93cdd55ed01864e22e615626e36868`, with the completed M39 worktree
present and no M40 semantic changes applied.

```text
                         decoded    liftable   unsupported   decode failures
main                     11180285   11113251        67034              391
```

The baseline unsupported rate was 0.599573%. The parallel scan (default
worker mode, `--max-instructions 12000000`) took 61.46 seconds wall time; the
serial `--coverage-workers 1` scan took 234.66 seconds. The JSON reports were
byte-identical.

The broad unsupported ranking was:

```text
unknown       47051
fp_simd       19974
msr               5
mrs               4
```

A Capstone-backed detailed survey decomposed the actionable high-frequency
members. The heterogeneous `unknown` bucket was not treated as one semantic
family.

## Selected families

The selected batch was the highest-payoff set that could share the existing
scalar integer/IR execution paths without claiming unrelated decode support:

| Family | Measured instances | Reason |
| --- | ---: | --- |
| UMADDL/UMSUBL/SMADDL/SMSUBL | 13,389 | complete scalar long multiply-add/subtract family |
| PRFM/PRFUM | 9,161 | architectural hint family with no guest-visible cache state |
| ADC/ADCS/SBC/SBCS/NGC/NGCS | 8,072 | one carry-in/no-borrow flag family and aliases |
| CRC32/CRC32C B/H/W/X | 7,615 | complete hardware CRC family with reusable lowering |
| scalar REV/REV16 | 2,710 | complete measured W/X byte-order family |
| **Total** | **40,947** | |

Broad remaining FP/SIMD forms and the heterogeneous decoder `unknown` bucket
were deferred rather than being accepted through a decoder-only shortcut.

## Semantics implemented

- Long multiply forms sign- or zero-extend W sources, multiply in the X
  domain, combine with the X accumulator, and preserve NZCV.
- ADC/SBC and NGC aliases consume the incoming C flag. The new IR
  `AddWithCarry`, `AddWithCarryCarry`, and `AddWithCarryOverflow` operations
  model result, no-borrow/carry-out, and signed overflow in both backends.
- CRC32 and CRC32C use the architectural reflected polynomials and
  little-endian byte order. The reusable `Crc32` IR operation lowers to the
  reference interpreter and LLVM bitwise implementation; NZCV and FP state are
  unchanged.
- PRFM and PRFUM are explicit non-faulting hint no-ops because the runtime has
  no guest cache-state model.
- Scalar REV reverses all bytes in W/X, while REV16 reverses bytes within each
  16-bit halfword. Upper register and unrelated architectural state remain
  governed by the existing scalar write policy.

Reserved operand widths and forms remain rejected by the normalized lifter and
coverage mirror. Coverage classification was updated alongside the direct
lifter predicate.

## Tests and validation

`tests/milestone40_semantic_convergence_tests.cpp` covers decoder identity,
W/X and signed/unsigned long multiply forms, all CRC widths and both
polynomials, hint state preservation, REV/REV16 byte boundaries, carry-in,
no-borrow, NZCV, and alias behavior. The focused M40 result was **85 assertions in 5
cases**.

The complete standard suite passed after the semantic checkpoint: **37,407
assertions in 405 cases**. Production sources compiled with the repository
warning policy; the optional LLVM target was disabled in this local build, but
its lowering was updated in parallel with the interpreter.

## Final coverage

```text
                         decoded    liftable   unsupported   decode failures
main                     11180285   11154198        26087              391
```

Final unsupported rate is 0.233330%. Unsupported coverage improved:

```text
67034 -> 26087  (40947 fewer, 61.0839% reduction)
```

The final parallel scan took 60.00 seconds wall time; serial took 219.97
seconds. Final parallel and serial JSON reports were byte-identical. The
parallel report's first unsupported addresses remain stable and begin at
`0x000000000000000c`.

## Remaining frontier and recommendation

The remaining broad ranking is `fp_simd` 19,974, `unknown` 6,104, `msr` 5,
and `mrs` 4. The largest detailed FP/SIMD members include LD1 (2,424), TBL
(2,296), FMLA (2,151), FCMLT (1,861), LD1R (1,713), and FMLS (164); vector
UMULL/SMULL and other conversion/reduction families also remain.

The exact recommended M41 target is the AdvSIMD structure-memory family,
starting with **LD1 and LD1R** and then matching LD2/LD4 structure forms. It is
the next coherent measured memory win (over 4,000 instances) and can reuse
checked guest vector-memory semantics. TBL and fused FMLA/FMLS should follow
only with their complete lane and FP architectural edge cases.
