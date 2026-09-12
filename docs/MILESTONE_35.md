# Milestone 35: FP/SIMD Semantic Convergence

## Status and lineage

M35 is implemented through the measured FP/SIMD frontier and stops at a
different CPU subsystem.

```text
Base:    0fa827684961725c0cc300ffb7d1f58757ea91cf
Parent:  infra-agent-orchestration-ci
Branch:  milestone-35-fp-simd-semantic-convergence
M34:     0ec9abbb6229936105644d73fa02fa8f4ffb4416
Schema:  20 (unchanged)
```

The branch is directly stacked on I0. No merge from `main`, rebase, squash,
or modification of I0 history is part of this milestone.

## Baseline

Before production changes, the exact I0 source reproduced the public standard
suite:

```text
356/356 tests
36,730 assertions
```

The historical M8 FP/SIMD filter also passed:

```text
8/8 tests
```

The restored private execution used the existing four-module recovery
configuration with no forced PC, target, seed, event ceiling, or budget
override. Its semantic baseline matched the M34 frontier exactly:

```text
schema: 20
stop: unsupported_instruction
module/function/PC: main / 0x00000072001485f0 / 0x0000007200148610
opcode/instruction: 0x0f03f5e1 / fmov v1.2s, #0.96875000
guest instructions / blocks / IR: 18,897 / 3,837 / 112,428
slices / yields / resumes / mid-block: 2,084 / 1 / 1 / 1
direct / indirect calls: 467 / 542
returns / transfers / maximum depth: 1,008 / 65 / 3
runtime fallbacks: 0
events total / retained / omitted: 4,170 / 2,122 / 2,048
```

The restored configuration's deterministic report was 42,431,792 bytes with
SHA-256 `f37d1d46d0f34025434b35497f2b3688df8382a880b6ca7762aaa6910caf3d53`,
versus the historical M34 evidence of 42,431,771 bytes and
`041906e67b377e8b8346f843a7a803b47f2b2ea4aeac775ea18bd4609b17f344`. The
21-byte difference was investigated before semantic edits: the recovered
configuration carries explicit restored-module provenance and a complete
provider assertion that the historical local file did not expose. The stop,
instruction counts, event accounting, refinement totals, and architectural
frontier are identical; no production behavior or resource limit was changed
to conceal the configuration difference.

## Starting frontier and forensic cause

The measured M34 blocker was:

```text
main:0x0000007200148610
opcode: 0x0f03f5e1
instruction: fmov v1.2s, #0.96875000
```

Capstone already decoded the instruction as the project-owned `FpSimd`
instruction with `SimdOperation::Fmov`, a vector destination `v1`, a typed
`.2S` arrangement, and a floating-immediate operand. It provided no source
register and ordinary fallthrough. The gap was in the semantic lifter: vector
FMOV handled register moves, but a vector floating immediate was not routed to
the existing typed broadcast/write path, so the instruction remained an
unsupported boundary.

