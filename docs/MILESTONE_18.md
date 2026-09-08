# Milestone 18 — Evidence-backed indirect guest target discovery

## Status

**Complete.** M18 resolves the exact M17 `unknown_guest_function` frontier by
validating and promoting the observed target through a generic immutable
function-map refinement path. The real controlled run crosses that boundary
and stops at a new, typed unsupported-instruction frontier. It does not yet
reach the real `UMULH` instruction.

## Git

* branch: `milestone-18-indirect-target-discovery`
* exact base SHA: `8ab6d1201be8eadcdd84352577780fbf5e763c23`
* exact final head SHA (implementation before documentation handoff):
  `e162bfc`
* PR: to be filled after push
* commits: `5b9cd9e` (analysis/data model/refinement), `e162bfc`
  (execution integration and tests), plus the evidence/documentation handoff

The branch was fetched, verified at the required M17 commit, checked out from
that commit, and created before source changes. The pre-change complete suite
was run before modifying the repository.

## Baseline

* expected M17 test count: `211/211`
* observed pre-change baseline: `211/211`
* final local suite: `227/227`

The existing M17 provenance was preserved: the four-module set is locally
asserted complete (`declared_complete`, basis `explicit_local_assertion`),
coherent only as `partially_verified`, and ordered deterministically as
`rtld -> main -> subsdk0 -> sdk`. No manifest verification was claimed.

## Previous frontier

The exact previous frontier was:

```text
source function:  sdk:0x7204c4a3d0
source PC:        sdk:0x7204c4a3dc
instruction:     BR / indirect transfer
register:        x17
target:          sdk:0x72047b6870
provenance:      guest_load:0x7205390468
call depth:      2
```

M17 correctly stopped because the target was aligned executable guest code but
was not an exact entry in the focused finalized process function map.

## Static investigation

The investigation used the exact local four-module executable set after the
same identity and base preflight used by M17. The target and pointer slot are
described by neutral guest-address terminology; no proprietary bytes or raw
private reports are committed.

### Target `sdk:0x72047b6870`

* owner: unique process-module owner `sdk`
* module base: `0x720468a000`
* module offset: `0x12c870`
* executable mapping: SDK text, `r-x`, beginning `0x720468a000`, size
  `0x5e02e0`; the target is 4-byte aligned and fully mapped for decoding
* dynamic symbol: index `9455`, name
  `_ZN2nn2os40MemoryAllocatorForThreadLocalInitializedEv`, defined `GLOBAL
  FUNC DEFAULT`, value `0x12c870`, size `20`
* first decoded instruction: `adrp x8, #0x72053af000`
* bounded CFG: one basic block, five decoded instructions, zero outgoing
  edges, no direct calls, and no unresolved indirect flow
* final decoded instruction: `ret` at `0x72047b6880`
* precise ownership before refinement: no existing owner; no existing
  secondary entry; no overlap with a precise `owned_code_ranges` record
* display-envelope result: no envelope approximation was used to establish
  ownership
* direct-call evidence: no existing finalized direct-call reference to this
  address was found in the focused map
* exception/unwind evidence: the current project parses MOD0 exception-range
  metadata but does not use an exception/unwind record to identify this entry;
  no such independent target evidence was available
* init/fini and dynamic-array evidence: the target was not identified as a
  `DT_INIT`, `DT_FINI`, init-array, or fini-array entry

The address is therefore classified as a distinct new function entry, not an
alias, local label, or target inside an established function. Its decisive
evidence is the defined dynamic `FUNC` symbol plus the relocation evidence
below. The runtime observation was the trigger for refinement in the focused
experiment, not proof by itself.

### Pointer slot `0x7205390468`

* owner: unique SDK data mapping
* mapping: `rw-` data region beginning `0x720533d000`, size `0x68e90`
* module offset: `0xd06468`; offset within the data mapping: `0x53468`
* region kind established by the project: data; this is retained as a guest
  pointer slot, not labeled as a vtable, callback table, object, or GOT without
  stronger evidence
* relocation: `R_AARCH64_JUMP_SLOT`, source `JMPREL`, relocation symbol index
  `9455`, zero addend, relocation offset `0xd06468`
