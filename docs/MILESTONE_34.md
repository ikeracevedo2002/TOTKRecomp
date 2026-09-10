# Milestone 34: Execution Observability and Resource Convergence

## Status and lineage

Milestone 34 separates logical execution-event accounting from retained
diagnostic history. It removes the ordinary event-vector capacity as a guest
execution guard, retains a finite deterministic history, preserves an explicit
`--max-events` compatibility guard, and continues the real workflow to the
next capability boundary.

- Required base SHA: `b80fd7133d18816cb66d9b0ef98dabdfa50291a8`.
- Stacked branch: `milestone-34-execution-observability-convergence`.
- Parent branch: `milestone-33-semantic-refinement-transaction-resource`.
- Parent milestone PR: `#38`.
- The M34 PR target is the M33 parent branch above.
- The branch was created directly from the required SHA. No rebase onto
  `main`, merge from `main`, squash of M33, or history rewrite was used.
- Before editing, `git status` showed the three unrelated machine-local
  changes: modified `.gitignore`, untracked `.DS_Store`, and untracked
  `src/.DS_Store`. They remain untouched, unstaged, and uncommitted.

No Nintendo binaries, private reports/configuration, keys, firmware, absolute
local paths, host pointers, timestamps, PIDs, or machine-local metadata are
committed. The ignored local reports used below remain outside version control.

## Baseline and exact M33 reproduction

Before production edits, the required base SHA was verified and the complete
standard suite passed:

```text
348/348 CTest tests
36,636 assertions in 348 test cases
```

The requested M29--M33 regression filter also passed:

```text
51/51 CTest tests
```

The exact ignored four-module workflow was then run without an event override,
larger budget, target address, forced PC, alternate seed, or workaround:

```bash
./build/run-entry --local-config config/local.m17.local.json --entry dt-init \
  --report build/reports/m34-m33-reproduction.json
```

The reproduction was byte-identical to the M33 evidence:

| Measurement | M33 reproduction |
| --- | ---: |
| schema / report size | `19` / `41,233,243` bytes |
| SHA-256 | `4b9ef350e2c056d3af2dfb3a6cdd2e95249852508f72575be83ec39ffaf5ddf0` |
| stop / events | `event_limit_exceeded` / `4096/4096` |
| execution slices | `2048` |
| guest instructions / blocks / IR operations | `18,647` / `3,765` / `110,727` |
| direct / indirect calls | `468` / `523` |
| returns / function transfers | `991` / `65` |
| maximum call depth / runtime fallbacks | `3` / `0` |
| refinement attempts / productive / stagnant rounds | `529` / `528` / `1` |
| candidate records / first assessments / reassessments | `528` / `528` / `0` |
| promotions / map generation / pending | `528` / `528` / `0` |
| aggregate functions analyzed / reanalyzed / reused | `1,124/200,000` / `6/100,000` / `147,958` |
| aggregate instructions / blocks / edges | `28,498/8,000,000` / `5,884/2,000,000` / `8,306/4,000,000` |
| aggregate bytes / boundary passes / invalidated records | `113,992/268,435,456` / `528/2,048` / `3/100,000` |
| aggregate transactions / ordinary transaction limit | `527` / `null` |
| controlled stacks created / reclaimed / live | `529` / `529` / `0` |
| peak / final live mapped bytes | `96,370,944` / `95,322,368` |
| peak live stacks | `1` |

The reproduction was completed before production edits were evaluated; its
report was not overwritten afterward.

## Event lifecycle and forensic audit

An `ExecutionEvent` is a diagnostic snapshot associated with session setup, a
guest control-flow boundary, a runtime-import boundary, or session termination.
It is not an architectural state transition. `record_event` is the only
production event sink in `src/switchrecomp/execution/session.cpp`; the
interpreter and refinement engine do not depend on retaining the event vector
for correctness. The interpreter's `InterpreterFrame` remains the source of
resumable execution state.

Every event kind and production status is accounted for here:

| Event kind | Production emission site and meaning |
| --- | --- |
| `SessionStart` | `ExecutionSession::run`, once per generation |
| `EntrySelected` | `ExecutionSession::run`, selected entry evidence |
| `StackMapped` | `map_stack`, controlled-stack mapping evidence |
| `FunctionEnter` | `enter_function` for the initial/call entry and successful non-call transfer entry |
| `DirectCall` / `IndirectCall` | `dispatch_call`, one call-boundary diagnostic before dispatch |
| `FunctionTransfer` | `dispatch_transfer`, one non-call transfer-boundary diagnostic, including a terminal attempt |
| `Return` | `run`, one guest return boundary |
| `FunctionResume` | `run` after a guest return, or a handled tail runtime transfer |
| `ImportBoundary` | `stop`, for an unresolved import stop |
| `RuntimeImportResolved` | `dispatch_runtime_import`, handled registered runtime import |
| `RuntimeImportEnter` | `dispatch_runtime_import`, immediately before the handler |
| `RuntimeImportArgumentSummary` | `dispatch_runtime_import`, observed ABI argument summary |
| `RuntimeStateRegistration` | Declared event kind; no production emission site, so its count is zero |
| `RuntimeImportReturn` | `dispatch_runtime_import`, after a handled handler outcome |
| `RuntimeImportBoundary` | `stop`, for a runtime import contract/outcome stop |
| `IndirectBoundary` | `stop`, for an indirect-target/certification stop |
| `MemoryFault` | `stop`, for a guest-memory resource or access stop |
| `UnsupportedBoundary` | `stop`, for unsupported instruction/semantic and other typed stops |
| `SessionStop` | `stop`, second terminal diagnostic when the first stop event was retained |

The old implementation used the retained vector for both storage and
permission to execute:

```cpp
if (result.events.size() >= options_.budgets.max_events) {
    result.stop_reason = ExecutionStopReason::EventLimitExceeded;
    running_ = false;
    return false;
}
event.sequence = result.events.size();
result.events.push_back(...);
```

Thus M33's ordinary `max_events = 4096` was a storage capacity disguised as a
semantic execution resource. Once the 4096th record was retained, the next
diagnostic emission ended guest execution. The full event history is not used
to execute instructions, validate architectural state, certify indirect
targets, publish refinement maps, restore interpreter frames, or reclaim
stacks. Retention therefore cannot be allowed to control those operations.

## Event-generation termination proof

Let `B` be charged guest blocks, `C` call-boundary attempts, `X` function-
transfer boundary attempts, `R` return boundaries, and `I` runtime-import
boundaries. For the production paths:

- setup contributes exactly three events (`SessionStart`, `EntrySelected`, and
  `StackMapped`), and the initial entry contributes one `FunctionEnter`;
- each call attempt contributes at most one call event and each successful call
  contributes at most one `FunctionEnter`;
- each transfer attempt contributes one `FunctionTransfer` and a successful
  transfer contributes one `FunctionEnter`;
- each return contributes one `Return` and at most one `FunctionResume`; a
  handled tail runtime transfer can contribute one additional resume within
  its runtime boundary;
- a handled runtime boundary intentionally contributes four events:
  `RuntimeImportResolved`, `RuntimeImportEnter`,
  `RuntimeImportArgumentSummary`, and `RuntimeImportReturn`;
- a typed stop contributes at most `UnsupportedBoundary`/another boundary
  event plus `SessionStop`, so at most two terminal events.

The resulting conservative relationship is:

```text
total generated events
  <= 3 + 1 + C + (1 + C + X) + X + R + (R + I) + 4I + 2
  <= 6 + 2C + 2X + 2R + 5I
```

`E <= 1 + C + X` is the successful-entry bound used in the first line. Each
`C`, `X`, `R`, or `I` is returned by an interpreter boundary step that charges
guest-block progress; a handler's four event emissions are a finite
multiplication of one runtime boundary, not a loop in `record_event`. Hence a
coarser bound `total events <= 6 + 11B` follows for ordinary execution. `B`
is already controlled by the finite `max_guest_blocks` resource, while call
depth, successful non-call transitions, guest memory, refinement work, and
explicit IR limits provide additional semantic limits. If an extreme
combination of finite values would make the derived count unrepresentable,
checked event arithmetic fails closed with a typed arithmetic error rather
than wrapping. No independent arbitrary ordinary event constant is required.

