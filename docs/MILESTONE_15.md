# Milestone 15 — Real executable-set closure and `__nnmusl_init_dso`

## 1. Exact baseline

This branch was created directly from M14:

```text
branch: milestone-14-complete-executable-set
SHA:    15690d04c573b9cc386ea57d92ba5a71937c90c0
schema: 4
baseline CTest: 195/195
```

The M14 `ModuleSetInventory`, `ProcessImage`, process-wide symbol namespace,
provider-base relocation handling, dynamic-symbol function seeds, cross-module
execution, and runtime-handler precedence architecture are retained.

## 2. Objective and legal boundary

The objective was to determine whether the complete prepared executable set
contains an eligible guest definition of `__nnmusl_init_dso`, and to take the
next controlled execution step only from that evidence.

M15 consumes prepared local NSOs only. It does not decrypt XCI, NSP, or NCA
files, derive or read title keys, extract firmware, download binaries, bundle
Nintendo SDK material, or commit Nintendo binaries or private reports.

## 3. Local executable-set state

A prepared local ExeFS directory was available outside the repository. The
inventory found four NSOs and one non-NSO `main.npdm` entry. No authoritative
sidecar manifest containing the expected module identities was present.

Two evidence states are therefore kept separate:

* an ordinary directory scan remains `incomplete` with basis
  `directory_scan_only`; it cannot select a provider;
* the controlled experiment used a private, uncommitted configuration with
  `declared_complete` and basis `explicit_local_assertion`. This records the
  user's local assertion for the experiment; it is not
  `manifest_verified_complete`.

The four files were internally parseable and their metadata was coherent
enough for a `partially_verified` experiment, but the absence of an
authoritative exact-build manifest means M15 does not claim independently
verified exact-build coherence.

## 4. Exact module inventory

The deterministic analysis layout and identities were:

| logical name | input size | Build ID | SHA-256 | flags | dynsym (defined/undefined) | relocations | selected base |
|---|---:|---|---|---:|---:|---:|---|
| `rtld` | 7,140 | `7501abfe55fa41cffeb46bd619bdcbf45b9cae3a000000000000000000000000` | `28317605926caa40a5cbee3e3cb290c65783b4d9f8c3697b43ca41ab2866a44d` | 63 | 19 (0/19) | 38 | `0x7204676000` |
| `main` | 36,549,681 | `082ce09b06e33a123cb1e2770f5f9147709033db000000000000000000000000` | `faf81f8a609ff61a6322d04948132b5929af9f6e2398e0e0ebbe819b8ba3eaab` | 63 | 704 (29/675) | 504,436 | `0x7200000000` |
| `subsdk0` | 3,428,817 | `a6bb7c6ffa9673769fa74ed3d7b054191f25d29c000000000000000000000000` | `9d04c98997977e99c12a48a2dbfd3064b8da96b5152e0119064957a43f6c3f99` | 63 | 11,622 (10,954/668) | 19,397 | `0x7205473000` |
| `sdk` | 6,132,082 | `b9046c31eb5d31271be970fe732d38df49c6aa21000000000000000000000000` | `91573d4444d0dcd07080db78fece2b098b76080d77cdb2e1d301a2a41d7c4eea` | 63 | 27,761 (27,747/14) | 35,032 | `0x720468a000` |

Every module had MOD0 and dynamic metadata available and an executable
mapping. Duplicate binary identity checks remain enforced.

## 5. Load-order evidence

The deterministic reported load order is:

```text
rtld -> main -> subsdk0 -> sdk
```

