# Milestone 19 — Move-wide immediate semantics and the next execution frontier

## Status

**Complete.** M19 removes the former real execution boundary at
`sdk:0x72047182ac` by lifting and executing the reusable A64 move-wide family.
The same controlled guest path advances beyond `MOVZ W0, #2` and stops at a
new, typed unknown-function boundary before the known real `UMULH` site. No
claim is made that TOTK boots or that the full executable set is executable.

## Git

* branch: `milestone-19-move-wide-umulh-frontier`
* exact base SHA: `f571c21e7d306bfc80f3cba294bd4d31cfb2607e`
* exact implementation head SHA: `34077c6`
* final branch head and PR: reported in the completion handoff

The branch was created from the exact M18 head before source changes. The
pre-change working tree contained only the pre-existing `.gitignore` edit and
untracked `.DS_Store` files; those local changes were preserved and are not
part of this milestone.

## Baseline

* M18 expected baseline: `227/227`
* observed pre-change baseline: `227/227`
* final local suite: `233/233`

The same private, locally configured four-module executable set was used for
the real run: `rtld`, `main`, `subsdk0`, and `sdk`. No executable bytes,
private reports, keys, or local absolute paths are committed.

## Previous frontier

M18 ended at:

```text
module: sdk
PC: 0x72047182ac
instruction: mov w0, #2
architectural operation: MOVZ W0, #2
opcode: 0x52800040
```

The M18 lifter had an older move-wide helper, but it derived the immediate and
shift from the raw opcode and its diagnostic-stop policy converted every later
CFG block into a synthetic trap after the first unsupported block. M19 makes
the normalized move-wide fields authoritative and keeps a typed trap local to
the unsupported block, allowing independently lifted CFG blocks to execute
when the real path selects them.

## Move-wide implementation

The decoder normalizes `MOVZ`, `MOVN`, and `MOVK` to project-owned instruction
IDs with a general W/X destination, a typed architectural `imm16`, and an
explicit `LSL` shift. For `MOVN`, normalization corrects Capstone's signed
alias presentation so the lifter receives the encoded `imm16`, not a formatted
final constant. The exact `0x52800040` normalization is `MOVZ`, destination
`W0`, immediate `2`, shift `0`.

The existing generic Semantic IR remains the representation:

* `MOVZ` lowers to a width-typed `Constant` followed by `WriteRegister`;
* `MOVN` lowers to the width-limited inverse of the shifted immediate;
* `MOVK` reads the destination, clears exactly one 16-bit lane, inserts the
  shifted immediate, and writes the result.

No Capstone type enters the IR and no opcode-specific `#2` path was added.
The lifter rejects non-general destinations, stack-pointer destinations,
invalid immediates, non-LSL shifts, and shifts outside the legal width-specific
sets. The IR verifier rejects invalid register widths, scalar constants with a
nonzero high half, and all resulting type/width mismatches through the generic
constant and register-write invariants.

Architectural results are width-limited before the generic write. Therefore a
W write uses the existing CPU-state rule and zero-extends into X:

```text
MOVZ W0, #2  =>  X0 = 0x0000000000000002
```

The interpreter and LLVM lowering both use the existing typed constant and
register-write operations. LLVM uses explicit i32/i64 values and zero-extends
W writes to the 64-bit architectural register storage.

The architectural semantics were checked against Arm's public A64
documentation for [MOVZ](https://developer.arm.com/documentation/ddi0602/2026-06/Base-Instructions/MOVZ--Move-wide-with-zero-),
[MOVN](https://developer.arm.com/documentation/ddi0602/2026-06/Base-Instructions/MOVN--Move-wide-with-NOT-),
and [MOVK](https://developer.arm.com/documentation/ddi0602/2026-06/Base-Instructions/MOVK--Move-wide-with-keep-).

## Synthetic coverage

The M19 test file covers independent known encodings for:

* `MOVZ W0` immediates `0`, `1`, `2`, and `0xffff`, including W shift `16`;
* `MOVZ X1` shifts `0`, `16`, `32`, and `48`;
* overwrite of stale destination state and W-register upper-half clearing;
* `MOVN` W/X forms and `MOVK` X lanes plus W lane/zero-extension behavior;
* interpreter execution, malformed normalized forms, malformed register/type IR,
  and the M18 diagnostic CFG-block isolation failure mode;
* optional interpreter/LLVM bit-for-bit equivalence for representative forms.

## Real execution

The fresh run used the same metadata-selected entry and setup as M18. The
former boundary was crossed continuously through guest execution:

| item | evidence |
| --- | --- |
| former PC reached | `sdk:0x72047182ac` |
| normalized ID | `movz` |
| mnemonic | `mov w0, #2` |
| pre-state | `X0 = 0x0000000000000000` |
| post-state | `X0 = 0x0000000000000002` |
| expected result | `0x0000000000000002` |
| next guest PC | `sdk:0x72047182b0` |
| instructions after former blocker | `20` |
| total guest instructions | `114` |
| maximum call depth | `2` |
| runtime fallback | `0` invocations |
| provider guest code entered | `true` |

The M18 discovered guest target remained canonically owned and was entered as
guest code; no duplicate promotion or host fallback was used.

## UMULH frontier

The known real instruction at `sdk:0x7204717c30` was **not reached**. The
continuous path stopped first at a different indirect call, so `x10`, the old
`x9`, the resulting `x9`, the independent expected high half, and a post-UMULH
PC are not established. No state was synthesized and the instruction was not
manually invoked.

## Final execution boundary

The exact next boundary is:

```text
typed stop:       unknown_guest_function
module:           main
source PC:        0x7200000148
instruction:      blr x8
opcode:           0xd63f0100
target register:  x8
target:           0x7200001520
provenance:       guest_load:0x720456ab68
```

The target is aligned, executable, and owned by the `main` process module, but
it is not an exact trusted finalized function entry. The bounded candidate
analysis also reports an undecoded instruction at `main:0x72000001a4`, so the
target is not promoted or entered. This is an existing typed ownership/
discovery boundary, not a move-wide or bootstrap failure.

## Bootstrap and runtime policy

* bootstrap required: not established;
* bootstrap model added: no;
* runtime fallback invoked: no;
* runtime handlers were not used to represent ordinary guest instructions;
* `BR`, `BLR`, `RET`, call depth, canonical ownership, and bounded target
  discovery behavior remain unchanged.

## Determinism

The report schema remains `6`; M19 adds deterministic fields for instruction
observation targets, bounded guest-instruction counts, pre/post relevant
register values, and UMULH oracle comparison without changing the existing
schema version. Two final reports generated from identical local inputs were
byte-identical with SHA-256
`ef1cb928c58991399d72821d65124a713d4adca13079fb5b40be2008ef9321f5` for
each run.

## Validation

* local AppleClang standard build/tests: `233/233`;
* local GCC build/tests: `233/233`;
* local ASan/UBSan build/tests: `233/233`;
* local TSan build/tests: `233/233`;
* LLVM backend equivalence tests are compiled when the configured LLVM backend
  is enabled; this machine has LLVM 23, while the project requires LLVM 18.1.3,
  so the local LLVM-18 job was not available;
* the repository LLVM-18 and MSVC CI jobs remain the authoritative validation
  for those toolchains.

## Privacy

No proprietary executable, NSO/XCI/NSP content, keys, private report, or local
absolute path was added to the repository. Real evidence was read from the
existing local-only configuration and summarized using guest addresses and
typed outcomes only.
