# Engineering workflow

This document is the practical operating procedure for milestone work. It is
not a generic agent framework.

## Order of operations

```text
freeze input manifest and expected frontier
   -> declare wall-clock budget and validation level
   -> implement the scoped change
   -> focused build and tests
   -> one real run, measured against the frozen input
   -> checkpoint SHA
   -> detached review worktree at exactly that SHA
   -> Pi report
   -> fix justified blockers
   -> final checkpoint: single A/B real run, remote CI, handoff
```

Do not decide what to measure after seeing the measurement.

## Progress accounting

Report frontier movement, not counter movement.

```text
frontier before:   <guest instructions executed> @ <module:address>
frontier after:    <guest instructions executed> @ <module:address>
next stop_reason:  <exact typed reason>
```

- The scanner's `unsupported instructions` count is a diagnostic line in the
  coverage section. Never place it in the status headline, a milestone title,
  or the handoff's notion of advance.
- If `frontier after` is not strictly greater than `frontier before` under the
  same frozen input, the milestone records no capability advance. Say so
  explicitly rather than describing the unsupported delta as progress.
- Only a real run under the frozen measurement contract establishes a frontier.
  Synthetic fixtures establish semantics.

## Frozen measurement contract

Before implementing, validate and record the input manifest:

```text
module logical name, byte size, SHA-256, load order, recovery/version identity
derived expected frontier and expected next stop_reason
```

Validation is complete only when every module hashes and orders as declared and
the unmodified baseline reproduces the expected frontier on the current
toolchain. Then:

- If a run does not reproduce the expected frontier, declare the milestone
  **BLOCKED** in its first line. Stop and report.
- Do not re-baseline the expectation to match the observed stop point.
- Do not substitute a smaller, older, or more convenient module set in order to
  obtain a comparable or better number.
- Re-freeze the manifest whenever module set, load order, or recovery version
  changes, and state that a re-freeze occurred.
- Store manifests and hashes in the external runtime directory. Commit schema,
  procedure, and outcome, never proprietary content or absolute local paths.

## Current blocker

Real execution is blocked at the guest-provider boundary `__nnmusl_init_dso`:
the owning module is declared incomplete and the eligible `sdk` candidate is
discarded by the completeness policy. That rejection is the intended invariant.

- This blocker outranks instruction-family convergence in milestone selection.
- A milestone may not be opened on an instruction family that the current
  frontier cannot reach unless the Master Spec explicitly scopes it.
- Clear it with manifest-verified completeness and faithful bootstrap evidence,
  never with an asserted complete state, a host stub, or invented rtld state.

## Wall-clock budget

Every real run declares a wall-clock budget before it starts, and the budget is
part of the acceptance criteria.

- Exceeding the budget is a blocking defect. Report it as a blocker, not as a
  footnote beside a passing test count.
- Attribute the time: refinement dominates wall time by roughly 98% and is the
  optimisation target; guest execution is not.
- Record budget, measured wall/user/system time, and refinement share together.
  Report an aborted or terminated run with the elapsed time at termination.

## Checkpoint and review flow

```text
Architect
   -> master milestone specification
Codex Orchestrator
   -> implementation and focused validation
checkpoint SHA
   -> detached review worktree at exactly that SHA
tmux Pi verifier
   -> stdout report and stderr log in an external runtime directory
Codex
   -> consume report, fix justified blockers, validate
final checkpoint
   -> remote CI and handoff
```

Codex creates a checkpoint only after the scoped implementation is locally
stable. The review worktree must report the same SHA as the checkpoint before
Pi starts. Pi receives a file-backed prompt and reviews the committed tree,
not an in-progress worktree. Runtime files such as prompts, reports, logs,
metadata, and status files live outside the repository, normally under a
directory such as `../TOTKRecomp-agent-runtime/` or `/private/tmp`.

The normal maximum is two Pi rounds. Round one audits the implementation
checkpoint. If it reports a valid BLOCKING finding, Codex fixes only the
justified defect, runs focused checks, commits a new checkpoint, recreates or
updates the detached review worktree, and runs round two. Non-blocking style
suggestions do not require a new round. A third round is exceptional and is
only for confirming a concrete blocking correction; unresolved blockers must
be reported rather than bypassed.

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

Do not begin with a full rebuild when a focused check answers the question.
Do not repeat an expensive level without a concrete reason. Keep persistent
Ninja directories, avoid needless CMake reconfiguration or clean builds, and
do not run C++ or sanitizer validation for documentation-only changes.

## Validation proportional to risk

Select the validation set from the risk of the delta, not from the milestone
number.

| Delta | Required validation |
| --- | --- |
| Documentation only | Light lane; no C++ build, no sanitizer, no real run |
| Test-only or fixture-only | L1/L2 on the affected tests |
| Local refactor, no semantic change | L1/L2 plus one relevant L3 filter |
| Semantic or lowering change | L2/L3, L4 once when stable |
| Concurrency or shared-state change | TSan on demand, otherwise the nightly TSan |
| Final code-complete checkpoint | L4, one A/B real run, L6 remote CI |