Public Switch research describes this as the usual physical ExeFS loading
order. Atmosphère's public loader documentation likewise lists these NSO
names and says found NSOs are loaded contiguously and that the first loaded
NSO supplies the process entrypoint in the usual case. See the public
[Atmosphère loader documentation](https://github.com/Atmosphere-NX/Atmosphere/blob/master/docs/components/modules/loader.md)
and [Switch memory-layout notes](https://switchbrew.org/wiki/Memory_layout).

M15 models this as load-order evidence only. It is not used to resolve ties.
No public source located during this milestone establishes the complete
Nintendo symbol-lookup precedence contract. Multiple eligible strong
providers therefore remain `ambiguous_guest_provider`.

The public [NSO documentation](https://switchbrew.org/wiki/NSO0),
[MOD0 documentation](https://switchbrew.org/wiki/MOD), and the public
[rtld notes](https://switchbrew.org/wiki/Rtld) support the separate concepts
of NSO images, MOD0/dynamic metadata, module construction, relocation, and
startup. Public open-source implementations were used as format/loader
cross-checks, including [yuzu's NSO loader](https://source.hodakov.me/hdkv/yuzu/src/commit/e24b89646465c4161aa073f667f11f64c535c0b7/src/core/loader/nso.cpp)
and the [Atmosphère loader implementation documentation](https://github.com/Atmosphere-NX/Atmosphere/blob/master/docs/components/modules/loader.md).
They are not treated as proof of TOTK-specific lookup precedence.

## 6. Provider-search algorithm

The search uses parsed dynamic-symbol records from every module. It never
interprets a string-table occurrence as a definition. Each occurrence retains
module, dynamic-symbol index, definition state, binding, type, visibility,
section index, `st_value`, checked address, mapping state, executable state,
eligibility, and exclusion reason.

An eligible provider must be defined, externally eligible, visible, supported,
checked into the process guest address space, and executable when it is a
function. One strong provider wins; one weak provider is usable when no strong
provider exists; multiple eligible providers remain ambiguous. Crucially, an
eligible candidate found during an incomplete search is retained as evidence
but is not selected.

The generic relocation arithmetic is cross-checked against Arm's public
[AAELF64 specification](https://github.com/ARM-software/abi-aa/blob/main/aaelf64/aaelf64.rst)
and the call/return model against [AAPCS64](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst).
The project does not copy proprietary source.

## 7. All `__nnmusl_init_dso` occurrences

The complete parsed search over the four supplied modules found exactly three
occurrences:

| module | dynsym index | defined | binding | type | visibility | section | `st_value` | checked guest address | executable | eligibility |
|---|---:|---|---|---|---|---:|---:|---|---|---|
| `main` | 5 | no | `GLOBAL` | `FUNC` | `DEFAULT` | 0 | `0` | — | no | `undefined` |
| `sdk` | 8767 | yes | `GLOBAL` | `FUNC` | `DEFAULT` | 1 | `0x8d880` | `0x7204717880` | yes | `eligible` |
| `subsdk0` | 5 | no | `GLOBAL` | `NOTYPE` | `DEFAULT` | 0 | `0` | — | no | `undefined` |

There was no eligible occurrence in `rtld`. The provider result under the
declared local set is `resolved_guest_module`, selected candidate 0, module
`sdk`, dynamic-symbol index 8767. Because the local set lacks an authoritative
manifest, the stronger statement “complete exact-build closure is verified”
remains not established.

## 8. Relocation closure

For `main`, the known import remains an undefined dynamic symbol 5 reached by
the `DT_JMPREL` table as `R_AARCH64_JUMP_SLOT`. Its parsed relocation index is
503796, target slot `0x720453ef80`, source `JMPREL`, and addend zero. The
selected value is checked as:

```text
provider base  0x720468a000
+ st_value     0x00008d880
= guest target 0x7204717880
```

The slot was applied transactionally and read back as exactly
`0x7204717880`. The `subsdk0` import was also resolved to the same `sdk`
provider (relocation index 15473, slot `0x72059f7ed0`). No consumer base and
no host address is used. Reports retain provider module, provider symbol
index, provider base, `st_value`, resolved value, and slot-readback status.

Relocation application now snapshots the write set and restores it if a commit
write fails. Focused tests cover provider-base arithmetic, wrong-consumer-base
regression, invalid/ambiguous providers, overflow paths, and transaction
failure behavior.

## 9. Runtime-handler precedence

The runtime registry is consulted only after process binding evidence says
`provider_not_found_complete`. Incomplete, ambiguous, ineligible, invalid, or
missing process bindings produce a typed unresolved boundary even when a
same-name runtime descriptor is registered. A valid guest provider therefore
cannot be masked by `RuntimeImportRegistry["__nnmusl_init_dso"]`.

M15 tests cover a strong provider, weak-only provider, ambiguous providers,
incomplete search, complete no-provider search, and simultaneous runtime
descriptors. The real run encountered no runtime import and the runtime
fallback counter remained zero.

## 10. ABI and DSO/TLS evidence

The real provider wins, so the conditional no-provider ABI investigation was
not used to invent a prototype. The existing M12 evidence remains the only
runtime-contract observation: ten incoming argument slots were observed
(`X0`–`X7`, `[SP]`, `[SP+8]`) without assigning semantic types.

The real run did not invoke a runtime DSO/TLS handler, did not register DSO
state, and did not mutate runtime TLS state. MOD0 and dynamic metadata parsed
successfully in all four modules, but parsing those structures alone does not
prove the state normally created by Nintendo `rtld`. No host pointer or guessed
return value was introduced.

## 11. Controlled execution result

The controlled entry remains the existing metadata-selected `DT_INIT` candidate
at `0x7200000030`; it is explicitly not relabeled as the Nintendo process
entry. A focused, bounded run analyzed the startup/provider closure only.

Observed result:

```text
entry:                 0x7200000030 (main DT_INIT candidate)
functions entered:     0x7200000030, 0x7202aa4210 (both main)
modules entered:       main
import trampoline:     reached; target loaded from main slot
resolved target:       0x7204717880 in sdk
runtime fallback:      not invoked
stop reason:           unsupported_instruction
stop PC:               0x7202aa421c (boundary source PC)
diagnostic instruction: umulh x9, x10, x9 at 0x7204717c30
call depth:            1
IR operations:         319
guest blocks:          3
```

The execution engine validated the target as an executable guest address and
the next lift boundary was in the provider module. The provider function was
not counted as entered because its owning function could not be lifted past
the unsupported `umulh`; `provider_guest_code_entered` is consequently false.
This is a genuine ISA/lifting boundary, not a runtime replacement and not a
claim that TOTK boots or runs.

The result also leaves open a second startup question: public rtld evidence
describes loader-created module and startup state that the current controlled
`DT_INIT` experiment does not recreate. M15 does not emulate that state
accidentally.

## 12. Determinism and report schema

Schema 4 remains sufficient; no schema version was introduced. New fields
record load-order basis, provider-base arithmetic, resolved slot value,
readback verification, and runtime fallback eligibility without changing the
schema number. Reports contain no absolute host paths, usernames, timestamps,
UUIDs, host pointers, keys, or proprietary bytes.

The same focused real experiment was run twice with identical inputs and
budgets. The reports were byte-identical:

```text
SHA-256 #1  2aa7092de5f0329995f27943e7958e56ed6fccfe949d76dab355c9756fc2ce1e
SHA-256 #2  2aa7092de5f0329995f27943e7958e56ed6fccfe949d76dab355c9756fc2ce1e
```

The reports are private and are not committed.

## 13. Tests and CI

The baseline was 195/195 CTest. M15 adds seven focused CTest cases and 87
Catch2 assertions covering load-order evidence, incomplete-provider gating,
candidate/exclusion/ambiguity evidence, JUMP_SLOT readback, runtime fallback
precedence, and complete no-provider eligibility. The final local Debug suite
is 201/201.

Local AppleClang builds and tests are the evidence available in this macOS
workspace. Linux GCC, Linux LLVM 18, ASan/UBSan, TSan, and Windows MSVC are
CI matrix responsibilities unless their jobs are available; they are not
claimed as locally passed merely because the ordinary build passes.

## 14. Dependencies and privacy

No new dependency was added. The prepared NSOs, private local configuration,
and real reports remain outside the repository and ignored by the existing
local-content rules. Only synthetic fixtures were committed.

## 15. Exact next blocker

M15 conclusively closes the guest-provider question for the declared local
four-module set: `sdk` supplies `__nnmusl_init_dso`, and the `main` and
`subsdk0` JUMP_SLOTs resolve to its guest address. It does not claim
manifest-verified exact-build completeness because no authoritative identity
manifest was supplied.

The immediate execution blocker is lifting the provider's owning function past
`umulh` at `0x7204717c30`. After that, execution must test whether the provider
requires state established by the real `rtld`/process bootstrap. The next
architecture milestone should therefore be an evidence-based AArch64 lifting
extension for this boundary followed, if required by observed state, by
minimal rtld/process bootstrap—not a host `__nnmusl_init_dso` replacement.
