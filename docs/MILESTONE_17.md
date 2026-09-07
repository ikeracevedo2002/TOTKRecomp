# Milestone 17 — Post-UMULH provider frontier

## Status

**Complete.** The exact four-module controlled experiment was rerun on the
Milestone 16 base. It reached the real guest provider, but stopped before the
real `UMULH` at `sdk:0x7204717c30`; the first observed execution boundary is a
typed unknown guest function in `sdk`. This is an execution-frontier result,
not a boot claim.

## A. Previously established evidence

Milestones 15 and 16 established the following facts, which were preserved:

* The controlled entry remains `0x7200000030`, selected from `main` `DT_INIT`
  metadata. It is not a verified Nintendo process entrypoint.
* `__nnmusl_init_dso` has an eligible guest provider in `sdk`, dynamic symbol
  index `8767`, `GLOBAL FUNC DEFAULT`, section `1`, `st_value` `0x8d880`.
* The checked provider address is
  `0x720468a000 + 0x8d880 = 0x7204717880`.
* The `main` JUMP_SLOT is relocation index `503796`, slot
  `0x720453ef80`; the `subsdk0` JUMP_SLOT is relocation index `15473`, slot
  `0x72059f7ed0`.
* M16 implemented scalar AArch64 `UMULH` through decoder normalization,
  Semantic IR, verification, interpretation, LLVM lowering, and tests.

## B. New M17 observations

### Git

* branch: `milestone-17-post-umulh-provider-frontier`
* exact base SHA: `297768205e44c617b4eaf3f714ef30d2e97d733c`
* exact final head SHA: recorded in the final Git handoff after the final
  documentation commit
* commits: M17 implementation, diagnostic execution, tests, and evidence
  documentation
* PR: recorded after branch push

### Baseline and executable set

* expected M16 baseline: `209/209` CTest tests
* observed pre-change baseline: `209/209`
* local prepared set available: yes
* all four exact module identities matched: yes
* completeness: `declared_complete`
* completeness basis: `explicit_local_assertion`
* coherence: `partially_verified`
* load-order basis: `public_exefs_load_order`
* deterministic load order: `rtld -> main -> subsdk0 -> sdk`
* no proprietary inputs committed: yes

The expected identities and checked bases were:

| module | size | SHA-256 | analysis base |
| --- | ---: | --- | --- |
| `rtld` | 7,140 | `28317605926caa40a5cbee3e3cb290c65783b4d9f8c3697b43ca41ab2866a44d` | `0x7204676000` |
| `main` | 36,549,681 | `faf81f8a609ff61a6322d04948132b5929af9f6e2398e0e0ebbe819b8ba3eaab` | `0x7200000000` |
| `subsdk0` | 3,428,817 | `9d04c98997977e99c12a48a2dbfd3064b8da96b5152e0119064957a43f6c3f99` | `0x7205473000` |
| `sdk` | 6,132,082 | `91573d4444d0dcd07080db78fece2b098b76080d77cdb2e1d301a2a41d7c4eea` | `0x720468a000` |

NSO parsing, MOD0, dynamic metadata, executable mappings, Build IDs, sizes,
and SHA-256 identities all matched. The process report intentionally retains
the M15 provenance classification; matching the prior controlled identities
does not upgrade it to a manifest attestation.

### Provider execution

* symbol: `__nnmusl_init_dso`
* consumer occurrence in `main`: dynamic symbol index `5`, undefined,
  `GLOBAL FUNC`
* provider occurrence in `sdk`: dynamic symbol index `8767`, defined,
  `GLOBAL FUNC DEFAULT`, section `1`, `st_value=0x8d880`
* provider module: `sdk`
* provider base: `0x720468a000`
* resolved guest address: `0x7204717880`
* `main` relocation readback: slot `0x720453ef80` read back
  `0x7204717880`
* `subsdk0` relocation readback: slot `0x72059f7ed0` read back
  `0x7204717880`
* runtime fallback invoked: no; count `0`
* provider guest code entered: yes
* provider entered at call depth `1`; execution then entered a second `sdk`
  function at `0x7204c4a3d0` at call depth `2`

The valid guest provider remained authoritative. No host function pointer,
host implementation, fake return value, or symbol-name special case was added.

### UMULH frontier

* real UMULH PC: `0x7204717c30`
* decoded operation: `umulh x9, x10, x9`
* operand identities: destination `x9`; sources `x10`, `x9`
* executed successfully: no
* next guest PC: not established because execution did not reach the
  instruction
* provider code legitimately entered: yes

