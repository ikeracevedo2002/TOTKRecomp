# Milestone 12 — Evidence-driven runtime import boundary

## Goal

Milestone 12 adds a first-class, ABI-aware boundary between guest execution
and runtime implementations. It preserves guest addresses and relocation
provenance, records how an import was reached, validates AArch64 call state,
and distinguishes an unknown import from a known import that is deliberately
not implemented.

The result for the first real dependency is intentionally conservative:
`__nnmusl_init_dso` is recognized and measured, but remains an explicit
`runtime_import_unimplemented` boundary. No guessed return value or host
function-pointer binding was added.

## Non-goals

This milestone does not implement Nintendo's loader, full `rtld` startup,
multi-module loading, a complete Switch TLS model, Horizon, services,
filesystem, graphics, audio, input, game-loop startup, or game boot. It also
does not claim that the synthetic M9 TLS/thread state is Nintendo's loader TLS
state.

## M11 frontier

The local prepared main NSO was entered from the metadata-selected `DT_INIT`
candidate. M11 stopped at:

```text
PC:       0x7102aa421c
function: 0x7102aa4210
reason:   unresolved_import
symbol:   __nnmusl_init_dso
X17:      0x0000000002aa41f0
slot:     0x710453ef80
```

The analysis base remains `0x7100000000` and `guest_base_verified` remains
`false`. The entry is `DT_INIT` at `0x710000000030` and is not a verified
process entry. The synthetic stack, LR sentinel, and zero TLS state remain
explicitly synthetic.

## Import architecture

`RuntimeImportRegistry` is an ordered, session-supplied registry keyed by the
exact symbol name after the execution-side relocation index has established
the import boundary. Each entry contains a subsystem, support status, partial
ABI signature, and evidence record. Duplicate registrations fail, enumeration
is deterministic, and no native function pointer is exposed.

`RuntimeImportContext` gives a handler only `AArch64GuestCall`, checked
`GuestMemory`, session-local `RuntimeState`, the descriptor, the copied
relocation provenance, invocation kind, and optional parsed module metadata.
Guest addresses remain integers in the guest address domain. A handler cannot
turn one into a host pointer through the runtime interface.

The registry reports four distinct outcomes:

1. no descriptor for a provenance-backed import: `unresolved_import`;
2. a registered but unimplemented descriptor:
   `runtime_import_unimplemented`;
3. a handler or ABI/memory validation failure with a typed runtime stop;
4. a handled import with an explicit ABI return-register write.

## AArch64 ABI model

The reusable `AArch64GuestCall` model follows the AArch64 Procedure Call
Standard: integer and pointer arguments 0–7 are X0–X7, later 8-byte argument
slots are read from `[SP + (index - 8) * 8]`, and SP must be non-zero and
16-byte aligned. When stack argument slots are required, SP must point into a
mapped guest range. Stack locations use checked 64-bit arithmetic and checked
`GuestMemory.read`; the host stack is never inspected. Integer32 returns are
written as the zero-extended W0 result, while integer64 returns write X0.

The import observation records X0–X7, SP, X29, X30, and PC, followed by each
observed argument slot, mapping, permissions, region kind, alignment, and
safe metadata relationships. A pointer's point mapping is not treated as
proof that an unknown-length range is valid; handlers must validate their own
known extents.

The ABI register/alignment model is based on the public
[AAPCS64 specification](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst).

## Invocation and continuation semantics

The existing distinction between guest calls and tail transfers is preserved.
`BL`/`BLR` are calls and retain an explicit guest continuation. `B`/`BR` are
tail transfers and do not manufacture a continuation. A handled tail import
inherits the current guest frame's expected return contract; a handled call
resumes the boundary's explicit continuation block and PC. Synthetic tests
cover both forms, including a `BL` to a thunk ending in `BR` to an import.

Runtime imports do not increment the guest `function_transfers` counter. Their
separate `runtime.import_returns` counter records a handled external return;
ordinary guest `RET` counts remain separate and auditable.

## Provenance and trampoline investigation

The real import has the following parser-proven facts:

```text
dynamic symbol index: 5
name:                 __nnmusl_init_dso
binding:              GLOBAL
type:                 FUNC
visibility:           DEFAULT
section index:        SHN_UNDEF / 0
undefined in main:    yes
relocation index:     503796
relocation type:      1026 / R_AARCH64_JUMP_SLOT
relocation source:    DT_JMPREL
relocation addend:    0
relocation slot:      0x710453ef80
DT_PLTGOT:            0x710453ef68
slot minus DT_PLTGOT: 0x18 (reported comparison, not a GOT-size claim)
DT_JMPREL:            0x710362ef30
DT_PLTRELSZ:          0x3c00
DT_JMPREL count:      640
```

The relocation slot is the exact provenance carried by the interpreter's
guest-load analysis. The finalized guest function at `0x7102aa4210` has one
owned basic block and ends at the exact boundary PC with `BR X17`. Its earlier
instructions form the usual address/load/add sequence, but the classification
does not rely on appearance: exact ownership, exact boundary PC, relocation
slot provenance, `JMPREL` source, and `R_AARCH64_JUMP_SLOT` type all agree.
The report therefore classifies it as `import_trampoline` and retains the
parsed `DT_PLTGOT` value for comparison. Arbitrary indirect branches remain
ordinary indirect boundaries.

## `__nnmusl_init_dso` evidence and contract