* relocation application: the project applied the relocation through checked
  guest memory and the loaded value was observed as the executable guest
  address `0x72047b6870`
* nearby storage: the slot is part of the parsed relocation-backed data
  region; no table-level semantic label was inferred from adjacency alone

No static scan of arbitrary readable memory was added. The generic process
loader now records bounded relocation-backed executable `FUNC` seeds, while
the focused M17-compatible run retains its deliberate startup/provider seed
closure and lets the refinement assessment combine the static metadata with
the runtime observation.

## Architecture

M18 separates five phases:

1. `ExecutionSession` emits an `ObservedIndirectTarget` containing source
   module/function/PC, `BR`/`BLR`/`RET` control-flow kind, register, guest
   target, target module, pointer provenance, guest load address, and count.
2. `assess_indirect_target` validates checked arithmetic, nonzero/alignment,
   mapping, execute permission, unique module ownership, exact/secondary
   entries, precise owned ranges, display-envelope gaps, bounded CFG results,
   overlaps, static symbols/relocations/direct calls, and limits.
3. The analysis layer—not the execution session—decides whether promotion is
   eligible. Runtime observation alone can be rejected by policy.
4. `refine_function_map` and `refine_process_function_map` rebuild and return
   new frozen maps. Existing finalized state is never modified in place.
5. The controlled runner performs a deterministic execution-frontier loop,
   sorting candidates by module/address/source identity and limiting refinement
   to eight passes and 32 new candidates, in addition to CFG/function/byte
   budgets. A failed or bounded candidate remains a typed failure and is not
   executed.

The new discovery source is `ObservedIndirectTarget`. It is only attached to a
new `FunctionSeed` after validation and successful map reconstruction. Existing
canonical and secondary entries remain existing records. `BR` remains a tail
function transfer, `BLR` remains a call that writes the architectural link
register, and `RET` remains a return; no host callback, host pointer, synthetic
return address, or guest-address reinterpretation is used.

## Trust model

An executable address is necessary but not sufficient to enter the trusted
function namespace. The trusted namespace is owned by the analysis/refinement
layer. A new entry requires a unique executable module owner, valid bounded CFG
structure, no precise ownership conflict, and an evidence policy permitting
promotion. Static `STT_FUNC` and relocation-backed function-pointer evidence
can raise confidence; runtime observation remains separately represented. A
candidate inside precise ownership, an overlap, ambiguity, malformed CFG, or
exhausted budget fails closed. Convex `range_begin/range_end` display envelopes
never replace precise `owned_code_ranges`.

## Exact candidate decision

`0x72047b6870` was promoted as:

* classification: `trusted_new_entry`
* ownership: `new_entry`
* canonical entry: `sdk:0x72047b6870`
* discovery source: `observed_indirect_target` after immutable refinement
* confidence: `confirmed`
* static evidence: defined dynamic `FUNC` symbol and exact
  `R_AARCH64_JUMP_SLOT` relocation-backed guest pointer slot
* CFG decision: `validated`, one block, five instructions, no unresolved flow
* conflict result: no precise ownership or candidate overlap

This is a distinct function record. It is not an alias of an existing record.

## Real execution

The exact controlled run began at the same metadata-selected entry
`main:0x7200000030` (`DT_INIT` candidate, not a verified Nintendo process
entry). Exact module identities and bases were reverified before execution:

| module | SHA-256 | analysis base |
| --- | --- | --- |
| `rtld` | `28317605926caa40a5cbee3e3cb290c65783b4d9f8c3697b43ca41ab2866a44d` | `0x7204676000` |
| `main` | `faf81f8a609ff61a6322d04948132b5929af9f6e2398e0e0ebbe819b8ba3eaab` | `0x7200000000` |
| `subsdk0` | `9d04c98997977e99c12a48a2dbfd3064b8da96b5152e0119064957a43f6c3f99` | `0x7205473000` |
| `sdk` | `91573d4444d0dcd07080db78fece2b098b76080d77cdb2e1d301a2a41d7c4eea` | `0x720468a000` |

The provider checks were reproduced: `__nnmusl_init_dso` resolved to
`sdk:0x7204717880`; both `main` and `subsdk0` relocation slots read back that
guest address; runtime fallback was invoked zero times; and guest provider code
was entered. The previous frontier `sdk:0x7204c4a3dc -> sdk:0x72047b6870`
was crossed. The promoted target was entered at call depth `2`, returned to
the provider continuation, and execution resumed at call depth `1`.

