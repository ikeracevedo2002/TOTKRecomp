# Milestone 16 — Real guest-provider execution frontier

## Status and scope

**Status:** Partial

**Exact base SHA:** `aadde85a2018da3e9ddb0dbfa8e1c18462c07710`

**Branch:** `milestone-16-provider-execution-frontier`

M16 implements the real architectural boundary identified by the M15 run:
scalar A64 `UMULH`. It carries that operation through the project-owned
decoder, Semantic IR, verifier, interpreter, and LLVM backend, and adds a
synthetic process test that crosses a guest-provider relocation into a provider
containing `UMULH`.

The M15 real four-module input set was not present in this checkout. The local
inputs contain only a prepared `main` image, so the exact M15 controlled run
could not be rerun and no post-`UMULH` guest boundary can honestly be reported.
No loader/bootstrap state was invented to compensate for that missing evidence.

## Established M15 evidence

M15 established the following facts and they remain unchanged:

* The locally asserted real executable set was `rtld`, `main`, `subsdk0`, and
  `sdk`. Its completeness was only `declared_complete`, based on an explicit
  local assertion; exact-build manifest verification was not established and
  coherence was only partially verified.
* The eligible guest definition of `__nnmusl_init_dso` was in `sdk`, dynamic
  symbol index `8767`, `st_value` `0x8d880`, with selected `sdk` base
  `0x720468a000`, producing guest address `0x7204717880`.
* `main` and `subsdk0` imports resolved through guest `R_AARCH64_JUMP_SLOT`
  relocations to that guest address, and relocation-slot readbacks were
  verified.
* No host function pointer was written and runtime fallback was not invoked.
* The controlled M15 entry was `0x7200000030`, a metadata-selected `main`
  `DT_INIT` candidate. It was not a verified Nintendo process entrypoint.
* M15 reached `main`, `0x7200000030`, `0x7202aa4210`, the `main` import
  trampoline, and the resolved target address, but lifting stopped at
  `umulh x9, x10, x9` at `0x7204717c30`. The provider function therefore was
  not meaningfully executed in M15.

The guest provider remains authoritative for `__nnmusl_init_dso`; no runtime
replacement was added.

## UMULH implementation

Architecturally, `UMULH Xd, Xn, Xm` reads two unsigned 64-bit X-register
operands, forms their mathematical 128-bit product, writes product bits
`[127:64]` to `Xd`, leaves NZCV unchanged, and observes the existing project
XZR behavior. There is no W-register form in this implementation.

The implementation follows the public A64 definition of `UMULH` and the Arm
A-profile description of the upper half of a 128-bit multiply:

