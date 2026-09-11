# Milestone 14 — Complete executable-set ingestion and provider closure

## Goal

Milestone 14 adds a bounded, reusable executable-module inventory layer and
connects it to the existing M13 `ProcessImage`, process-wide symbol namespace,
transactional relocation planner, function map, and controlled execution path.
The objective is to turn an informal provider-search boolean into an auditable
statement about the exact locally supplied executable set.

The milestone is intentionally evidence-led. It does not add a guessed
implementation for `__nnmusl_init_dso` and does not equate execution progress
with correctness.

## Exact M13 baseline

The implementation branch is based on M13 commit
`8d8bfe6a4236a88d60e34a983a09bc0b845a0ffc`, from the
`milestone-13-multi-module-provider-resolution` line of work. M13's prior base
was `8a8ee6f2ed936830a4fdc12393584bbe9a6dfe05`.

M13's historical real-input evidence consisted of one legally supplied `main`
module. Its provider search for `__nnmusl_init_dso` was incomplete and stopped
at `runtime_import_unimplemented`; those facts remain the prior frontier and
are not silently reclassified by M14.

## Non-goals and legal/content boundary

M14 consumes only already prepared, locally supplied NSO inputs. It does not
decrypt XCI, NSP, or NCA content; extract firmware; parse keys; derive Nintendo
keys; download proprietary content; bundle `hactool`; or commit binaries,
keys, firmware, SDKs, reports with private paths, or extracted assets.

M14 does not implement rtld startup, Horizon process startup, ASLR
reproduction, full dynamic linking, TLS bootstrap, services, graphics, audio,
input, filesystem HLE, or game-loop behavior. The controlled M11/M12/M13
`DT_INIT` experiment remains the execution entry path.

## Executable-set input model

The existing top-level `modules` object remains supported, as does the legacy
`provider_search_complete` boolean. A structured `module_set` object may name
an explicit module inventory or a prepared non-recursive directory. Module
paths are configuration-only and never enter deterministic reports.

`process-inspect` performs inventory, metadata analysis, and provider search
without guest execution:

```text
process-inspect --local-config config/local.json --json \
  --report build/reports/m14-process-inventory.json
```

It also accepts `--directory PATH`. The directory scanner examines regular
files only, does not require an `.nso` suffix, ignores non-NSO files with a
stable reason, rejects malformed NSOs, bounds entry/file counts, and sorts
inputs by logical name before processing. Symlinks and recursive structures
are not followed.

## Module inventory design

`analysis::ModuleSetInventory` owns the prepared input bytes and retains the
logical identity and evidence needed to explain the search: name provenance,
SHA-256, NSO Build ID, input size, flags, segment ranges, BSS range,
materialization status, selected analysis base and provenance, runtime-base
verification, MOD0/dynamic status, symbol and relocation counts, executable
mappings, and provider-index eligibility. It can be enriched from the
existing `ProcessImageSummary`; no second process representation is created.

Exact duplicate binary identity under different logical names is rejected by
the ingestion layer. Duplicate names, malformed/truncated NSOs, bounded-read
failures, and manifest identity mismatches are typed failures. A failed
inventory or process load is never published as a partial executable process.

## Completeness model and provenance

Completeness is represented by:

```text
incomplete
declared_complete
manifest_verified_complete
```

Every state carries a basis, including `legacy_config_false`,
`explicit_local_assertion`, `explicit_inventory`, `local_manifest_match`,
`target_manifest_match`, and `directory_scan_only`. A directory scan alone
therefore remains incomplete. The legacy `provider_search_complete: false`
configuration maps to `incomplete`; the legacy true value maps to
`declared_complete` with explicit-local-assertion provenance.

Manifest-verified completeness requires exact logical names, SHA-256 values,
and Build IDs. The committed TOTK manifest is still a template, so it cannot
establish target identity or real-game completeness.

## Target coherence

The inventory exposes `verified`, `partially_verified`, `unverified`, and
`conflicting` coherence. A target manifest with real identities can verify the
set; the current template intentionally cannot. Contradictory expected names,
hashes, Build IDs, or sizes fail deterministically. Filenames are only local
logical-name hints and never establish Nintendo linker precedence.

## Process construction

The M14 pipeline is:

```text
local configuration/directory
    -> ModuleSetInventory
    -> identity/completeness/coherence validation
    -> existing ProcessImage
    -> audited ProcessSymbolNamespace
    -> relocation plan and transactional application
    -> existing ProcessFunctionMap
    -> controlled execution
```

Analysis-only inspection deliberately parses and indexes providers without
claiming an executable relocation-applied process. Controlled execution uses
the normal checked `GuestMemory` and only an applied, validated process state.

## Process-wide symbol-provider search

All parsed dynamic symbols in all supplied modules are considered. The audited
namespace retains occurrences and exclusion reasons for undefined, local,
hidden, non-executable, out-of-range, and unsupported definitions. Eligible
candidate order is stable by symbol name, module name, and dynamic-symbol
index.

