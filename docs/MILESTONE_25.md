# Milestone 25: Resumable M9 Interpreter Parity

## Status

M25 is complete for the supported Milestone 9 IR contract. The resumable
interpreter now executes the same eight M9 operations as the reference
interpreter and the LLVM backend. The real bounded run crosses the M24 M9
execution frontier and stops at a later, truthful indirect-refinement limit.

## Starting point and reproduced failure

The exact M24 base was
`f4559f3c9124979562ad873386d9dac8b6d16042` on
`milestone-24-bounded-execution-closure`. The existing suite reproduced the
expected baseline: 266/266 tests passed.

Using the ignored local executable-set configuration and the normal
`run-entry --entry dt-init` workflow, M24 stopped at `main:0x72000ea7f8`
after 1,787 guest instructions with zero runtime fallbacks. Its diagnostic
was `legacy interpreter received a Milestone 9 opcode`.

## Exact former frontier

The CFG identifies the instruction as:

- module: `main`
- guest PC: `0x72000ea7f8`
- normalized instruction ID: `ldxr`
- mnemonic: `ldxr w20, [x10]`
- normalized IR opcode: `ExclusiveLoad`
- width: 4 bytes
- memory order: relaxed
- barrier metadata: none
- system-register metadata: none

The proprietary instruction word is intentionally not recorded here. The
decoder, lifter, M9 verifier, reference interpreter, and LLVM M9 backend all
already supported this operation. The exact real transition was observed in a
normal 22-round refinement run: the shared exclusive-load path executed this
PC with width 4; the session count was 1,813 before and 1,814 after it.

## Architectural root cause

`ExecutionSession` calls `execute_until_boundary()`, whose switch had an
explicit unsupported default for all M9 opcodes. The monolithic M9 interpreter
already dispatched those operations to the project-owned runtime helpers, so
the failure was an execution-path parity gap, not a decoder, lifter, or
analysis-budget defect.

## Implementation

`m9_semantics.cpp` and its header provide one narrow authoritative semantic
dispatcher reused by both interpreter paths. It covers `AtomicLoad`,
`AtomicStore`, `ExclusiveLoad`, `ExclusiveStore`, `ClearExclusive`,
`MemoryBarrier`, `ReadSystemRegister`, and `WriteSystemRegister`. Existing
runtime helpers remain the source of truth for validation, ordering, guest
memory permissions, reservations, barriers, TLS, and structured failures.

The resumable path now associates the active `RuntimeContext` with the current
CPU on every call and preserves the M9 result provenance in `InterpreterFrame`.
`ExecutionSession` now owns a `SharedRuntimeState` for the same runtime ABI
required by atomic and exclusive helpers. The CLI also preserves the last
real `ExecutionSession` result when the outer refinement ceiling is reached,
instead of serializing an all-zero placeholder; no report schema change was
needed.

## Semantics and parity tests

The dedicated M25 tests cover all four supported widths, accepted load/store
orders, little-endian values, permission/misalignment/unmapped failures,
exclusive reservation success/failure and invalidation, DMB/DSB/ISB, TPIDR
register behavior, InterpreterFrame continuation across a call boundary,
typed errors, and truthful guest-load provenance. Differential tests compare
reference and resumable final CPU/TLS/memory/status state. An
`ExecutionSession` regression exercises a synthetic structural LDXR through
the boundary-aware path.

No support was added for pair-exclusive operations, unsupported LSE atomics,
WFE/WFI, or unimplemented Horizon services.

## Real execution result

Two identical final-source reports were produced. M25 recorded:

- guest instructions: 3,832 (M24: 1,787; delta: +2,045)
- maximum call depth: 3
- runtime fallbacks: 0
- refinement rounds: 64
- unique candidates / assessments / promotions / immutable rebuilds: 64 / 64 / 64 / 64
- final map generation: 64, corresponding to the 64 immutable promotions in this report
- final reported frontier: `main:0x7200000148`, target `main:0x72000c6aa0`
- stop reason: `indirect_target_refinement_budget_exceeded`

The default refinement ceiling was not increased in source. The additional
128-round run was exploratory only and also exhausted the existing outer
refinement worklist; no analysis envelope was enlarged to manufacture
execution success.

## Determinism and privacy

The two final reports were byte-identical:

`a2e47ac928f07bc35958880e2e5f1c69b38847c7fa29add90e8e73e327590735`

No proprietary executable, instruction dump, key, firmware, private absolute
path, or local configuration was committed. The ignored local executable set
remains local.

## Validation

The AppleClang suite passed 274/274. ASan/UBSan passed 274/274 with zero
findings. TSan passed 274/274 with zero race reports. Linux/GCC, Linux/LLVM
18, and Windows/MSVC are validated by the repository CI workflow after push;
LLVM 18 and MSVC are not installed on the macOS development host.

The dependency delta is zero. Existing report schema compatibility is
preserved at schema 11.

## Next frontier

The next blocker is not another missing M9 operation. It is the bounded
indirect-refinement worklist at `main:0x7200000148` (`blr x8`), with a guest
load provenance at `main:0x720456ad38`; decode, lift, IR, resumable boundary,
and LLVM support already exist. Milestone 26 should address scalable,
evidence-preserving indirect-target closure and then identify the next genuine
architectural or service frontier without weakening certification or increasing
budgets merely to cross it.