* [Arm A-profile Instruction Set Overview](https://developer.arm.com/-/media/Files/pdf/graphics-and-multimedia/ARMv8_InstructionSetOverview.pdf)
* [public A64 UMULH pseudocode reference](https://www.scs.stanford.edu/~zyedidia/arm64/umulh.html)

The decoder already used Capstone as its frontend, so M16 adds the normalized
project-owned `InstructionId::Umulh` and keeps Capstone identifiers out of the
IR. `UMULH` is treated as an ordinary fall-through scalar instruction by CFG
construction. The lifter emits `ir::Opcode::MulHighUnsigned` and rejects
non-64-bit or malformed operand forms.

`MulHighUnsigned` is a generic Semantic IR operation: exactly two `i64`
operands and one `i64` result. The verifier rejects other widths or mismatched
operand/result types deterministically. The opcode is included in IR naming,
printing, and both backend dispatches.

The interpreter uses a project-owned portable 32-bit-limb decomposition. It
does not depend on `unsigned __int128`, signed overflow, host endianness, or a
host pointer. The LLVM backend zero-extends both operands to `i128`, multiplies,
logically shifts right by 64, and truncates to `i64`.

The support matrix in [AARCH64_SUPPORT.md](AARCH64_SUPPORT.md) marks only
scalar `UMULH` as supported. `SMULH`, widening multiply/add/subtract families,
vector operations, and SVE operations remain separate and unsupported.

## Tests and synthetic execution

M16 adds deterministic tests for:

* an independent four-limb test oracle and a fixed-seed randomized set;
* zero, maximum, boundary-crossing powers of two, high-bit-only,
  alternating-bit, and low-product-trivial/high-half-nonzero vectors;
* decoder normalization, decode-to-lift-to-interpreter execution, PC advance,
  NZCV preservation, destination/source aliases, equal sources, and XZR
  source/destination behavior;
* verifier rejection of invalid width combinations;
* optional LLVM/interpreter state and status parity;
* a synthetic `main` consumer and `sdk` provider with a `JUMP_SLOT`, checked
  provider-base arithmetic, indirect guest transfer, provider `UMULH`, return
  continuation, module transition accounting, and a runtime descriptor that is
  deliberately not called;
* an invalid provider definition that remains a typed ineligible-provider
  result instead of being silently replaced by HLE.

The existing bounded event trace now records the guest PC and owning module on
each `FunctionEnter` event, so a future real run can correlate provider entry,
source transfer, and call depth without including code bytes or host pointers.

The committed fixtures contain only synthetic NSO metadata and public A64
instruction encodings. They contain no Nintendo executable bytes.

## Real executable-set experiment

**Executable set available:** no. The exact M15 four-module set was not
available; only a local prepared `main` image was present.

**Entry candidate:** `0x7200000030` (preserved M15 controlled experiment).

**Entry provenance:** `main` `DT_INIT` metadata candidate; not a verified
process entrypoint.

**Provider symbol:** `__nnmusl_init_dso`

**Provider module:** `sdk`

**Provider guest address:** `0x7204717880` (M15 evidence)

**Provider guest code entered:** no new M16 observation; the M15 run was
stopped before provider code could execute. This is not claimed as a successful
post-`UMULH` result.

**First provider PC actually executed:** not established. M15's first provider
boundary was `0x7204717c30`, the unsupported `umulh x9, x10, x9`, before the
provider body was entered.

**Modules/functions entered:** M16 real-input trace not produced. The inherited
M15 trace entered `main`, `0x7200000030`, `0x7202aa4210`, the `main` import
trampoline, and the resolved guest target boundary.

**Runtime fallback invoked:** no, according to the M15 evidence; M16 did not
have the exact real set available to produce a new run.

**Final stop:** the inherited M15 stop was typed as
`unsupported_instruction`, module `sdk`, PC `0x7204717c30`, instruction
`umulh x9, x10, x9`. The post-M16 real stop is unknown until the exact set is
rerun.

The existing process report fields remain compatible with schema 4, including
typed stop reason, stop module/PC, provider guest-entry state, function/module
trace, and runtime fallback accounting. No report-schema bump was needed.

## Bootstrap investigation

**Required:** not established.

**Exact evidence:** the M16 checkout could not enter the real provider because
the exact four-module input set was unavailable. Therefore no provider-side
memory access, register contract, TLS requirement, loader-created structure, or
causal bootstrap wall was observed.

**Bootstrap model added:** no.

**Modeled state:** none. The existing controlled synthetic stack/TLS state was
not relabeled as Nintendo startup state.

**Startup candidate analysis:** no new `rtld` candidate was selected. The
metadata-selected `main` `DT_INIT` candidate remains explicit and unverified.

Public evidence used for the investigation is kept distinct from observations:

* [AAPCS64](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst)
  establishes the public AArch64 procedure-call contract.
* [AAELF64](https://github.com/ARM-software/abi-aa/blob/main/aaelf64/aaelf64.rst)
  establishes public ELF/AArch64 ABI rules.
* [Atmosphere loader documentation](https://github.com/Atmosphere-NX/Atmosphere/blob/master/docs/components/modules/loader.md)
  documents an open-source loader's module search/load behavior; it is not
  proof of Nintendo's private implementation.
* [Switchbrew Rtld](https://switchbrew.org/wiki/Rtld),
  [NSO0](https://switchbrew.org/wiki/NSO0), and
  [MOD](https://switchbrew.org/wiki/MOD) provide public format and loader
  observations; they do not turn the M15 `DT_INIT` address into a verified
  process entrypoint.

No host pointer, unexplained zero-filled structure, guessed return value, or
proprietary loader behavior was introduced.

## Determinism

**Report schema:** 4, unchanged.

**Real report SHA-256 #1:** not produced; exact four-module input unavailable.

**Real report SHA-256 #2:** not produced; exact four-module input unavailable.

**Identical:** not applicable to the unavailable real report.

The synthetic arithmetic and provider tests use fixed inputs and a fixed random
seed. They emit no timestamps, usernames, absolute paths, host addresses, or
proprietary bytes.

## Validation

The exact M15 checkout baseline was `202/202` CTest tests before M16 changes.
The final local AppleClang non-LLVM build/test result is `209/209` CTest tests
and the M16 filter reports `4,186` assertions in seven test cases. The
available ASan/UBSan and TSan configurations also pass `209/209`.

| Configuration | Result |
| --- | --- |
| Local default AppleClang | 209/209 |
| ASan/UBSan | 209/209 |
| TSan | 209/209 |
| GCC | not available in this checkout; `/usr/bin/gcc` is AppleClang |
| LLVM 18 | not available; LLVM-enabled configuration was disabled by CMake |
| MSVC | not available locally |

LLVM lowering remains implemented but was not executable-tested here because
the installed local LLVM is 23.1 and the project requires LLVM 18. CI must
provide the authoritative LLVM 18 and MSVC results.

## Dependencies and privacy boundary

**New dependencies:** 0. M16 uses the existing Capstone, Catch2, and optional
LLVM integrations only; no 128-bit integer library or emulator framework was
added.

The repository remains free of NSOs, XCI/NSP/NCA content, keys, proprietary
binary fragments, private reports, and private absolute paths. Real executable
sets remain local, user-supplied inputs outside the committed repository.

## Exact next blocker

The concrete blocker for the next real frontier result is the missing exact
M15 four-module input set (`rtld`, `main`, `subsdk0`, and `sdk`) needed to rerun
the controlled experiment after the `UMULH` fix. Until that set is supplied,
the first post-`0x7204717c30` guest instruction, provider return behavior, and
any evidence for loader-created bootstrap state remain unknown. That missing
input—not a guessed bootstrap model—is the precise next boundary for M17.