The audit found no event path that can repeatedly emit without a guest
boundary, refinement progress, or a finite session transition. Runtime handler
execution does not recursively emit execution events. This is why the ordinary
event ceiling can be removed while preserving termination and safety.

## M34 architecture

`ExecutionBudgets::max_events` is now `std::optional<std::size_t>` and is absent
by default. `event_history_limit` is a positive diagnostic-only capacity with
default `4096`. `ExecutionSessionResult` carries an `ExecutionEventResource`
with:

```text
model
total_generated
retained
omitted
history_truncated
history_policy
retention_limit
execution_limit
execution_limit_provenance
by_kind
first_sequence_retained
last_sequence_retained
reconciles
terminal_attempt_sequence/kind/event
```

The retention policy is `first_prefix_plus_recent_window`. For a limit `L`,
the first `floor(L/2)` events are immutable prefix evidence. Before truncation,
all events are retained. On the first event beyond `L`, the suffix after the
prefix is discarded and the new event starts the recent window. Subsequent
events evict only the oldest recent event before appending. The retained vector
is always at most `L`, remains in logical chronological order, and contains the
first prefix followed by the recent window. `omitted = total_generated -
retained` is checked after every emission. The report also serializes all event
kinds in enum order, including zero counts.

Logical sequence numbers are assigned from the checked total count, beginning
at zero. They are never derived from the retained-vector index, so a retained
sequence can jump over omitted history. `checked_next_execution_event_sequence`
and all new per-kind/omitted arithmetic fail closed with
`ErrorCode::ArithmeticOverflow`; no unsigned wraparound is permitted.

`--max-events N` remains an explicit positive finite compatibility/debug
execution guard. The same value can be supplied in local JSON as
`execution.budgets.max_events` (or the equivalent `execution` budget object),
and CLI configuration overrides local configuration. Library callers receive
`explicit_library_api` provenance when they engage the option without a more
specific provenance. Ordinary absence is reported as JSON `null` and
`not_configured`. The guard is exact and exclusive: when `total_generated == N`
and another event is requested, that event is not counted as emitted, execution
stops with `event_limit_exceeded`, and a structured terminal attempt records its
sequence and kind. The diagnostic includes `consumed`, `limit`, and
`attempted_sequence`. `SIZE_MAX` is not an unlimited sentinel.

Terminal CPU state, stop reason, stop PC, target/provenance, instruction
diagnostics, runtime diagnostics, refinement accounting, transition accounting,
and stack/memory accounting remain first-class report data and do not depend on
the final retained event.

The report schema is bumped once from `19` to `20`. The top-level
`event_resource` object includes all required resource fields, while the
existing `budgets` object preserves the configured optional limit and its
provenance. Event records additionally carry deterministic sampled guest
instruction, block, and IR counts so progress across a truncated history is
auditable.

## Synthetic validation

`tests/milestone34_execution_observability_tests.cpp` is registered in the
normal test target. Its eight production-path cases passed as `94 assertions
in 8 test cases`:

1. A legitimate looping synthetic guest crosses 4096 logical events under
   ordinary defaults, continues changing `x0`, and stops at the independent
   guest-block resource with a bounded history.
2. Identical synthetic execution with history limits `8` and `128` produces
   identical architectural state and semantic counters while retaining
   different bounded evidence.
3. The limit-8 policy retains sequences `0..3` and the final four logical
   sequences in deterministic order, with exact omission reconciliation.
4. An explicit library `max_events=3` stops before the fourth emission,
   reports consumed `3`, limit `3`, and terminal attempt sequence `3`.
5. A synthetic runtime/import boundary emits and counts each of the four
   intentional runtime events; `RuntimeStateRegistration` remains zero.
6. A synthetic direct call and return path reconciles `FunctionEnter`,
   `DirectCall`, `Return`, and `FunctionResume` with the total count.
7. The checked sequence seam rejects the maximum representable next count with
   `ArithmeticOverflow` and accepts the preceding count exactly.
