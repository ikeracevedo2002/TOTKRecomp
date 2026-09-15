# Engineering workflow

This document is the practical operating procedure for milestone work. It is
not a generic agent framework. `AGENTS.md` defines the authority model and
non-negotiable project rules; this document defines how Pi should run the work.

## Operating loop

The default loop is architecture-first and evidence-driven:

```text
inspect newest relevant branch/checkpoint
   -> read active objective / Master Spec
   -> seed or refresh dynamic TODO graph
   -> reproduce frozen baseline when real-frontier evidence is required
   -> prove immediate dependency
   -> parallel bounded exploration where useful
   -> synthesize evidence / classify subsystem
   -> choose local fix or reusable substrate
   -> delegate bounded implementation where appropriate
   -> integrate and inspect diff
   -> focused build/tests/review
   -> real run when justified
   -> bounded reconnaissance beyond crossed frontier
   -> cluster blockers / rewrite TODO graph
   -> checkpoint when the integrated state is stable
   -> final proportionate validation and handoff
```

Do not decide what to measure after seeing the measurement. Do not keep
executing an obsolete TODO sequence after new evidence changes the optimal path.

## Session start

Before changing code, Pi should establish the real current state rather than
trusting historical conversation context.

1. Identify the most advanced relevant branch/commit and inspect its HEAD.
2. Read the active milestone/objective and the current durable architecture
   documentation.
3. Determine the latest verified real frontier from current evidence.
4. Confirm whether a frozen manifest/baseline is required for the work at hand.
5. Seed the TODO graph with only the work needed to reduce the current decision
   uncertainty and reach the objective.

A stale milestone document or old chat is context, not authority over newer
verified repository/runtime evidence.

## TODO graph procedure

The TODO plugin is the live control plane for a session. Tasks should be small
enough to have a clear acceptance condition but large enough to represent a
meaningful engineering step.

Useful task kinds are:

```text
OBSERVE     collect a concrete runtime/static fact
PROVE       establish a dependency or invariant
EXPLORE     map a bounded part of the code/runtime
SYNTHESIZE  combine evidence and update the working model
DESIGN      choose an architectural contract
IMPLEMENT   perform a decided code change
VERIFY      build/test/review
RECON       inspect what lies beyond a crossed frontier
CHECKPOINT  record a stable accepted Git state
```

The TODO graph is deliberately dynamic:

- add or split tasks when evidence exposes a missing prerequisite;
- remove tasks when an assumption is disproven;
- reorder tasks when a higher-leverage dependency becomes proven;
- fan out independent exploration in parallel;
- do not let implementation tasks decide unresolved architecture implicitly;
- preserve a small queue of observed/proven upcoming blockers when practical;
- keep speculative hypotheses out of the implementation queue until they are
  sufficiently proven.

The orchestrator should periodically ask whether the graph is still the shortest
architecturally correct route to the objective.

## Subagent delegation

Pi owns global reasoning and integration. Subagents provide bounded work.

Use the configured roles as follows:

| Role | Default use | Write authority |
| --- | --- | --- |
| `code-explorer` | Searches, call paths, layouts, traces, analogous code/tests, evidence inventory | Read-only |
| `quick-implementer` | Mechanical or highly specified change in one/two files | Narrow scoped writes |
| `implementer` | Decided multi-file feature/bug fix with explicit contract | Scoped writes |
| `code-reviewer` | Independent review of integrated diff/contract | Read-only |
| `commit-pusher` | Exact commit/push operation after Pi acceptance | Git-only, exact paths/ref |

If plugin names differ, preserve these roles conceptually.

### When to delegate

Prefer a subagent when the task:

- has a bounded question or deliverable;
- can be evaluated from concrete evidence;
- is mostly search, inspection, repetition, or deterministic command work;
- would consume substantial orchestrator context without requiring global
  architectural reasoning;
- can run independently from another read-only investigation;
- implements semantics already decided by Pi.

Keep work in the orchestrator when it decides ownership, provenance, lifecycle,
Horizon ABI semantics, subsystem boundaries, evidence sufficiency, fake-vs-real
progress, integration strategy, or next-target selection.

