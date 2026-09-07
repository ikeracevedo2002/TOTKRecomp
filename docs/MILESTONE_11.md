# Milestone 11 — Controlled entry-path execution

Milestone 11 executes one bounded main-module initialization candidate through
the existing prepared-NSO pipeline. Its purpose is auditability: supported
Semantic IR can cross guest function boundaries, resume callers, and stop at
the first genuine import, indirect-flow, runtime, memory, or semantic boundary.

It is not game boot, full Switch process startup, Horizon emulation, rtld
execution, multi-module loading, or evidence that `main()` was reached.

## Pipeline and architecture

`run-entry` reuses NSO parsing/materialization, SHA-256 validation, GuestMemory,
MOD0/dynamic metadata, relocation application, unresolved relocation retention,
function discovery, precise ownership, CFG finalization, and the existing
lifter. The execution layer does not rediscover function boundaries.

```text
prepared main NSO
  → resolved relocations + explicit unresolved bindings
  → finalized precise FunctionMap
  → on-demand CFG → Semantic IR → verifier → lift cache
  → interpreter boundary
  → ExecutionSession guest call stack
  → exact guest dispatch / return / FunctionTransfer
  → typed runtime boundary and deterministic trace
```

The interpreter executes one function at a time. `InterpreterFrame` preserves
the function's block cursor, SSA values, vector high halves, and narrow value
provenance across suspension. `ExecutionSession` owns the guest stack of
suspended caller frames; it does not use recursive C++ calls for guest calls.

`BL` and `BLR` retain the lifter's single authoritative link-register semantics:
X30 receives the checked guest address `call_site + 4`. A call stores an
explicit continuation block and guest PC, pushes a session frame, and dispatches
only an exact trusted function entry. `RET` validates the actual guest target
against the suspended caller's expected continuation before resuming it.

An unconditional `B` into a strong known function entry is represented as
`FunctionTransfer`. It replaces the current function without pushing a return
frame, does not modify X30, does not increase call depth, and preserves the
inherited return contract. `BR` to an exact known entry has the same transfer
shape; unknown indirect targets are never guessed.

Execution eligibility is separate from whole-module translation status. A
function with a valid CFG and liftable instructions ending in an intentional
indirect boundary is runnable to that boundary. Ownership conflicts, invalid
CFGs, verifier failures, unsupported instructions, and unknown entries are
explicit decisions, not arbitrary owner selection or host function-pointer
casts.

## Controlled launch context

The selected target is semantic. `--entry dt-init` finds `EntryPointEvidence`
created from parsed `DT_INIT`; it does not contain a TOTK address. `dt-fini`,
`text-start`, and an explicit `--entry-address` analyst override are also
available. `--entry process` fails when no verified process entry exists; it
never falls back to `.text + 0`. The analysis-selected module base is always
reported with `guest_base_verified: false` for the local prepared main input.

Each session creates a fresh checked GuestMemory stack mapping. The default is a
synthetic 1 MiB read/write, non-executable, page-aligned mapping placed after a
deterministic guard gap above mapped module ranges. SP starts at its aligned
upper end and the stack grows downward. The LR sentinel is deterministic,
synthetic, unmapped, and never dispatched: a top-level `Return` stops as
`EntryReturned`.

X0–X30, NZCV, FPCR/FPSR, vector registers, and TPIDR_EL0/TPIDRRO_EL0 begin in
documented deterministic synthetic state. This is not Horizon thread/TLS
initialization. Guest stores are real checked memory side effects and faults
become structured `MemoryFault` stops.

## Boundaries, imports, and budgets

The session reports typed stop reasons including `EntryReturned`,
`UnresolvedImport`, `UnknownGuestFunction`, `InvalidIndirectTarget`,
`UnresolvedIndirectControlFlow`, `UnsupportedInstruction`,
`FunctionOwnershipConflict`, `MemoryFault`, `GuestTrap`,
`ReturnTargetMismatch`, and global operation, transition, depth, block, and
event limits. Expected guest stops return a valid session result; malformed
input, broken invariants, and failed analysis remain `Result` failures.

An execution-side import index maps unresolved relocation target addresses to
symbol name, symbol index, relocation type/index, addend, and binding metadata.
The interpreter carries only narrow provenance: constants, register writes,
and guest loads (with simple address adjustments). A zero indirect target is
classified as an import only when that provenance identifies an unresolved
relocation slot. Otherwise it remains an invalid or unknown target.

The semantic trace is bounded by `max_events` and contains deterministic event
records for session start, entry selection, stack mapping, function entry,
calls, transfers, returns, resumes, boundaries, and session stop. It contains
no timestamps, host pointers, thread IDs, random identifiers, or private input
paths. The JSON report also includes module identity, entry provenance,
relocation and precise-ownership summaries, synthetic launch state, execution
counters, register summary, and a bounded guest call-stack snapshot.

## Synthetic validation

Public M11 tests use only artificial guest addresses and synthetic AArch64
bytes. They cover simple return, direct and nested calls, BL/BLR link behavior,
caller continuation, recursion depth, global operation limits, exact known and
unknown indirect calls/branches, B versus BL, tail transfer, return mismatch,
stack permissions and faults, unresolved-import provenance, zero-without-import
evidence, event/report determinism, and entry provenance/error handling.

## Real local validation

A legally supplied local prepared main NSO may be validated with:

```bash
build/run-entry --module-name main --module-base 0x7100000000 \
  --entry dt-init --stack-size 0x100000 \
  --analysis-max-functions 5000 --analysis-max-instructions 200000 \
  --analysis-max-blocks 50000 --analysis-max-edges 100000 \
  --analysis-max-seeds 10000 --analysis-max-bytes 16777216 \
  --max-ir-operations 100000 --max-function-transitions 1000 \
  --max-call-depth 128 --max-events 4096 \
  --report build/reports/m11-dt-init.json \
  local/totk/prepared-main.nso
```

The report is a private local artifact and must not be committed. For the
validated prepared input, the observed path entered `DT_INIT`, executed its
direct guest call to `0x7102aa4210`, reached `0x7102aa421c` (`BR X17`), and
stopped at an unresolved import proven by relocation provenance. The observed
import identity is retained in the local report rather than implemented as a
host binding. Repeating the run from fresh module state produces byte-identical
JSON.

This local test does not represent full Switch process startup and does not
model rtld, Horizon, filesystem, graphics, audio, input, or a game loop.

## Deferred scope

The next milestone is chosen from the measured M11 boundary. Deferred work
includes rtld-led startup, multi-module process loading, Horizon/runtime
services, remaining imports, renderer/filesystem/audio/input integration,
exceptions/unwinding, remaining unsupported AArch64 families, and full game
boot.