8. Identical synthetic sessions produce byte-identical schema-20 JSON.

The post-change complete suite passed `356/356` CTest tests and `36,730`
assertions. The M29--M33
filter passed `51/51`, and the dedicated M34 filter passed `8/8`.

## Former frontier crossing and convergence

The ordinary real workflow was rerun with the unchanged local configuration and
no event override. Logical event `4096` was emitted normally:

```text
sequence 4096: Return, main, PC 0x00000072001356c0
guest instructions 18646, guest blocks 3766, IR operations 110747
```

The first successor event was sequence `4097`, `FunctionResume`, at the
controlled stack continuation with the same counters. The first subsequent
instruction-bearing boundary was sequence `4098`:

```text
kind: IndirectCall
module: main
PC: 0x0000007200000148
opcode: 0xd63f0100 (blr x8)
target: main:0x00000072001356d0
guest instructions 18650, guest blocks 3768, IR operations 110779
call depth: 0
```

This is normal execution progress after the former event ceiling. No event was
skipped, forced, seeded, or hidden.

The convergence audit found no subsequent historical/redundant event,
retention, trace, slice, scheduler, or bookkeeping ceiling. The final run's
`execution_slices=2084` and one resumable yield are scheduler accounting, not a
stop; `max_function_transitions` is only `65/1000`; the transaction resource is
ordinary-unlimited and consumes `546/547` boundary-finalization passes; no
ordinary IR hard limit is configured. Therefore no second arbitrary counter was
changed in M34.

## Final real execution

Two identical ordinary runs used:

```bash
./build/run-entry --local-config config/local.m17.local.json --entry dt-init \
  --report build/reports/m34-final-a.json
./build/run-entry --local-config config/local.m17.local.json --entry dt-init \
  --report build/reports/m34-final-b.json
```

Both stopped at the same materially different capability frontier:

| Measurement | Final result |
| --- | ---: |
| schema / report size | `20` / `42,431,771` bytes |
| SHA-256 | `041906e67b377e8b8346f843a7a803b47f2b2ea4aeac775ea18bd4609b17f344` |
| stop | `unsupported_instruction` |
| module / current function / stop PC | `main` / `0x00000072001485f0` / `0x0000007200148610` |
| instruction ID / opcode / decoded instruction | `fp_simd` / `0x0f03f5e1` / `fmov v1.2s, #0.96875000` |
| guest instructions / blocks / IR operations | `18,897` / `3,837` / `112,428` |
| execution slices / yields / resumes / mid-block resumes | `2,084` / `1` / `1` / `1` |
| direct / indirect calls | `467` / `542` |
| returns / function transfers / maximum call depth | `1,008` / `65` / `3` |
| total logical / retained / omitted events | `4,170` / `2,122` / `2,048` |
| event history / execution limit | truncated / `null` (`not_configured`) |
| runtime fallbacks | `0` |

The final event resource has `first_sequence_retained=0`,
`last_sequence_retained=4169`, `retention_limit=4096`, policy
`first_prefix_plus_recent_window`, all per-kind counts reconciled, and
`reconciles=true`. It is not a hidden execution limit.

The next instruction is a scalar-immediate FP/SIMD form with no source
registers; its destination operand is `v1.2s` and its source operand is the
encoded immediate `#0.96875000`. The path is the normal `dt-init` controlled
entry through the main-module startup/refinement dispatch into function
`main:0x00000072001485f0`. There were `251` additional guest instructions
after the sequence-4096 snapshot (`18897 - 18646`). M34 does not implement this
instruction.

### Refinement and aggregate resources

The final nested refinement report records:

```text
attempts: 548
productive rounds: 547
stagnant rounds: 1
candidate records: 547
first assessments: 547
generation reassessments: 0
promotions: 547
map generation: 547
pending candidates: 0
aggregate functions analyzed: 1162 / 200000
aggregate functions reanalyzed: 6 / 100000
aggregate functions reused: 158503
aggregate instructions: 35274 / 8000000
aggregate blocks: 7040 / 2000000
aggregate edges: 10508 / 4000000
aggregate bytes: 141096 / 268435456
boundary finalization passes: 547 / 2048
invalidated records: 3 / 100000
transactions: 546; ordinary transaction limit: null
```