### Delegated prompt shape

A good delegated prompt includes:

```text
TASK
WHY IT MATTERS
KNOWN FACTS
ALLOWED PATHS
READ-ONLY OR WRITE SCOPE
EXPECTED DELIVERABLE
ACCEPTANCE CHECKS
PROHIBITED ACTIONS
```

Exploration output should separate facts from inference:

```text
OBSERVED FACTS
INFERENCES
UNCERTAINTIES
EVIDENCE LOCATIONS
RECOMMENDED FOLLOW-UP
```

Pi must inspect delegated output. A subagent conclusion is not automatically a
project conclusion.

## Parallelism and checkout safety

Parallelize independent read-only investigations aggressively when it reduces
uncertainty faster than serial work.

Do not run overlapping writers against the same checkout. For concurrent
implementation, use isolated worktrees and disjoint scopes or keep the work
serial. Pi performs the final integration.

Never allow a worker to use broad/destructive repository operations in the main
working tree. `git clean -fdx` and `git reset --hard` are prohibited there. Do
not delegate merges, rebases, arbitrary resets, broad staging, or conflict
resolution as routine mechanical tasks.

## Progress accounting

Track architecture coverage and real execution frontier separately.

### Architecture coverage

Report the durable capability added or strengthened, such as loader provenance,
process layout, thread/TLR/TLS semantics, SVC behavior, handles,
synchronization, IPC, filesystem, GPU runtime interfaces, or reusable AArch64
semantics.

Architecture coverage is the primary planning signal. It must be connected to a
current or reconnaissance-proven near-frontier dependency; unrelated coverage
work must be labeled as such.

### Real frontier

When a real run is performed, report:

```text
frontier before:   <guest instructions executed> @ <module:address>
frontier after:    <guest instructions executed> @ <module:address>
next stop_reason:  <exact typed reason>
```

- Only a real run under the frozen measurement contract establishes a frontier.
- Synthetic fixtures establish semantics, not frontier movement.
- The scanner's `unsupported instructions` count is diagnostic, not the primary
  progress measure.
- An unchanged frontier does not erase real architecture progress, but it must
  never be presented as a frontier advance.

## Frontier proof and classification

For each genuine blocker, establish enough evidence to determine whether it is:

- a bug in an already implemented capability;
- a missing architectural capability;
- a tooling/refinement/performance limitation;
- a correct refusal caused by absent evidence or unsupported semantics.

Classify the blocker into the nearest subsystem before selecting the fix. The
current instruction/address is not necessarily the architectural scope.

Use evidence levels from `AGENTS.md`:

- P0 observation;
- P1 dependency proof;
- P2 architectural proof.

Local bounded fixes often need P1. Persistent reusable abstractions should have
P2-quality evidence for the invariants they encode.

## Exploration and synthesis

When the immediate dependency is non-trivial, Pi should fan out bounded
investigations instead of serially reading the whole repository itself.

Typical parallel exploration:

```text
                    current blocker
                         |
          +--------------+--------------+
          |              |              |
          v              v              v
     call-path map   cross-module use   existing analog/tests
          |              |              |
          +--------------+--------------+
                         |
                         v
                    Pi synthesis
```

Synthesis is an orchestrator responsibility. Pi should explicitly decide:

- what is verified versus inferred;
- whether multiple blockers share a substrate;
- whether current evidence changes the TODO graph;
- whether implementation can be delegated mechanically;
- which unknowns actually block the next decision.

Do not continue collecting evidence merely because more evidence is possible.
Collect enough to make the next architectural decision safely.

## Reusable-substrate rule

Prefer a subsystem-sized solution when multiple reachable blockers share the
same semantics.

After two local fixes in one architectural family, a third local fix requires a
written justification for why a common reusable abstraction is not appropriate.

A reusable abstraction must still be minimal and evidence-backed. Do not use
"generalization" as permission to emulate speculative Horizon behavior.

## Anti-stagnation procedure

Trigger a strategy review when any of the following occurs:

- the same real frontier survives two implementation cycles;
- consecutive blockers repeatedly fall in the same subsystem;
- two exploration rounds add no decision-relevant evidence;
- the TODO list expands while architecture coverage does not;
- repeated patches accumulate around one abstraction;
- the same build/run/debug loop repeats without changing the decision space;
- a hypothesis cannot be strengthened using the current evidence source;
- execution advances trivially and immediately stops on a closely related
  dependency.

During a strategy review Pi stops the local patch loop and performs:

```text
1. Restate verified facts.
2. Separate assumptions/hypotheses.
3. Restate the actual objective and stop condition.
4. Reclassify the blocker/subsystem if needed.
5. Search for a shared architectural dependency.
6. Fan out alternative evidence sources/directions where useful.
7. Reconsider task granularity and substrate boundaries.
8. Rewrite the TODO graph around the highest-leverage proven route.
```

The point is not to force a different answer; it is to prevent a weak local
strategy from consuming the whole session.

## Reconnaissance after advancement

Crossing a frontier is not the end of the reasoning cycle. Within a bounded
investigation/run budget, inspect what follows before choosing the next isolated
implementation target.

Reconnaissance may include:

- running to the next honest stop;
- collecting the next trace/call path;
- static inspection of reachable nearby code;
- parallel inventories of repeated facilities/consumers;
- recording several upcoming blockers as `observed`, `proven`, or `hypothesis`.

Use the result to cluster dependencies and choose the next task by architectural
leverage rather than nearest PC alone.

## Frozen measurement contract

Before claiming real frontier movement, validate and record the input manifest:

```text
module logical name, byte size, SHA-256, load order, recovery/version identity
derived expected frontier and expected next stop_reason
```

Validation is complete only when every module hashes and orders as declared and
the baseline reproduces the expected frontier on the current toolchain.

- If the baseline does not reproduce, the measurement is **BLOCKED**.
- Do not re-baseline the expectation to match a convenient observed stop.
- Do not substitute a smaller, older, or more convenient module set.
- Re-freeze whenever module set, load order, or recovery version changes.
- Store private manifests/hashes outside the tracked repository.
- Static reconnaissance does not require a fresh real-run measurement contract,
  but any claimed frontier does.

## Runtime and investigation budgets

Every real run declares a wall-clock budget before it starts.

- Exceeding the budget blocks that run/result.
- Record measured wall/user/system time when reporting real execution.
- Attribute current dominant costs from current data; do not preserve stale
  percentages from historical runs.
- Give reconnaissance a bounded budget too.
- Prefer several focused parallel investigations over one unbounded repository
  exploration when they answer the same decision.

## Implementation and integration flow

Once Pi has decided a semantic contract:

1. define exact acceptance criteria and affected invariants;
2. delegate mechanical implementation if suitable;
3. integrate into the authoritative checkout;
4. inspect the entire relevant diff;
5. verify no unrelated architectural policy changed;
6. run the smallest build/test level that proves the change;
7. use a `code-reviewer` for an independent bounded review when semantic risk
   justifies it;
8. fix only justified findings;
9. run the real frontier measurement when the milestone requires it;
10. perform reconnaissance and update the TODO graph before declaring the next
    target.

Subagent implementation is never accepted solely because it compiles or reports
success.

## Build discipline

Use the lowest useful level:

```text
L0 — inspect/edit only
L1 — compile affected target(s)
L2 — focused new/changed tests
L3 — relevant regression filters
L4 — complete standard suite, only when implementation is stable
L5 — sanitizers, nightly or on demand for semantic changes
L6 — remote full CI, only near a code-complete checkpoint
```

Do not begin with a full rebuild when a focused check answers the question. Do
not repeat an expensive level without a concrete reason. Keep persistent Ninja
directories, avoid needless CMake reconfiguration or clean builds, and do not
run C++ or sanitizer validation for documentation-only changes.

## Validation proportional to risk

Select validation from semantic risk, not milestone number.

| Delta | Required validation |
| --- | --- |
| Documentation only | Light lane; no C++ build, sanitizer, or real run |
| Test-only or fixture-only | L1/L2 on affected tests |
| Local refactor, no semantic change | L1/L2 plus relevant L3 filter |
| Semantic/lowering change | L2/L3, L4 once stable |
| Reusable runtime/subsystem contract | Focused contract/rejection tests + relevant L3/L4 |
| Concurrency/shared-state change | TSan on demand, otherwise nightly TSan |
| Final code-complete checkpoint | L4, required real A/B run, L6 remote CI |

