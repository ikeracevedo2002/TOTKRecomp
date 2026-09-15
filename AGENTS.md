# TOTKRecomp agent operating contract

This file is persistent project context for future agent sessions. The
external Master Spec is authoritative for milestone scope, architecture,
invariants, stop conditions, and prohibited shortcuts. Pi must not silently
redesign those decisions.

This contract defines how Pi should turn real execution frontiers into reusable
project capability. The goal is not to move one instruction at a time. The goal
is to use each frontier as evidence for the next missing architectural layer,
implement that layer faithfully, and amortize the investigation across as much
future execution as the evidence supports.

## Execution model

Pi is the sole project agent. It owns the full milestone lifecycle: inspect the
current verified state, reproduce the baseline when required, prove the scoped
dependency, implement the change, perform focused builds and tests, run private
real validation when a milestone requires it, create committed checkpoints,
self-review the resulting state, fix justified blocking defects, and run final
CI.

There is no persistent multi-agent role split, verifier handoff,
detached-review requirement, or tmux-based agent coordination. When an
independent review is explicitly requested, treat it as an additional
validation activity rather than a second persistent agent role.

The normal topology is:

```text
external architect -> Pi -> checkpoint SHA -> validation/self-review
                   -> Pi fixes -> final validation
```

## Development objective

Optimize for reusable, evidence-backed Horizon/runtime coverage, not for the
smallest possible patch that moves the PC.

A new execution frontier is evidence. It is not automatically the next isolated
implementation target.

For every genuine frontier:

1. prove the immediate cause far enough to distinguish missing capability from
   a bug in existing capability;
2. classify the dependency by architectural subsystem;
3. decide whether the correct fix is local or belongs to a reusable substrate;
4. implement the smallest architecturally complete change justified by the
   evidence and milestone scope;
5. validate it proportionally to risk;
6. perform bounded reconnaissance beyond the crossed frontier before committing
   to another isolated blocker;
7. cluster newly discovered dependencies and select the next work item by
   architectural leverage, not simply by nearest address.

Do not silently widen a milestone beyond the Master Spec. If reconnaissance
proves that the highest-leverage substrate is outside the current scope, record
it as the proposed next milestone instead of implementing it opportunistically.

## Progress model

Track two mandatory, distinct forms of progress.

### Architecture coverage

This is the primary planning KPI. It answers: what reusable part of the Switch /
Horizon execution environment is now correctly represented that was not before?

Examples include verified process layout, loader provenance, relocation,
initial-thread bootstrap, TLR/TLS, SVC semantics, handles, synchronization,
IPC/services, filesystem behavior, GPU interfaces, or reusable AArch64
semantics.

Architecture progress may be real even when the measured execution frontier
does not move in the same milestone, but it must be tied to a proven current or
near-frontier dependency. Never label such work as a frontier advance.

### Real execution frontier

This is the empirical execution KPI: the highest number of guest instructions
actually executed on the frozen real input, together with the exact next
`stop_reason`, module/address, and enough context to reproduce the boundary.

- Treat the static `unsupported instructions` counter as a scanner diagnostic.
  Never present it as the headline progress metric.
- A lower `unsupported` count with an unchanged or earlier real frontier is not
  a frontier advance.
- State frontier before, frontier after, and the next `stop_reason` in every
  milestone report that includes a real run. If it did not move, say so.
- Accept frontier movement only from a reproduced real run under the frozen
  measurement contract. Synthetic fixtures prove semantics, not frontier.
- Never trade architectural correctness for a larger instruction count.

The two metrics must not be collapsed into one number. Frontier movement without
correct reusable semantics can be fake progress; reusable semantics without a
real run must not be reported as measured execution advance.

## Frontier classification

Classify every genuine blocker before choosing the implementation strategy.
Use the closest applicable subsystem and record uncertainty explicitly.

```text
CPU / AArch64 semantics
memory / virtual address layout
loader / MOD0 / relocation / module provenance
Horizon process / kernel / SVC
thread / TLR / TLS / thread identity
synchronization / scheduling
handles / objects / shared memory
filesystem / storage
IPC / services
GPU / display / graphics runtime
game-specific behavior
TOTKRecomp runtime defect
toolchain / refinement / performance infrastructure
```

A blocker that is initially observed in one instruction can still belong to a
larger subsystem. Do not equate instruction locality with architectural scope.

## Frontier reconnaissance

After crossing a genuine frontier, do not immediately begin another
blocker-patch-run loop. First perform bounded reconnaissance within the declared
run/time budget.

Reconnaissance may combine:

- running forward to the next genuine stop;
- preserving the trace, call path, register/memory evidence, and typed stop;
- static inspection of the reachable basic blocks and nearby call graph;
- identifying repeated accesses to the same Horizon/runtime facility;
- locating already-reachable later dependencies that can be proven without
  fabricating state;