## UMULH

* real PC: `sdk:0x7204717c30`
* real execution: **not reached**
* `x10` input: not established
* old `x9` input: not established
* resulting `x9`: not established
* independent expected result: not applicable
* match: not applicable
* next guest PC: not established

The observation target set still contains the real `UMULH` site, but the
executed-observation list is empty. Decoding, lifting, and synthetic M16/M17
coverage are not substituted for real execution.

## Final execution boundary

The first new boundary after the promoted target was entered and returned is:

* typed stop: `unsupported_instruction`
* module: `sdk`
* PC: `0x72047182ac`
* source PC if distinct: same diagnostic PC; no indirect target is associated
* target: none
* call depth: `1`
* diagnostic instruction: `mov w0, #2` (`MOVZ`, opcode `0x52800040`)
* explanation: the provider continuation reached an instruction that the
  current Semantic IR execution path does not lift; the report records the
  decoded instruction and fails closed

The promoted target itself has a complete bounded CFG ending in `ret`; the
stop is later in the provider continuation, not a rejection of the promoted
entry.

## Bootstrap evidence

* required: not established
* exact evidence: the old unknown-function frontier was resolved and execution
  advanced to a new unsupported instruction; no causal TLS, loader DSO,
  startup-stack, MOD0-runtime-object, or other bootstrap failure was observed
* model added: no
* modeled state/provenance: none

## Determinism

* report schema: `6` (M17 schema `5` was incremented for the new serialized
  `indirect_target_discovery` and diagnostic-instruction fields)
* SHA-256 #1: `648f8e481875fd4d466074bc72013560f881e0df6c292d8dbf196779040bd40d`
* SHA-256 #2: `648f8e481875fd4d466074bc72013560f881e0df6c292d8dbf196779040bd40d`
* identical: yes

The reports contain no timestamps, PIDs, host pointers, temporary paths,
usernames, or proprietary bytes.

## Public research and evidence classes

* public specification: Arm A64 documentation distinguishes register-indirect
  `BR`, link-producing `BLR`, and return `RET` control flow. See [Arm A64
  instruction documentation](https://developer.arm.com/documentation/ddi0602/2026-06/Base-Instructions/BR--Branch-to-Register-).
* public specification: [Arm AAELF64](https://github.com/ARM-software/abi-aa/blob/main/aaelf64/aaelf64.rst)
  documents `STT_FUNC` and AArch64 relocation semantics used as context for
  the static evidence classification.
* public implementation evidence: Atmosphère's [loader documentation](https://github.com/Atmosphere-NX/Atmosphere/blob/master/docs/components/modules/loader.md)
  supports the conventional ExeFS load-order reference only; it is not treated
  as evidence of Nintendo-private bootstrap behavior.
* project observation: checked NSO/ELF metadata, mappings, relocations,
  function ownership, CFGs, and execution reports produced by TOTKRecomp.
* private executable observation: exact local module identities, target,
  pointer slot, and runtime path recorded above; private inputs remain outside
  the repository.
* hypothesis: none was promoted to implementation for TLS, loader bootstrap,
  DSO state, or other missing runtime behavior.

## Validation

* final test count: `227/227` local CTest tests
* M18-specific test cases/assertions: `16` cases and `90` assertion macros
* ASan/UBSan: `227/227` passed in `build-asan`
* TSan: `227/227` passed in `build-tsan`
* local compiler: AppleClang 17 via `/usr/bin/c++`; no local GCC claim
* GCC CI: pending until the pushed branch workflow completes
* LLVM 18 CI: pending until the pushed branch workflow completes
* MSVC CI: pending until the pushed branch workflow completes

## Dependencies

Exact number of new dependencies: `0`.

## Privacy

* proprietary files committed: no
* private reports committed: no
* private absolute paths committed: no

## Exact next blocker

The real controlled path now reaches and returns from `sdk:0x72047b6870`, then
stops at `sdk:0x72047182ac` on unsupported `mov w0, #2`, before the real
`sdk:0x7204717c30` `UMULH`.