The measured caller places eight values in X0–X7 and two further values at
`[SP]` and `[SP+8]`. The M12 report records these ten observed slots, but does
not promote them to a formal prototype. The public generated
[nnsdk declaration](https://docs.rs/nnsdk/latest/src/nnsdk/lib.rs.html) exposes
the symbol name/declaration only; it does not establish the parameter types,
provider module, side effects, or return contract. No Nintendo SDK/runtime
source was copied.

The call-site behavior is separately known: the caller stores W0 after the
call and continues through its own conditional initialization path. That is
not enough to establish what the callee's return value means. Consequently
M12 does not rely on a `0 == continue`, `1 == skip`, or any other guessed
interpretation.

The provider classification is also not established. The symbol is an
undefined main-module dynamic import, but this observation alone does not
prove whether its provider is another Switch module or a loader/runtime
service. A host replacement is therefore not justified. Multi-module/provider
evidence is deferred.

## DSO/TLS investigation

The real NSO parser provides MOD0, dynamic metadata, NSO segment mappings, and
the main module's BSS bounds. It does not provide ELF section names sufficient
to label the ten call arguments as `.tdata` or `.tbss`. The observed BSS range
is `0x71045979a0`–`0x7104665c60`; it is not relabeled as TLS.

Status of the relevant facts:

| Item | Status |
| --- | --- |
| NSO text/rodata/data/BSS mappings | Observed and checked |
| MOD0 dynamic/BSS metadata | Observed and parsed |
| `.tdata` template range for this call | Needs verification |
| `.tbss` range for this call | Needs verification |
| TLS template size/alignment | Needs verification |
| module TLS registration semantics | Needs verification |
| M9 per-thread CpuState/TLS subset | Implemented synthetic state only |
| generic checked DSO/TLS range descriptor | Implemented and unit-tested |
| Nintendo initial-thread loader TLS | Deferred |

The generic `DsoTlsDescriptor` validator checks ordering, overflow, pairwise
overlap, power-of-two non-zero alignment, mapping, and sensible permissions.
`RuntimeState::register_dso_tls` validates a proposed descriptor and commits it
transactionally. M12 does not use it to claim that the real import has been
implemented.

## Implemented and deliberately unimplemented

Implemented:

- first-class runtime import descriptors and an ordered registry;
- reusable AArch64 register/stack ABI extraction and explicit return writes;
- relocation-backed import provenance and invocation-kind reporting;
- evidence-based import-trampoline classification;
- checked guest-memory isolation for runtime handlers;
- session-local runtime state and transactional generic DSO/TLS registration;
- typed runtime stop reasons and deterministic runtime report records;
- real-call ABI diagnostics without proprietary memory fixtures.

Deliberately unimplemented:

- `__nnmusl_init_dso` itself;
- a guessed return value;
- a direct host binding;
- DSO/TLS side effects for the real NSO;
- provider/module selection without independent evidence.

## Synthetic tests

`tests/milestone12_runtime_tests.cpp` adds 8 test cases and 124 assertions.
They cover deterministic registry lookup/enumeration, duplicate and unsupported
registration, a successful controlled handler, X0–X7 and stack arguments,
invalid SP, unmapped/read-permission failures, checked arithmetic overflow,
32/64-bit returns, guest-call continuation, tail-import inheritance, unknown
versus known-unimplemented stops, checked handler memory faults, transactional
DSO/TLS registration, invalid overlap/alignment/mapping/permissions, and the
synthetic ten-slot `nnmusl` evidence model.

## Real local validation

Validation used only the user's legally supplied local prepared NSO. It was not
committed. The final invocation used the M11 budgets and:

```text
SHA-256:       faf81f8a609ff61a6322d04948132b5929af9f6e2398e0e0ebbe819b8ba3eaab
Build ID:      082ce09b06e33a123cb1e2770f5f9147709033db000000000000000000000000
analysis base: 0x7100000000
base verified: false
entry:         DT_INIT, 0x7100000030, verified_process_entry=false
```

The M12 result is:

```text
old M11 frontier reached: yes
recognized:               yes
handled:                  no
advanced beyond M11:     no
next stop reason:         runtime_import_unimplemented
next stop PC:             0x7102aa421c
next import:              __nnmusl_init_dso
invocation:               tail_transfer
functions entered:       0x7100000030, 0x7102aa4210
runtime imports:          encountered=1, resolved=1, handled=0, unimplemented=1
direct calls:             1
indirect calls:           0
function transfers:       0
returns:                  0
IR operations:            319
guest blocks:             3
maximum call depth:       1
memory faults:            0
```

The ABI snapshot records `X0`–`X7`, `SP=0x7104775f70`,
`X29=0x7104775fe0`, `X30=0x710000011c`, and the unresolved raw target PC
`0x0000000002aa41f0`. The raw target remains distinct from the provenance
boundary PC; it is not converted into a host address.

## Determinism

Two fresh `run-entry` processes materialized and initialized independent guest
and runtime state. Their final JSON reports were byte-identical:

```text
first report SHA-256:  b1fa9d658e5ec7ed8ae38f7fb858d37ad7af4e69207623b5b95129b8fb931725
second report SHA-256: b1fa9d658e5ec7ed8ae38f7fb858d37ad7af4e69207623b5b95129b8fb931725
byte-identical:       yes
```

The report is a private build artifact and is not committed. No timestamps,
host pointers, thread IDs, random IDs, usernames, private paths, XCI paths, or
keys paths are emitted.

## Safety and legal constraints

No game binary, key, firmware, SDK, extracted asset, private execution report,
or proprietary implementation is committed. The implementation is independently
written against parsed target metadata, public ABI documentation, and the
observed local call state. Public declarations were used as interface evidence
only; they were not copied into runtime implementation code.

## Next genuine blocker

The next genuine boundary is the recognized but unimplemented
`__nnmusl_init_dso` import. The actionable missing evidence is its formal ABI,
return semantics, side effects, and provider identity. The next milestone
should establish those facts or implement the minimum provider/module metadata
needed to represent a proven cross-module dependency. It should not begin
loading familiar module names without evidence.