There were `353,227` observations, `1,590` unique observations, `547` unique
candidates, `352,680` existing-trusted hits, and `351,637` duplicate-coalesced
observations. Runtime fallback and failed-refinement counts remained zero.

### Memory and report scaling

The final report's memory accounting was:

```text
controlled stacks created/reclaimed/live: 548/548/0
cumulative controlled-stack bytes: 574,619,648
cumulative mapped bytes: 669,942,016
peak/final live mapped bytes: 96,370,944 / 95,322,368
peak live controlled-stack bytes: 1,048,576
peak live stacks: 1
```

The event vector retained at most the configured `4096` records and the final
run retained `2122` records because the total stopped at `4170` and the prefix
was preserved. The report grew from the M33 reproduction's `41,233,243` bytes
to `42,431,771` bytes because schema-20 resource data and per-event progress
snapshots were added; it did not grow in proportion to an unbounded event
history. No new dependency or alternate storage library was introduced.

## Adjacent execution-resource classification

| Resource | M34 classification and disposition |
| --- | --- |
| `max_guest_blocks` | Semantic independent progress/safety resource; unchanged |
| `max_call_depth` | Semantic active-call-stack resource; unchanged |
| `max_function_transitions` | Semantic non-call transfer resource established by M31; unchanged |
| `max_ir_operations` | Optional explicit global semantic IR hard limit; M29 resumability preserved; unchanged |
| `slice_ir_operations=4096` | Scheduler quantum only; yields/resumes resumable state and does not terminate globally |
| guest-memory region/total bounds | Genuine semantic resource; unchanged |
| execution slices, yields, resumes | Accounting/scheduler diagnostics; no independent stop ceiling |
| refinement aggregate dimensions | Semantic finite work resources; unchanged |
| refinement transactions | M33 diagnostic projection with optional explicit compatibility ceiling; unchanged |
| transition history (`max_guest_blocks + 1`) | Bounded diagnostic history, separate from transition charging; unchanged |
| logical event count/history | Checked observability accounting plus bounded history; ordinary guard removed, explicit guard retained |

M29 resumable frames, M30 generation-scoped stack ownership, M31 transition
semantics, M32 candidate generation, and M33 transaction semantics remain
unchanged and are covered by the regression suite.

## Determinism, validation, CI, dependencies, and privacy

Determinism verification:

```text
Run A: 42,431,771 bytes; SHA-256 041906e67b377e8b8346f843a7a803b47f2b2ea4aeac775ea18bd4609b17f344
Run B: 42,431,771 bytes; SHA-256 041906e67b377e8b8346f843a7a803b47f2b2ea4aeac775ea18bd4609b17f344
cmp: identical (exit 0)
```

Local validation completed:

- standard CTest: `356/356`, `36,730 assertions`;
- M34 focused tests: `8/8`, `94 assertions`;
- M29/M30/M31/M32/M33 filter: `51/51`;
- GCC/LLVM 18/ASan-UBSan/TSan/MSVC CI: not yet run for this branch;
- no sanitizer suppression was added;
- new dependency count: `0`.

The local host is macOS with AppleClang; GNU GCC, LLVM 18, MSVC, and the
sanitizer toolchains requested for the cross-platform matrix are not all
available locally. They must be reported only from the corresponding GitHub CI
jobs after this branch is pushed; no remote result is claimed here.

The tests use only project-generated synthetic bytes. The real four-module
configuration, reports, and any executable-derived evidence remain ignored and
outside the commit. No private paths, binary contents, keys, firmware, build
IDs, timestamps, PIDs, or host addresses are production constants.

## Milestone 35 recommendation

The only M35 recommendation is the final materially different frontier:

```text
main:0x0000007200148610
opcode 0x0f03f5e1
fp_simd: fmov v1.2s, #0.96875000
destination v1.2s; source immediate #0.96875000; no source registers
```

Do not increase another counter as the M35 objective, and do not implement this
instruction as part of M34.