- mapping dependencies to existing or missing reusable abstractions.

Maintain a small queue of upcoming blockers when practical, ideally several
rather than exactly one. The queue must distinguish:

```text
observed  — reached in a real run
proven    — dependency is demonstrated by code/trace evidence
hypothesis — plausible from static reconnaissance but not yet proven
```

Never promote a hypothesis to `proven` to make the queue look fuller. A queue of
one honest blocker is better than ten guessed blockers.

## Cluster before patching

The default development unit is a subsystem, not an individual faulting
instruction.

If two or more nearby blockers belong to the same subsystem, or reconnaissance
shows strong evidence that the same facility will recur, stop treating them as
independent patches. Define the minimum reusable substrate that covers the
shared semantics and implement that substrate within authorized scope.

Prefer:

```text
frontier -> proof -> reconnaissance -> blocker cluster
         -> reusable substrate -> focused validation -> long run
```

over:

```text
frontier -> patch -> run -> frontier -> patch -> run
```

A reusable substrate is not permission to emulate speculative future behavior.
Implement only the surface for which semantics, ownership, lifecycle, and
failure behavior are sufficiently evidenced. Keep unsupported behavior honest
and observable.

## Selecting the next target

Do not select the next task solely because it is the closest PC after the
current frontier. Prefer the highest-leverage proven target allowed by the
Master Spec.

Evaluate at least:

- current or near-frontier reachability;
- number of observed/proven blockers it can eliminate;
- likelihood of reuse across `rtld`, `main`, `subsdk0`, and `sdk`;
- architectural centrality and prerequisite relationships;
- evidence confidence;
- implementation and validation cost;
- risk of creating game-specific or fabricated state.

A small local fix is correct when the behavior is genuinely local. Do not force
an abstraction where no repeated architectural dependency exists.

## Evidence levels

Use evidence proportional to the permanence and impact of the implementation.
Do not spend architectural-proof effort on every observation, and do not build
permanent runtime APIs from weak evidence.

### P0 — observation

Record what the guest actually did: instruction, operands, relevant state,
address, call context, memory access, or typed stop. P0 can identify a lead but
is not enough for a reusable semantic contract.

### P1 — dependency proof

Prove the minimum semantics required by the current reachable code path: what
value/object/operation is required, where it comes from, and why existing
behavior is insufficient. P1 is normally enough for a local, narrowly bounded
implementation when ownership and lifecycle are not ambiguous.

### P2 — architectural proof

Establish the invariants needed for a reusable substrate: ownership, lifetime,
provenance, layout, ordering, failure semantics, cross-module use, and the
boundary between guest-owned and host/runtime-owned state.

Use P2 when introducing or changing persistent architectural abstractions.
Do not require exhaustive knowledge of unrelated Horizon behavior before
implementing a proven subset.

## Architecture coverage record

Keep project documentation sufficient to answer, for each important subsystem:

```text
subsystem
state: unknown | observed | partial | verified
evidence level: P0 | P1 | P2
current TOTK reachability
implemented reusable surface
focused/regression tests
known unsupported surface
observed/proven upcoming blockers
```

The coverage record exists to prevent rediscovering the same Horizon/runtime
facts in later milestones. Update the appropriate architecture/milestone
document when a durable subsystem invariant changes.

## Frozen measurement contract

Freeze the input before implementing, not after measuring.

Before a milestone uses real frontier movement as evidence it must have a
validated module-set manifest:

```text
module logical name, byte size, SHA-256, load order, recovery/version identity
expected frontier and expected next stop_reason for that exact input
```

- If the baseline does not reproduce the expected frontier, declare the
  measurement BLOCKED. Do not reinterpret the mismatch or switch to a more
  convenient input to obtain a better number.
- Re-derive and revalidate the manifest whenever the module set, load order, or
  recovery version changes. An unvalidated manifest is not a measurement input.
- Keep the manifest and its hashes in the private runtime directory; commit only
  non-proprietary schema, methodology, and validation outcome.
- Reconnaissance may inspect code statically without re-running the complete
  measurement sequence, but any claimed real frontier still requires this
  contract.

## Transient frontier state

Do not hardcode the current execution blocker in this persistent operating
contract. Exact current frontiers change frequently and belong in the active
milestone document and external handoff/task state.

When starting work, derive the current frontier from the newest verified
checkpoint and active milestone evidence. If a stale document names an older
frontier, do not route development around the newer reproduced boundary merely
because the older text exists.

## Time budget and investigation budget

Declare a wall-clock budget for every real run before starting it.

- Exceeding the declared budget is a blocking defect for that run, not a passing
  result with a footnote.
- Record budget and measured wall/user/system time when the milestone reports a
  real run.
- Attribute dominant cost from current measurements. Do not preserve an old
  percentage assumption after the runtime profile changes.