The generic policy inherited from M13 is: one strong definition wins; a single
weak definition is usable when no strong definition exists; multiple plausible
definitions remain ambiguous; hidden and local definitions are not exported
providers; and a `FUNC` definition must be in executable guest memory. No
filename or invented load order is used.

Provider addresses are checked `provider_base + st_value` addresses in the
guest domain. They are never consumer-base addresses or host pointers.

## Relocation behavior

The existing transaction validates every relevant relocation before any
loader-authorized write. `R_AARCH64_JUMP_SLOT` and other already-supported
generic relocation operations retain consumer, relocation, candidate, provider,
base, and application provenance. Unknown relocation types remain failures
with module, index, raw type, offset, and symbol context; they are not treated
as relative relocations, zeroed, or skipped.

The generic AArch64 relocation and PLT rules are cross-checked against Arm's
[AAELF64 specification](https://github.com/ARM-software/abi-aa/blob/main/aaelf64/aaelf64.rst)
and calling-convention constraints against
[AAPCS64](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst).
NSO/MOD0 metadata interpretation is kept separate from generic ABI claims and
is documented against the public [NSO0](https://switchbrew.org/wiki/NSO0),
[MOD](https://switchbrew.org/wiki/MOD), and
[Rtld](https://switchbrew.org/wiki/Rtld) references. Nintendo-specific lookup
precedence is not claimed where public evidence does not establish it.

## `__nnmusl_init_dso` investigation

The focus search uses parsed dynamic-symbol records, not string-table matches.
For every occurrence it reports the module, symbol index, definition state,
binding, type, visibility, section, raw value, computed guest address when
valid, executable status, eligibility, and exclusion reason. The resulting
typed states distinguish:

```text
provider_resolved
provider_not_found_complete
provider_search_incomplete
provider_ambiguous
provider_ineligible
```

If the complete set has no eligible definition, the result is
`provider_not_found_complete`; M14 still preserves the runtime boundary and
does not return zero, no-op, guess a prototype, or bind unrelated host libc.

## Real module inventory

No prepared local executable-module set or local M14 configuration was
available in the checked-out workspace during this implementation. Therefore
the real complete-set experiment was not claimed, no private inventory was
recorded, and no real module count, hashes, Build IDs, or report hash was
fabricated. The historical M13 one-module evidence remains an incomplete
prior observation.

When a legally supplied set is available, `process-inspect` is the required
first step. It records the number of modules supplied, parsed, executable,
their identities and metadata status, completeness basis, coherence, and all
focus-symbol occurrences before `run-entry` is attempted.

## Real provider result and controlled execution result

The real M14 provider result is explicitly **search incomplete because the
prepared local executable set was unavailable**. Consequently there is no
real M14 `__nnmusl_init_dso` candidate count, no real JUMP_SLOT application,
and no real controlled execution result to report.

The synthetic M14 execution fixture does establish the implementation
frontier: a consumer `R_AARCH64_JUMP_SLOT` is filled with the provider module's
guest address; a PLT-style `BR` enters the dynamically seeded provider as a
tail transfer; `X30` and call depth remain correct; and a same-name implemented
runtime handler is not invoked. This is synthetic validation, not a claim
about the TOTK executable set.

## Deterministic reports and privacy

Execution and inspection reports use schema version 4 for process/module-set
evidence. Arrays are explicitly ordered. Reports contain logical names,
cryptographic identities, guest-domain addresses, and stable diagnostics, but
no absolute paths, usernames, timestamps, UUIDs, host pointers, ASLR-derived
host addresses, keys, or proprietary bytes. The module-set inventory and the
existing execution renderer use canonical key construction and stable order.

Real report SHA-256 values are intentionally not listed because no real report
could be generated without the missing local executable set. Deterministic
synthetic inventory and report-equivalence tests remain required before any
real report is accepted.

## Tests and validation

M14 adds focused coverage for deterministic two-module inventories, ignored
non-NSO directory entries, malformed inputs, duplicate logical/binary identity,
manifest identity matching, completeness provenance, audited provider
occurrences, summary propagation, provider-base JUMP_SLOT application, and
runtime-handler precedence with a cross-module tail transfer.

The M13 PR #18 CI was green across the recorded Linux GCC, Linux LLVM 18,
Linux sanitizer, and Windows MSVC jobs before this branch was started. The
baseline suite contained 185 CTest cases before M14. The final Debug suite
contains 195 CTest cases; all 195 pass. The ten M14 cases contain 90 Catch2
assertions. The separate AppleClang 17 ASan/UBSan and TSan configurations also
pass all 195 tests. Ninja, GCC, LLVM 18, and MSVC were not available in this
macOS workspace; they remain CI responsibilities and are not claimed as local
tests.

## Exact next blocker

The next genuine blocker is the missing legally supplied, exact-build,
complete executable module set. Once available, the next investigation is to
run the complete parsed-symbol search for `__nnmusl_init_dso`. If it has no
eligible provider, the following milestone must investigate its runtime
contract, DSO/TLS metadata, call-site ABI, and loader semantics. M14 itself
does not implement that contract speculatively.
