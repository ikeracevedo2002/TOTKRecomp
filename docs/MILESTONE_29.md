# Milestone 29: Resumable IR Execution

## Status

M29 separates the finite work of one interpreter invocation from the finite
resources that terminate an execution session. Ordinary execution no longer
uses the historical `100,000` IR-operation value as a global stop. Each
interpreter invocation has a finite scheduling slice and persists an exact
Semantic IR cursor. The execution session aggregates that progress under its
existing finite guest-block, transition, event, call-depth, and refinement
resources. An explicitly supplied IR maximum remains an exact global hard
limit.

No PC was injected, no guest instruction was skipped, no candidate was
seeded, no private address was special-cased, and no proprietary artifact is
part of this change.

## Base and M28 reproduction

M29 is stacked directly on M28 commit
`4659163107e19c5db8a05d4673279c14af17dbc1`, on branch
`milestone-29-resumable-ir-execution`. The pre-edit worktree contained only
the preserved local `.gitignore`, `.DS_Store`, and `src/.DS_Store` changes.
The clean-code baseline was `296/296` tests.

Before editing, the ordinary private four-module configuration reproduced the
M28 report exactly:

- size: `37,643,135` bytes;
- SHA-256: `c6cd5e02cb1dbf2e96f7aadc247622993fd5efd77009ab52e22622b3eb4fda18`;
- stop: `ir_operation_limit_exceeded`;
- IR operations: `100,000 / 100,000`;
- current function: `main:0x72000c353c`;
- stopping instruction: `main:0x72000c3550`, `stp x9, x1, [x0]`;
- pending refinement work: `0`.

The unchanged M28 execution counters were guest instructions `17,030`, guest
blocks `3,303`, function transitions `65`, maximum call depth `3`, direct
calls `468`, indirect calls `408`, returns `874`, runtime fallbacks `0`,
execution attempts `414`, productive rounds `413`, stagnant rounds `1`,
unique observations `1,320`, unique candidates `413`, candidate assessments
`413`, terminal resolutions `0`, successful promotions `413`, immutable
generations/map rebuilds `413/413`, module maps rebuilt/reused `413/413`, and
reconsiderations `0`. Aggregate refinement work was `894` newly analyzed
functions, `6` reanalyzed functions, `24,588` instructions, `4,964` blocks,
`7,156` edges, `98,352` bytes, `413` boundary passes, `3` invalidated
records, `412` transactions, and `91,838` reused functions.

## IR-operation budget lifecycle

At M28, `ExecutionBudgets::max_ir_operations` and
`runtime::ExecutionOptions::max_ir_operations` were mandatory scalar fields,
both defaulting to `100,000`. `run-entry` parsed
`--max-ir-operations N` into the session budget, and local configuration had
no execution-budget field. `ExecutionSession::run` subtracted already
executed operations and passed the remainder to
`interpreter::execute_until_boundary`.

The interpreter checked the supplied number immediately before every
Semantic IR instruction and incremented `ExecutionResult::executed_operations`
once before executing that instruction. `SetPc`, constants, register and
flag operations, scalar and vector arithmetic, loads, stores, runtime/helper
operations, and M9 semantic operations each count once. Terminators are not
Semantic IR instructions and do not consume this counter. A vector or FP
operation still counts as one IR operation regardless of its lane count.

The old exhaustion path returned `LimitExceeded` with
`BudgetExhaustion`. The session mapped that pair to
`ExecutionStopReason::IrOperationLimitExceeded`, and the CLI serialized the
stop and consumed/allowed values in the report. Because the old frame stored
only the current block and values, the local observation object and the
implicit range-for cursor were lost at exhaustion. Recalling the interpreter
with that frame therefore restarted the block. It could repeat stores,
register writes, M9 side effects, observation setup, guest-instruction counts,
and block counts; `cpu.pc` and `final_guest_pc` did not identify the next IR
instruction.

The budget check occurs before the next Semantic IR operation, not during an
operation. Therefore a side effect from the last consumed operation can be
visible when the budget expires, while the next operation has not run. A
single guest instruction can consequently be suspended between its lifted IR
operations. Semantic IR operations themselves are atomic interpreter steps;
the frame cursor is advanced only after one returns successfully.

## Former private frontier at IR granularity

The explicit compatibility run with a hard limit of exactly `100,000` retained
the M28 frontier and added the cursor diagnostic:

- current function: `main:0x72000c353c`;
- guest PC and source: `main:0x72000c3550`;
- instruction: `stp x9, x1, [x0]`;
- current block: `0`;
- next IR operation index: `36`;
- consumed IR operation index: `35`;
- operation count: `7` for this scalar STP lift;
- slice count: `1,817`;
- resumable yields/resumes: `1/1`;
- mid-block resumes: `1`;
- max operations in one slice: `4,096`.

The lifter sequence for this STP is `SetPc`, `ReadRegister x0`,
`ReadRegister x9`, first `GuestStore`, `GuestAddressAdd` by eight,
`ReadRegister x1`, and second `GuestStore`. Thus index `35` was the STP
`SetPc`; it had executed and set `cpu.pc` to `0x72000c3550`. Index `36`, the
first `ReadRegister x0`, was next. Neither store had run, so neither guest
memory write had occurred and no register write from this STP had occurred.
The relevant pre-state was `x0 = 0x72045a6158`, `x9 = 0x720459d9b0`, and
`x1 = 0x720415a168`; these values are recorded only as guest-state evidence.
No observation was pending for this ordinary instruction.

This proves that a naive resume based on `cpu.pc` or block ID would replay the
already consumed `SetPc` and any earlier operations, and would be unsafe in a
case where the cursor followed the first store. The frame must carry the
exact operation index and pending observation state.

## Chosen execution model

M29 uses exact Semantic IR cursors. `InterpreterFrame` persists:

- immutable function identity and verification state;
- current basic-block ID and next instruction index;
- SSA values, high halves, value provenance, and register provenance;
- block-counting state and a checked block-entry serial;
- terminal-boundary state;
- pending instruction observation and completed observation identities.

An internal slice is a typed `ExecutionStatus::Yielded` result with
`SliceExhaustion`. It is not a guest boundary, execution attempt, refinement
round, candidate observation, function transition, call, return, or event.
The default quantum is `4,096` Semantic IR operations. A zero quantum is
invalid. Empty looping blocks also yield after their terminator so an
individual invocation cannot spin forever without consuming an IR operation.

The cursor advances after the complete Semantic IR operation succeeds. A
resume therefore starts at the next operation, including when a slice ended
between the two stores of an STP-like sequence. Real terminators alone select
continuation blocks; `resume_at` clears the old cursor and is used when a
call, return, or runtime boundary changes the active frame. Function pointers
remain valid in the session's stable `std::map` lift cache. A new immutable map
generation creates a new execution session, so no old cursor can cross a map
replacement.

Observation pre-state begins at `SetPc` and is persisted in the frame until a
later `SetPc` or real terminal boundary supplies post-state and next PC. A
yield cannot discard or duplicate it. Completed observation identities are
retained in the frame so a resumed slice cannot emit the same evidence twice.
Guest block counts increment only on actual block entry, not on slice resume;
guest instruction counts and PC traces are emitted only by actual `SetPc`
operations. Calls, returns, function transfers, maximum depth, and runtime
boundaries are handled only by real terminators.

## Explicit compatibility and aggregate termination

The ordinary execution budget now has no configured global IR ceiling. The
session passes `nullopt` for the interpreter hard limit and separately passes
the slice quantum. `--max-ir-operations N` remains an explicit CLI hard guard;
library callers can set the optional field and receive explicit-library-API
provenance. The remaining budget is passed on every slice, so an explicit
limit cannot be bypassed. Exhaustion remains typed and exact, with no
operation beyond `N` executing.

The finite ordinary termination argument is:

1. every basic block contains a finite verified IR vector;
2. every non-terminal slice resumes at its persisted cursor, never at the
   beginning of an already partially executed block;
3. each resumed operation advances the cursor, a real terminator changes the
   block/frame, or a zero-IR block yields after changing/attempting its real
   control flow;
4. actual block entries are charged against finite `max_guest_blocks`;
5. real function transitions are charged against finite
   `max_function_transitions`, calls against finite `max_call_depth`, and
   published events against finite `max_events`;
6. outer refinement attempts and map replacements remain bounded by the M27
   candidate, promotion, stagnation, assessment, and aggregate analysis
   ledgers; a new execution generation starts with a new frame;
7. therefore loops, including tight multi-block loops and loops spanning
   calls, eventually hit a named finite resource unless they return or reach
   another typed semantic boundary.