- Bound reconnaissance too. The purpose is to expose the next architectural
  cluster, not to turn every crossed frontier into an unbounded research phase.
- Prefer a small amount of high-value reconnaissance that changes target
  selection over exhaustive speculative reverse engineering.

## Validation proportional to risk

Match validation cost to the semantic risk of the delta.

- Run sanitizers, TSan in particular, nightly or on demand for semantic and
  concurrency changes. Do not run them at every checkpoint.
- Run the byte-identical A/B pair of the real run exactly once at the final
  checkpoint when the active milestone requires it.
- Use the light lane for documentation-only deltas. They justify no C++ build,
  sanitizer, or real run.
- Keep focused L1/L2/L3 checks as the default per-edit loop.
- Subsystem work requires focused tests for the reusable contract, not only a
  test that the original faulting PC now passes.
- Add regression coverage for ownership, provenance, invalid-state rejection,
  or failure semantics whenever those are part of the architectural proof.

## Architecture authority and no fake progress

Do not advance TOTK by forcing PCs, forcing indirect targets, inventing
providers, hardcoding register values, returning fake runtime success,
weakening validated completeness/provenance checks, or arbitrarily increasing
limits.

Do not add game-specific state merely because it predicts the next few
instructions. Unsupported behavior remains an honest, observable boundary until
its governing semantics are sufficiently evidenced and the Master Spec permits
the capability.

A correct refusal is a result. Preserve it rather than routing around it.

## Instruction-family fan-out

Keep one instruction family inside a bounded set of layers: decoder
normalization, coverage predicate, Semantic IR, verifier, interpreter, and
lowering.

- When the coverage record and the lifter predicate diverge for a family, fix
  the shared predicate. That divergence is a structural defect; do not reward
  it with more code.
- Do not increase the number of layers a family touches without a stated reason
  in the milestone document.
- Instruction-family work that is not reachable from the current or
  reconnaissance-proven near frontier is coverage work, not frontier work,
  unless explicitly scoped by the Master Spec.

## Privacy

Never commit Nintendo binaries, NSOs, XCI/NSP content, extracted game assets,
keys, firmware, proprietary SDKs, private configurations, private real
execution reports, machine identifiers, absolute private paths, build output,
agent logs, provider credentials, API tokens, or `.DS_Store` files. Runtime
communication belongs outside the tracked repository.

## Local content protection

The local game content folder is git-ignored and is never deleted, cleaned, or
committed.

- Do not run `git clean -fdx` in the main working tree. It destroys the
  untracked, ignored local content that cannot be re-acquired.
- Do not run `git reset --hard` in the main working tree. Remove only the
  specific paths you created, and only after naming them.
- Never stage an ignored local path to make a check pass. Move the artifact
  outside the repository instead.

## Git and checkpoints

- Follow exact stacked lineage supplied by the Master Spec.
- Use one milestone number per branch and per document.
- Do not develop milestone N on the uncommitted working tree of milestone N-1.
  Checkpoint N-1 first, then branch.
- Do not leave a branch whose name contradicts the milestone number it contains.
  Rebuild or rename the branch rather than documenting the mismatch.
- Do not merge or opportunistically rebase from `main`.
- Preserve unrelated dirty state exactly; do not reset, stash, clean, stage, or
  commit it.
- Use committed checkpoints as stable references for self-review and validation
  when needed; no second-agent review worktree is required.
- Record transient checkpoint and CI identifiers in the external handoff, not
  tracked project documents.

## Build ladder

Use the smallest level that answers the question:

```text
L0 — inspect/edit only
L1 — compile affected target(s)
L2 — focused new/changed tests
L3 — relevant regression filters
L4 — complete standard suite, only when implementation is stable
L5 — sanitizers, nightly or on demand for semantic changes
L6 — remote full CI, only near a code-complete checkpoint
```

Prefer persistent Ninja build directories. Avoid unnecessary full rebuilds,
reconfiguration, sanitizer reruns, cleaning, and remote pushes used only as
compile-error detectors. Documentation-only edits do not justify C++ builds or
sanitizer runs.

## Milestone completion and handoff

A frontier-oriented milestone should leave more than a moved PC. Its handoff
should state, when applicable:

```text
architecture coverage added or strengthened
frontier before / after
next exact stop_reason
frontier classification
proof level reached
reusable substrate introduced or extended
reconnaissance findings
observed/proven blocker queue
recommended next highest-leverage subsystem
validation performed
remaining unsupported surface
```

If the next blocker is merely another instance of the subsystem just built,
prefer extending/fixing the substrate within authorized scope rather than
opening a sequence of tiny milestones around individual addresses.

## CI run IDs

Never create a commit solely to update a tracked document from one “latest CI
run ID” to another. Workflow run IDs are transient and belong in the final
external handoff.
