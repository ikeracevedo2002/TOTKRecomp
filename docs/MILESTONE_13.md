# Milestone 13 — Multi-module guest linking and provider resolution

## Goal

Milestone 13 adds the next truthful layer after the M12 runtime-import
boundary: a deterministic process image containing multiple prepared NSO
modules, a process-wide guest symbol namespace, transactional cross-module
relocations, and module-aware execution. The rule is now:

```text
undefined guest import
        |
        v
process guest-provider lookup
        |
        +-- unique proven provider --> guest relocation --> guest execution
        |
        +-- no proven provider ------> unresolved boundary --> runtime registry
```

The runtime registry remains available for evidence-backed HLE. It is no
longer the first destination merely because the primary module's symbol is
undefined.

## Baseline

The branch was created from the exact M12 commit
`8a8ee6f2ed936830a4fdc12393584bbe9a6dfe05`, whose parent is
`09aed504887e9c7b009c21ab42a68d85538695a7`. Before M13 changes, the complete
test suite passed 179/179 tests.

The M12 real prepared-main baseline was preserved: analysis base
`0x7100000000`, base verification `false`, `DT_INIT` candidate
`0x7100000030`, and the first boundary was the tail-transfer import
`__nnmusl_init_dso` at `0x7102aa421c`. The M12 report hash was
`b1fa9d658e5ec7ed8ae38f7fb858d37ad7af4e69207623b5b95129b8fb931725`.

## Non-goals

This milestone does not implement Horizon, the kernel, rtld startup, a
complete dynamic linker, TLS bootstrap, services, filesystem, graphics, game
loop, or full AArch64 coverage. It does not claim that TOTK boots and does not
turn `__nnmusl_init_dso` into a guessed host stub.

## Process/module architecture

`analysis::ProcessImage` is a reusable, temporary-until-valid process model.
It accepts arbitrary logical module names and prepared NSO bytes, parses and
retains each module's NSO header/image, MOD0, dynamic metadata, dynamic
symbols, relocations, seeds, mappings, and provenance. Modules are sorted by
logical identity for deterministic construction and reporting. Host paths are
never part of module identity or generated reports.

`analysis::ProcessSymbolNamespace` is the reusable process-level provider
index. `analysis::ProcessFunctionMap` keeps finalized function maps separate
per module while offering deterministic process-wide address ownership.

The loader constructs a complete temporary guest memory image, validates every
module range and relocation, and exposes the process only after the plan has
been applied successfully. A failed load therefore cannot publish a usable
half-relocated process.

## Address-layout provenance

Automatic module bases use a checked, page-aligned deterministic cursor
starting at `0x7200000000`, with a deterministic gap and explicit-base
collision avoidance. Locally supplied bases override that assignment. The
provenance is explicit:

* `explicit_analysis_base` — supplied by local analysis configuration;
* `deterministic_analysis_layout` — assigned by the process builder;
* `externally_observed` and `runtime_verified` — reserved for stronger future
  evidence.

The first two are analysis addresses, not retail Switch ASLR observations.
`runtime_base_verified` remains `false` unless independent runtime evidence
establishes the address.

Mappings preserve text/rodata/data/BSS permissions, zero-filled BSS, checked
range arithmetic, and no-overlap ownership. Reserved stack ranges are checked
against module ranges.

## Guest symbol namespace and provider candidates

Defined, non-local, default/protected global or weak symbols are indexed with
their logical module, dynamic symbol index, raw module-relative value, checked
guest address, binding, type, visibility, section index, and executable state.
Hidden/local/undefined symbols are not providers. Function providers must lie
in an executable mapping. Non-function linker boundary definitions outside the
mapped image are retained as non-provider metadata rather than being guessed
into a binding.

Provider discovery is separate from provider selection. A single eligible
strong definition is unambiguous; multiple plausible definitions are reported
as ambiguous because Nintendo-specific lookup ordering has not been
established here. A single weak definition can resolve when no strong
candidate exists. No filename is treated as semantic proof.

Every process binding retains the consumer module, symbol index/name,
relocation index/type/source/target, candidate list, result status, provider
module/symbol/address when selected, resolution basis, confidence, and
post-plan applied state.

## Cross-module relocation design

M13 reuses the M5 relocation planner and loader-only write path. All modules'
metadata are parsed first; local definitions and process-wide providers are
then resolved; target ranges, checked arithmetic, supported relocation types,
and write permissions are validated before any relocation bytes are committed.
The supported AArch64 forms remain `ABS64`, `ABS32`, `GLOB_DAT`, `JUMP_SLOT`,
and `RELATIVE`; unsupported forms remain errors.

Provider addresses are always `memory::GuestAddress` values computed with the
provider module's base. Host function pointers never enter guest memory. An
applied relocation retains its original binding record, so a later branch can
still be explained as consumer import → relocation slot → provider guest
address.

## Runtime import precedence

For each undefined symbol the process linker is consulted first. A resolved
guest definition is relocated and is not counted as a runtime import. An
ambiguous, incomplete, or not-found provider remains an unresolved guest
boundary. Only that unresolved boundary reaches `RuntimeImportRegistry`,
preserving M12's `implemented`, `known-unimplemented`, and `unknown` outcomes.