The bounded observation target set derived from the provider CFG was
`0x7204717c30` and `0x7204717dd0`. The executed-observation list was empty in
both real runs. This directly establishes that the real controlled path did
not execute the M15 `UMULH` site; decoder support and synthetic execution were
not used as a substitute for that conclusion.

### First observed execution boundary

Because `UMULH` was not reached, there is no post-UMULH boundary to report.
The first genuine boundary after entering the provider closure was:

* first later module: `sdk`
* first later PC: `0x7204c4a3dc`
* owning function: `0x7204c4a3d0`
* typed stop reason: `unknown_guest_function`
* source PC: `0x7204c4a3dc`
* diagnostic instruction: an indirect branch boundary; no unsupported
  architectural instruction was executed at the stop
* resolved target: `0x72047b6870`
* target provenance: `guest_load:0x7205390468`
* target register: `x17`
* target owning module: `sdk`
* call depth: `2`

The source PC of the provider transfer remains distinct from the provider
target: `main:0x7202aa421c -> sdk:0x7204717880`. The bounded transition trace
was:

```text
main:0x7200000030 depth 0  [controlled DT_INIT candidate]
main:0x7202aa4210 depth 1  [direct call]
main:0x7202aa421c depth 1  [indirect provider transfer]
sdk:0x7204717880 depth 1  [guest provider entered]
sdk:0x7204c4a3d0 depth 2  [direct call]
sdk:0x7204c4a3dc depth 2  [indirect target rejected as unknown function]
```

The run did not return to `main`, tail-transfer outside `sdk`, or enter a
second module. It executed `442` IR operations across `8` guest blocks, with
`2` direct calls, `1` function transfer, and maximum call depth `2`.

## C. Public research

* Arm's public A64 documentation describes `BR` as an indirect branch using a
  general-purpose register target. This supports the report's distinction
  between the source PC, the register-derived target, and the target's trusted
  function ownership; it does not prove that an executable address is a valid
  function entry.
* The public Arm ABI repository provides AAPCS64 and AAELF64 references for
  future call/ELF reasoning: [ARM-software/abi-aa](https://github.com/ARM-software/abi-aa).
* Atmosphère's public loader documentation records the conventional ExeFS
  search order `rtld`, `main`, `subsdk0..9`, `sdk`. It is used only as public
  load-order evidence here, not as proof of Nintendo's private loader state:
  [Atmosphère loader documentation](https://github.com/Atmosphere-NX/Atmosphere/blob/master/docs/components/modules/loader.md).

No bootstrap model was inferred from those sources.

## D. Hypotheses and bootstrap status

* bootstrap required: not established
* exact evidence: execution stopped at an unknown trusted-function boundary
  before `0x7204717c30`; no TLS, loader-created DSO structure, MOD0 runtime
  object, or required startup stack layout was shown to be the causal failure
* modeled state added: no
* modeled state/provenance: none
* public loader behavior was not treated as proof of Nintendo runtime behavior
* remaining uncertainty: the guest value loaded at `0x7205390468` may be a
  function pointer or another callable target, but this is not established;
  the project currently lacks an exact trusted function record for
  `0x72047b6870`

## Determinism

* report schema: `5`
* report SHA-256 #1: `703ba6b7a48516227e5174bfe24ecd74e32636c7a631886cea044bdd0a17cc9f`
* report SHA-256 #2: `703ba6b7a48516227e5174bfe24ecd74e32636c7a631886cea044bdd0a17cc9f`
* byte-identical: yes

The reports contain no absolute local paths, usernames, timestamps, host
pointers, process IDs, temporary directory names, or proprietary instruction
bytes.

## Validation

* final local CTest count: `211/211`
* focused M17 tests: `2` test cases, `16` assertions
* ASan/UBSan: `211/211` passed in the existing `build-asan` configuration
* TSan: `211/211` passed in the existing `build-tsan` configuration
* local compiler: AppleClang via `/usr/bin/c++`; no local GCC claim
* GCC CI: required by `.github/workflows/ci.yml`; result recorded after PR CI
* LLVM 18 CI: required by `.github/workflows/ci.yml`; result recorded after PR CI
* MSVC CI: required by `.github/workflows/ci.yml`; result recorded after PR CI

## Dependencies

New dependencies: `0`.

## Exact next blocker

The real controlled path reaches `sdk:0x7204c4a3dc`, loads target
`0x72047b6870` from guest address `0x7205390468`, and stops with typed
`unknown_guest_function` because that aligned executable target is not an
exact trusted function entry; the real `sdk:0x7204717c30` `UMULH` remains
unexecuted behind this boundary.