Do not validate only that the original faulting PC now passes. Reusable
substrates require tests for their contract, invalid-state rejection, ownership,
provenance, and failure semantics where applicable.

## Checkpoints and review

Pi creates a checkpoint after the integrated scoped state is locally stable and
its focused validation passes.

The current workflow does **not** require a persistent Codex orchestrator, tmux
Pi verifier, detached verifier worktree, or fixed two-round reviewer protocol.
Those were historical mechanics and are not part of the current authority
model.

Pi may spawn `code-reviewer` as an ephemeral independent reviewer. Review is an
input to Pi's acceptance decision, not a handoff of ownership.

A `commit-pusher` may perform the final mechanical Git operation only when Pi
has already inspected the exact diff, named the exact paths/ref, and accepted
required validation.

## Lineage hygiene

- Use one milestone number per branch/document when the active milestone model
  calls for milestone branches.
- Do not develop milestone N on the uncommitted state of milestone N-1.
- Confirm HEAD/branch/milestone identity before scoped implementation.
- Preserve unrelated dirty state exactly.
- Do not opportunistically merge/rebase `main` into milestone work.
- Do not use destructive cleanup to repair lineage mistakes.

## Instruction-family fan-out

Record which layers a family touches: decoder normalization, coverage predicate,
Semantic IR, verifier, interpreter, and lowering.

- Keep the layer set bounded and state why it must expand.
- If coverage and lifter predicates diverge, repair the shared predicate rather
  than adding compensating special cases.
- Unreachable instruction-family work is coverage work unless explicitly scoped
  or reconnaissance proves it is near-frontier relevant.

## CI architecture

Pull requests run on `opened`, `synchronize`, and `reopened`. Push validation is
restricted to `main`; manual dispatch requests the full matrix. PR concurrency
is scoped to the PR and cancels stale runs.

The project-owned `scripts/ci/classify_changes.py` is a pure standard-library
classifier. Only `docs/**`, `README.md`, and `AGENTS.md` are light. Everything
else, including mixed changes, unknown paths, empty input, and malformed input,
is heavy by default. Event/API logic is separate in
`scripts/ci/ci_decision.py`.

For PR `synchronize`, the decision logic uses the exact new push delta
`github.event.before -> github.event.after`, validates existence and ancestry,
and does not classify the entire historical PR range. Rewritten/unavailable
ranges are heavy.

A documentation-only delta may use the light lane only when its predecessor SHA
has a completed successful `CI Gate` for the same PR under the existing carry-
forward rules. The heavy lane preserves the five standard validations: Linux
GCC, Linux GCC with LLVM 18, Linux GCC ASan+UBSan, Linux GCC TSan, and
Windows/MSVC. `CI Gate` remains authoritative for CI acceptance.

## Privacy and repository hygiene

Keep Nintendo data, private configuration, real execution reports, credentials,
absolute personal paths, machine identifiers, build artifacts, agent logs, and
runtime communication out of Git. Inspect staged names/diffs before each commit.

The local game-content folder is git-ignored and is never deleted, cleaned, or
committed. Never run `git clean -fdx` or `git reset --hard` in the main working
tree. Unstage or relocate a specific artifact rather than using destructive
cleanup.

## Handoff format

The external handoff should record, when applicable:

```text
base / branch / final HEAD
objective and achieved scope
architecture coverage added or strengthened
frontier before / after
exact next stop_reason and classification
proof level reached
reusable substrate introduced/extended
reconnaissance findings
observed/proven blocker queue
remaining hypotheses/uncertainties
TODO graph state and deferred high-leverage tasks
subagent work accepted/rejected and important findings
validation actually run and why it was proportionate
runtime/investigation budget results
privacy / semantic-integrity statement
recommended next highest-leverage target
```

Transient CI IDs, manifest hashes, worker logs, prompt hashes, and private run
artifacts belong in the external handoff/runtime directory, not in another
tracked commit.