The architectural reference is
[FMOV (vector, immediate)](https://arm.jonpalmisc.com/latest_aarch64/fmov_advsimd).
Its immediate is expanded and broadcast to the destination elements; `.2S`
writes a 64-bit destination and clears the shared V register's upper 64 bits.

## Architecture implemented

### FMOV (vector, immediate)

- Decoder: reused the normalized `FloatingImmediate`, vector register, and
  project-owned arrangement fields; no disassembly-string parsing was added.
- IR: existing typed constant/bitcast, `VectorBroadcast`, and
  `WriteVectorRegister`.
- Verifier: existing vector-broadcast arrangement and lane-width checks.
- Interpreter/LLVM: existing `VectorBroadcast` lowering and explicit
  `Vector128{lo, hi}` representation.
- Reference semantics: exact f32/f64 bit patterns from the typed immediate.
- Destination state: `.2S` broadcasts two f32 lanes and clears upper 64 bits;
  `.4S` fills four f32 lanes; `.2D` fills two f64 lanes.
- FPCR/FPSR/NZCV: unchanged.

### ST1 (single structure, one lane)

- Decoder: added project-owned `SimdOperation::St1`. Capstone exposes the lane
  index but leaves its arrangement invalid, so normalization derives B/H/S/D
  arrangement from the architectural `Q/S/size/opcode` fields. No text parsing
  or Capstone type escapes the decoder.
- IR: existing `ReadVectorRegister`, `VectorExtractLane`, checked
  `GuestStore`, and existing address/writeback helpers.
- Verifier: existing typed lane extraction and checked memory-store rules.
- Interpreter/LLVM: existing checked little-endian guest store semantics.
- Reference semantics: stores only the selected byte/halfword/word/doubleword
  lane; the source V register and FP/status flags are unchanged. Multi-register
  ST1 forms are not falsely claimed as implemented.

The architectural reference is
[ST1 (single structure)](https://arm.jonpalmisc.com/latest_aarch64/st1_advsimd_sngl).

### MOVI (vector immediate)

- Decoder: added project-owned `SimdOperation::Movi` and generic expansion of
  `Q/op/cmode/imm8` into the exact architectural immediate. This covers the
  measured `.2D` form and the valid byte/halfword/word modified-immediate
  classes without using the formatted disassembly.
- IR: existing integer constants, `VectorBroadcast`, and full V-register
  writes.
- Verifier: existing integer lane-width and vector-broadcast validation.
- Interpreter/LLVM: existing broadcast implementation with explicit lane order.
- Reference semantics: the expanded constant is replicated to every valid
  destination element; the complete 64- or 128-bit destination is overwritten
  as required by its arrangement. FPCR/FPSR/NZCV are unchanged.

The architectural reference is
[MOVI](https://arm.jonpalmisc.com/latest_aarch64/movi_advsimd).

No new IR opcode, runtime dependency, raw-opcode escape hatch, target PC
conditional, or game-specific semantic branch was introduced.

## Synthetic validation

`tests/milestone35_fp_simd_convergence_tests.cpp` is registered in the normal
test target. The focused M35 filter currently contains 17 tests and covers:

- measured FMOV typed decode and ordinary fallthrough;
- measured FMOV liftability and IR verification;
- exact `.2S`, `.4S`, and `.2D` broadcast bits;
- negative and varied immediate values;
- `.2S` upper-half clearing and FPCR/FPSR/NZCV preservation;
- malformed `VectorBroadcast` verifier rejection;
- measured MOVI `.2D` normalization, liftability, exact zero state, and a
  nonzero repeated-byte immediate;
- exact MOVI MSL `#8`/`#16` 32-bit modified-immediate expansion;
- measured ST1 typed lane normalization and liftability;
- exact selected S-lane store through checked memory with V/GPR/status-state
  preservation;
- generic ST1 B/H/S/D arrangement normalization;
- rejection of the valid multi-register ST1 form at the single-lane seam;
- conditional interpreter/LLVM parity for the MOVI+ST1 program when LLVM is
  enabled.

## Convergence log

All real runs were ordinary private executions with the same entry/configuration
and separate ignored reports.

| Iteration | Frontier | Category | Implementation | First progress / next stop |
| --- | --- | --- | --- | --- |
| 0 | `main:0x0000007200148610`, `0x0f03f5e1`, `fmov v1.2s, #0.96875000` | FP/SIMD immediate | Generic FMOV vector immediate `.2S/.4S/.2D` | Advanced to `main:0x000000720014899c`, `0x4d008100`, `st1 {v0.s}[2], [x8]` |
| 1 | `main:0x000000720014899c`, `0x4d008100`, `st1 {v0.s}[2], [x8]` | FP/SIMD lane memory | Generic ST1 single-lane B/H/S/D store | Advanced to `main:0x0000007200148fc4`, `0x6f00e400`, `movi v0.2d, #0000000000000000` |
| 2 | `main:0x0000007200148fc4`, `0x6f00e400`, `movi v0.2d, #0` | FP/SIMD immediate | Generic MOVI expanded-immediate forms | Advanced to `main:0x00000072003875fc`, `0x1ad32108`, `lsl w8, w8, w19` |

The intermediate reports were sanitized to retain only architectural identity,
addresses, counters, and stop evidence. No proprietary instruction dump,
private path, binary, key, or report was added to Git.

## Final real execution

Two identical ordinary runs reached the same non-FP/SIMD frontier. Their
reports were byte-identical:

```text
schema: 20
report size: 52,808,340 bytes
SHA-256: 98f595160a00e98c83a5ab9558b115d8c287d37dc64c9217310b7bb687a76014
stop: unsupported_instruction
module/function/PC: main / 0x00000072003875a0 / 0x00000072003875fc
opcode/instruction: 0x1ad32108 / lsl w8, w8, w19
guest instructions / blocks / IR: 31,366 / 5,975 / 179,437
slices / yields / resumes / mid-block: 3,772 / 1 / 1 / 1
direct / indirect calls: 1,120 / 722
returns / transfers / maximum depth: 1,840 / 88 / 3
runtime fallbacks: 0
events total / retained / omitted: 7,546 / 4,096 / 3,450
```

The final refinement totals were 728 execution attempts, 727 productive
rounds, 727 promotions, 727 candidate records, zero pending candidates, and
664,043 observations. Aggregate analysis consumed 1,530 functions analyzed,
6 reanalyzed, 276,313 reused, 42,906 instructions, 8,636 blocks, 12,690
edges, 171,624 analyzed bytes, 727 boundary-finalization passes, 3 invalidated
records, and 726 transactions. Runtime fallbacks and failed refinements were
zero.

## Progress delta from M34

Relative to the exact M34 semantic counters:

```text
guest instructions: +12,469   blocks: +2,138   IR operations: +67,009
execution slices:    +1,688   direct calls: +653 indirect calls: +180
returns:             +832      transfers: +23
logical events:      +3,376   FP/SIMD frontiers crossed: 3
```

The final natural stop is classified as `non_fp_simd_instruction_semantics`.
It is scalar integer `LSL W8, W8, W19`, not a remaining FMOV/MOVI/ST1 or
adjacent FP/SIMD capability. M35 therefore stops at the measured subsystem
boundary and does not begin integer-shift work.

## Validation, privacy, and dependencies

The standard local build ladder used a persistent normal build directory; the
host-only AppleClang compatibility workaround for an existing structured
binding was applied temporarily during compilation and is not part of this
branch.

Completed final validation:

```text
M35 focused tests: 17/17
M8/relevant FP/SIMD regressions: 8/8
standard suite: 373/373 tests, 36,878 assertions
ASan/UBSan: 373/373
TSan practical set (M29-M35): 76/76
private ordinary determinism A/B: byte-identical reports, size 52,808,340,
  SHA-256 98f595160a00e98c83a5ab9558b115d8c287d37dc64c9217310b7bb687a76014
```

ASan/UBSan and TSan reported no findings. The local LLVM 18 backend is not
available on this host, so LLVM parity is covered by the conditional public
tests and remains part of the remote CI matrix.

Pi accepted the exact semantic checkpoint before this final documentation-only
update. Remote CI is the remaining final handoff step; transient workflow IDs
are intentionally kept out of tracked documentation.

Expected new dependency count is `0`. No private Nintendo/recovery artifacts,
keys, firmware, binaries, local configuration, private reports, build output,
credentials, or absolute personal paths are tracked. The public build and
tests remain independent of the private recovery workspace.

## Pi verification

```text
Reviewed SHAs:
3218834fc8e01d0e7e6ffeb8e7372e1903a4524b
d90efc99f96c343086a0a6790b5ff6051ce51197
Prompt SHA-256: c8a7d4cf1b9d305aea8a08d7d67c5528c76f55334e32c0427bb7b9a4ed504a3f
Rounds: 2
Final verdict: PASS WITH NON-BLOCKING FINDINGS
Blocking findings: none unresolved
Fixed blocker: MOVI cmode 0xC/0xD MSL classification and exact tests
Non-blocking finding: stale 16/16 documentation count, corrected in this commit
```

The external prompt, reports, and logs remain outside the repository.

## Documentation

Tracked documentation changed:

```text
docs/MILESTONE_35.md
docs/AARCH64_SUPPORT.md
```
