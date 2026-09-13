# Milestone 38: Coverage-Driven Semantic Convergence

## Status

M38 is a coverage-driven sprint that advances the execution frontier by
implementing whole unsupported instruction families selected from a static
multi-module survey. It implements two coherent families: conditional compare
(`CCMP`/`CCMN` and scalar floating-point `FCCMP`/`FCCMPE`) and integer divide
(`UDIV`/`SDIV`). It also repairs the coverage scanner, which used a stale
liftability heuristic that disagreed with the real lifter.

Real TOTK execution on the available local input remains externally blocked at
the `__nnmusl_init_dso` provider boundary before semantic execution becomes
useful. M38 therefore does not repeat the ~80 s run-entry attempts, does not
claim a new real guest blocker, and uses an accurate static survey of the four
available modules for scope selection and validation.

## Git

```text
Base lineage: milestone-37-incremental-entry-index
Parent: 954ab3163d07c5851c25ffd9b324cedd55f038a6
Branch: milestone-38-coverage-driven-semantic-convergence
Merge status: not merged
```

## Measurement contract and survey

The external, non-committed module contract is frozen in a gitignored local
artifact. Module identities used by the survey:

```text
main    size 36,549,681  decoded 11,180,285
rtld    size  7,140      decoded      1,519
sdk     size  6,132,082  decoded  1,539,998
subsdk0 size  3,428,817  decoded    855,798
```

The M36 baseline `run-entry` report is unchanged: 22,164,104 bytes, SHA-256
`8c9ce368c66e8a1aaaa7cff53d6ba684f1cdfa9ce1e363b551cf377ef77374ec`, first stop
`unresolved_import` `__nnmusl_init_dso` at PC `0x0000007202aa421c`.

`translate-module --diagnostic` was rejected as a survey source: a full-module
lift of `sdk` exceeded 1800 s. The viable survey is a raw linear decode driven
by the exact lifter predicate `lifter::is_instruction_liftable(DecodedInstruction)`.

The project `aarch64-analyze --coverage` scanner classified liftability with a
hardcoded heuristic (`common_liftable`, `fp_simd_liftable`) that had drifted from
the lifter: it omitted UMULH/SMULH, atomics, barriers, and MRS/MSR, and used a
coarse FP/SIMD rule. M38 replaces it with a table that mirrors the lifter
exactly, including the MRS/MSR system-register and barrier-option checks and the
conditional MOVI/ST1 forms. `switchrecomp-analysis` must not link the lifter, so
the duplication is explicit and commented as a synchronization contract. After
the repair, `aarch64-analyze --coverage --json` reports the same unsupported
totals as the independent lifter-driven scanner for all four modules.

## Implemented families

**CCMP/CCMN.** `CCMP`/`CCMN` normalize to their own scalar IDs. The lifter
computes the architectural flags of the underlying subtraction or addition,
always evaluates the incoming condition, then selects each of NZCV between the
computed flag and the corresponding bit of the immediate fallback before writing
flags. The incoming condition is read from the pre-write flags, matching the ARM
ordering. No new IR opcode was required.

**FCCMP/FCCMPE.** These normalize as new `SimdOperation` values. The comparison
always executes so FPSR (including signaling-NaN invalid) is updated, and NZCV is
then conditionally selected between the comparison flags and the fallback
immediate. Scalar `FCMP`/`FCMPE` was refactored onto the same helper.

**UDIV/SDIV.** These lift to new IR primitives `DivideUnsigned`/`DivideSigned`.
The verifier requires a matching i32/i64 operand pair and result. The interpreter
implements division by zero yielding 0 and `INT_MIN / -1` yielding `INT_MIN`
without host undefined behavior. The LLVM backend guards `udiv`/`sdiv`
undefined cases with `select` on a safe divisor and on the signed overflow pair.

### FP/SIMD fused multiply-add (deferred)

The fused family is intentionally deferred. Scalar `FMADD`/`FMSUB`/`FNMADD`/
`FNMSUB` are nearly absent (main 0, sdk 2). The high-count vector forms (`FMLA`
2151, `FNMUL` 627, `FMLS` 164 in main) require true fused rounding; a decomposed
multiply-then-add would change rounding and is not architecturally correct. A
correct family needs a new vector operation, a fused runtime primitive, and both
interpreter and LLVM lowering; LLVM is disabled on this host, so the backend half
could not be compiled locally. Two families satisfy the sprint requirement, so
M38 stops here rather than commit an unverifiable backend path.

## Tests and validation

The focused M38 suite passes 142 assertions in 8 cases: decode/normalization and
`is_instruction_liftable` for both families, CCMP/CCMN taken and fallback paths
(W and immediate forms), FCCMP taken and fallback paths with FPSR update,
UDIV/SDIV W/X zero, unsigned-max, signed-negative and `INT_MIN/-1` cases,
verifier rejection of mismatched/narrow divide operands, and a coverage-scanner
regression asserting the new families are liftable.

The focused regression filter (`M35`,`M36`,`M37`,`M16`,`M38`) passes 4707
assertions in 45 cases; the complete direct Catch2 suite passes 37,257 assertions
in 394 cases. One historical M17 assertion that expected `CCMP` to be an
unsupported boundary was updated to a still-unsupported vector `FMLA` while
preserving its intent. LLVM is disabled on this host, so the conditional LLVM
parity test is not compiled locally and remains covered by the remote matrix;
sanitizers are not rerun locally for this checkpoint.

## Before and after unsupported counters

Accurate lifter-driven unsupported counts before and after the two families:

```text
module    before   after    delta
main      132,006  121,083  -10,923
sdk        36,333   33,902   -2,431
subsdk0     9,934    8,401   -1,533
rtld           62       52      -10
total     178,335  163,438  -14,897
```

Each delta equals the exact sum of the intended family members in that module
(for example, main: 5503 `ccmp` + 179 `ccmn` + 2179 `fccmp` + 2184 `udiv` +
878 `sdiv` = 10,923). This confirms no unrelated opcode classification changed.

## Frontier and remaining bottlenecks

The real frontier is unchanged and externally blocked. Ranked next unsupported
families from the accurate survey:

1. **Undecoded `unknown`** (main 65,165, sdk 26,712, subsdk0 7,516): largest
   bucket; heterogeneous, needs mnemonic-level triage before it is a family.
2. **`fp_simd:none`** (main 38,689, sdk 6,073): vector data movement/reduction;
   `faddp` 12,445, `bif` 2,928, `ld1` 2,424, `tbl` 2,296, `fmla` 2,151.
3. **`fp_simd:movi` remaining forms** (main 17,179, sdk 756, subsdk0 575).
4. **Pair-exclusive atomics** (sdk `stlxp` 63, `ldaxp` 50, `stxp` 38, `ldxp`
   26): coherent but low count and needs new IR.
5. **`svc`**: trap semantics, not a semantic instruction family.

## Privacy

No game assets, private coverage JSON, local configuration, absolute personal
paths, credentials, build outputs, or agent logs are tracked; all survey
artifacts remain under a gitignored local directory. No new dependency was
introduced.