- TSan costs roughly 18 minutes. Run it nightly or on demand for semantic and
  concurrency changes; do not attach it to every checkpoint.
- Run the byte-identical A/B pair of the real run exactly once, at the final
  checkpoint. Repeating it to chase a nicer hash is waste.
- A documentation-only delta never justifies a sanitizer rerun or a push used as
  a compile-error detector.

## Lineage hygiene

- Use one milestone number per branch and per milestone document.
- Do not develop milestone N on the uncommitted working tree of milestone N-1.
  Commit N-1, then branch from that checkpoint.
- Do not keep a branch whose name contradicts the milestone number it contains.
  Rebuild the branch from the correct base rather than documenting a mismatch.
- Before the first edit, confirm the checked-out HEAD, the branch name, and the
  milestone document number all agree. If they do not, stop and repair the
  lineage before writing code.

## Instruction-family fan-out

Record in the milestone document which layers a family touches: decoder
normalization, coverage predicate, Semantic IR, verifier, interpreter, and
lowering.

- Keep the layer set bounded and state it explicitly.
- If the coverage record and the lifter predicate disagree for a family, treat
  that as a structural defect and repair the shared predicate. Do not add a
  second source of truth, a special case, or a compensating test to absorb the
  divergence.
- Pi should flag divergent coverage and lifter predicates as a structural
  finding even when every test passes.

## CI architecture

Pull requests run on `opened`, `synchronize`, and `reopened`. Push validation
is restricted to `main`; manual dispatch always requests the full matrix. PR
concurrency is scoped to the PR and cancels stale runs.

The project-owned `scripts/ci/classify_changes.py` is a pure standard-library
classifier. Only `docs/**`, `README.md`, and `AGENTS.md` are light. Everything
else, including mixed changes, unknown paths, empty input, and malformed input,
is heavy by default. The event/API logic is separate in
`scripts/ci/ci_decision.py`.

For a PR `synchronize`, the decision logic uses the new push delta
`github.event.before -> github.event.after`, checks that both commits exist,
checks ancestry, and computes that exact range. It never classifies the whole
base-to-head PR history. A rewritten or unavailable range is heavy.

A documentation-only delta may use the light lane only when the predecessor
SHA has a completed successful `CI Gate` for the same PR. The read-only GitHub
Actions lookup must match the exact SHA, pull-request event, PR number,
completed/successful run, and successful `CI Gate` job. Missing, failed,
cancelled, pending, unrelated, ambiguous, or API-error results are heavy.
This makes the invariant inductive: a successful light gate certifies that the
current executable state is the predecessor's validated state plus a strict
documentation-only delta.

The heavy lane preserves all five existing validations: Linux/GCC, Linux/GCC
with LLVM 18, Linux/GCC with ASan+UBSan, Linux/GCC with TSan, and Windows/MSVC.
`CI Gate` always runs. On the heavy path it requires all five results to be
`success`; on the light path it requires successful classification, carry-
forward proof, light checks, and all five heavy jobs to be `skipped`.

Nightly carries the sanitizer burden so ordinary checkpoints do not have to.
On-demand heavy reruns are requested by name when a semantic or concurrency
delta needs them before the nightly slot.

## Privacy and repository hygiene

Keep Nintendo data, private configuration, real execution reports, credentials,
absolute personal paths, machine identifiers, build artifacts, Pi logs, and
runtime communication out of Git. Before each commit inspect staged names and
diffs. Preserve unrelated local state and do not add `.DS_Store` files.

The local game content folder is git-ignored and is never deleted, cleaned, or
committed. Never run `git clean -fdx` or `git reset --hard` in the main working
tree; the ignored content there is not reproducible. Unstage or relocate a
specific artifact instead of reaching for a destructive command, and never
stage an ignored path to satisfy a check.

## Handoff format

The final external handoff records the base, branch, parent, final HEAD,
commits, PR and target, changed files, frozen manifest identity and expected
frontier, frontier before and after, exact next `stop_reason`, declared
wall-clock budget versus measured wall/runner time and refinement share, any
BLOCKED status, validation actually run and why it was proportionate, Pi
reviewed SHA/prompt hash/verdict/rounds, heavy and light workflow run IDs, job
results, duplicate-workflow result, dependencies, privacy audit,
semantic-integrity statement, and the exact next milestone base. CI run IDs and
manifest hashes are transient handoff data and must never be pinned in another
tracked commit.

## Live acceptance sequence

After the infrastructure checkpoint receives a green heavy `CI Gate`, make one
meaningful documentation-only follow-up on an allowlisted path. Confirm that
the new run classifies only the `before -> after` documentation delta, proves
the predecessor gate for the same PR, runs the light checks, skips every heavy
job, and passes `CI Gate`. Record workflow URLs, timestamps, job states, and
measured wall/runner time in the external handoff. Do not add a follow-up
commit containing those transient run identifiers.