Every new counter increment and derived aggregate addition uses checked
arithmetic. Invalid function, block, cursor, value-storage, or continuation
state fails closed. The checked block-entry serial also rejects overflow.
The scheduling quantum is not used as the termination proof and is not
reported as an execution capacity.

## Synthetic validation

The focused M29 suite contains ten test cases and passed `261` assertions.
It validates:

- identical CPU, memory, PC, observations, guest instruction traces, and
  block/function accounting for large, one-operation, two-operation, and
  awkward slice sizes;
- two side-effecting guest stores separated by IR operations, with a forced
  yield after the first store and exact once-only final memory;
- observation pre/post state, next PC, and one evidence emission across a
  yield inside an observed instruction;
- branch/block cursor accounting and loop re-entry independent of slices;
- direct call/return accounting with one-operation slices;
- exact explicit global hard-limit exhaustion at `4/4` with no bypass;
- ordinary intentional loop termination through the finite guest-block
  resource;
- checked monotonic progress across repeated yields; and
- zero-quantum and block-entry-serial overflow rejection.

The complete standard suite is `306/306`, including the existing Semantic IR,
lifter/interpreter, M7/M9, M11/M12, M18/M20, and M22-M28 regressions.

## Private former-frontier result and next frontier

The ordinary default runs crossed `main:0x72000c3550` naturally after the
former M28 state. Each final report was `37,770,905` bytes with SHA-256
`0702b7fd69d16d0b63e47a888fb2af7746c1d6921c0ca527f3a447cb0aea3789`.
The former instruction's seven operations ran in order; both stores completed
once, and execution proceeded normally into later execution/refinement work.
The ordinary final attempt contained `100,716` total IR operations, which is
strictly beyond the former `100,000` frontier. No forced PC, entry override,
candidate seed, memory patch, instruction skip, or budget override was used
for either ordinary run.

For the former instruction specifically, the pre-state was `x0 =
0x72045a6158`, `x9 = 0x720459d9b0`, and `x1 = 0x720415a168`, with guest PC
`0x72000c3550`. The post-state memory effects were one eight-byte store of
`x9` at `[x0]` and one eight-byte store of `x1` at `[x0+8]`; no register
writeback was present in this offset-form STP. The default `4,096` quantum did
not cut inside this seven-operation instruction, and the synthetic mid-store
oracle exercises the same ordering with slices between the stores.

The last completed ordinary execution result before the next generation was
blocked had `main:0x7200000030` as its current function and stopped at
`main:0x7200000148`, `blr x8`, with target
`main:0x72001334c0` and guest-load provenance
`main:0x720456b860`. The next generation could not map its controlled stack,
so this prior execution evidence carries the typed memory-resource stop
diagnostic.

The first genuine next blocker was not an IR cliff or indirect-target
certification issue. After the later execution/refinement attempts had
completed, starting another immutable execution generation required another
synthetic controlled stack. The process-wide guest-memory mapping resource
rejected that mapping with:

`resource_limit: total mapped guest size exceeds the configured maximum`

M29 reports this as the typed
`guest_memory_resource_limit_exceeded` stop reason and does not raise the
guest-memory limit. This is the next measured frontier and is intentionally
left for M30.

## Determinism, validation, and privacy

The report schema is now `15`. It distinguishes nullable explicit
`max_ir_operations`, its provenance, `slice_ir_operations`, total IR
operations, slice/yield/resume counters, maximum operations in one slice, and
the terminal IR cursor. Scheduler-internal fields do not affect guest state,
candidate identity, or refinement accounting. The M29 synthetic results are
independent of slice size. Final private report sizes and hashes are recorded
in the milestone handoff; no private report, binary, key, local configuration,
absolute machine path, or proprietary byte is committed.

The final local validation was `306/306` for the standard suite, `261`
assertions in ten focused M29 cases, `306/306` under ASan/UBSan, and `306/306`
under TSan. The final-head PR validation also passed Linux/GCC, Linux/GCC /
LLVM 18, Linux/GCC / ASan + UBSan, Linux/GCC / TSan, and Windows/MSVC.
No new dependency was introduced.

## Known limitations and M30 recommendation

The controlled process currently retains each generation's synthetic stack in
the shared guest-memory image. The finite memory-mapping resource is therefore
the newly observed blocker after the former IR frontier. M30 should investigate
transactional/reclaimable generation stack ownership or an equivalent
proof-preserving memory accounting model, beginning from the exact resource
failure above. It should not change indirect-target certification or raise
the memory ceiling without a separate finite-resource justification.