## Execution across modules and tail transfers

Execution sessions now accept a process function map and process image. The
dispatcher resolves the exact owner for a guest PC, requires executable
ownership and a finalized function record, and rejects unmapped,
non-executable, ambiguous, or unknown targets with typed stops.

`BR`/`B` provider transfers remain tail transfers: they preserve X30 and do not
create a continuation frame. The synthetic regression covers
`BL caller → PLT-style BR provider → RET`; it observes one call, one function
transfer, two returns (provider and caller), depth one, and zero runtime-import
events.

## `__nnmusl_init_dso` investigation

The only legally supplied local executable in this workspace is the logical
module `main`. No additional NSO was present in the local configuration, so
the provider search is explicitly incomplete rather than exhaustive.

The supplied module inventory is:

| Logical module | SHA-256 | Build ID | NSO flags | Dynamic symbols | Defined | Undefined | Relocations |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| `main` | `faf81f8a609ff61a6322d04948132b5929af9f6e2398e0e0ebbe819b8ba3eaab` | `082ce09b06e33a123cb1e2770f5f9147709033db000000000000000000000000` | `0x3f` | 704 | 29 | 675 | 504436 |

The dynamic table contains MOD0, `DT_SYMTAB`, `DT_STRTAB`, `DT_RELA`, and
`DT_JMPREL` metadata. Searching the supplied dynamic symbol table found one
occurrence of `__nnmusl_init_dso`: main symbol index 5, undefined, global,
`FUNC`, default visibility. There is no defined candidate in the supplied
module, so the result is `provider_search_incomplete`, candidate count zero,
and the main `R_AARCH64_JUMP_SLOT` at guest slot
`0x710453ef80` remains unresolved.

The real M13 run therefore correctly follows:

```text
main DT_INIT → main PLT thunk → unresolved guest boundary
             → M12 RuntimeImportRegistry → known unimplemented stop
```

No host handler was executed and no provider module was entered. This is not a
claim that the symbol is a Horizon service; it is only a statement about the
one supplied module. The next evidence needed for a provider decision is the
rest of the legally supplied executable set, searched by dynamic-symbol
metadata rather than filename.

## Public evidence and uncertainty

Public [NSO0 documentation](https://switchbrew.org/wiki/NSO0) establishes the
NSO segment/module identity fields used for inventory. Public
[MOD documentation](https://switchbrew.org/wiki/MOD) documents the MOD0
dynamic/BSS/module-offset metadata used by the parser. Public Switchbrew
[rtld research](https://switchbrew.org/wiki/Rtld) supports the high-level fact
that rtld participates in module discovery, relocation, and symbol resolution;
it does not establish the exact Nintendo module lookup order needed to choose
between multiple candidates. The relocation names and generic AArch64 ELF
semantics follow the public
[AAELF64 specification](https://github.com/ARM-software/abi-aa/blob/main/aaelf64/aaelf64.rst).
The [AAPCS64](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst)
register/stack model supports M12's observed-call description but does not
turn ten observed argument slots into a complete `__nnmusl_init_dso`
prototype.

Observed facts, public facts, inferred policy, and unverified assumptions remain
separate. In particular, deterministic analysis bases are not runtime bases;
the symbol name is not proof of a provider; and public generated declarations
are interface-level evidence only.

## Tests and determinism

`tests/milestone13_multimodule_tests.cpp` covers deterministic module layout,
explicit bases, overlap/overflow/stack/duplicate rejection, provider-base
address construction, hidden/undefined/non-executable provider rejection,
ambiguous candidates, process function ownership, and the PLT-tail-transfer
execution contract. Existing M12 tests remain in the same test executable.

The real single-module M13 CLI invocation produced schema 3 and retained the
M12 stop: `runtime_import_unimplemented` at `0x7102aa421c`, with one direct
call, zero indirect calls, zero function transfers, zero returns, 319 IR
operations, 3 guest blocks, maximum depth 1, and zero memory faults.

Two fresh reports were byte-identical:

```text
first report SHA-256:  1292177c75fb820f3f3d01e379c76c856ef42dcd0c22463357798636f479dc5f
second report SHA-256: 1292177c75fb820f3f3d01e379c76c856ef42dcd0c22463357798636f479dc5f
byte-identical:       yes
```

Reports contain logical module identities, hashes/build IDs, guest addresses,
and deterministic metadata only. They do not contain source paths, usernames,
keys, host pointers, timestamps, or private asset locations. The private real
reports remain under ignored build output and are not committed.

## Exact next blocker

The architecture is ready for multiple supplied modules, but this workspace
does not contain the additional legally supplied NSOs required to prove a
guest provider for `__nnmusl_init_dso`. M13 therefore stops at the same honest
runtime boundary. The next investigation is to populate local-only module
configuration with the complete available executable set, validate each NSO's
identity and metadata, repeat the symbol search, and select a provider only if
the candidate set is unambiguous and executable. No host implementation should
be added on the basis of the current evidence.

## Safety and legal constraints

The repository commits only generic infrastructure, synthetic fixtures, and
safe configuration templates. It does not commit proprietary NSOs, firmware,
keys, SDK source, extracted assets, or private reports. Local inputs must be
legally supplied by the user.
