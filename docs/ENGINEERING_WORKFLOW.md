# Engineering workflow

This document is the practical operating procedure for milestone work. It is
not a generic agent framework.

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
L5 — sanitizers, only when final code requires them
L6 — remote full CI, only near a code-complete checkpoint
```

Do not begin with a full rebuild when a focused check answers the question.
Do not repeat an expensive level without a concrete reason. Keep persistent
Ninja directories, avoid needless CMake reconfiguration or clean builds, and
do not run C++ or sanitizer validation for documentation-only changes.

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

## Privacy and repository hygiene

Keep Nintendo data, private configuration, real execution reports, credentials,
absolute personal paths, machine identifiers, build artifacts, Pi logs, and
runtime communication out of Git. Before each commit inspect staged names and
diffs. Preserve unrelated local state and do not add `.DS_Store` files.

## Handoff format

The final external handoff records the base, branch, parent, final HEAD,
commits, PR and target, changed files, validation actually run, Pi reviewed
SHA/prompt hash/verdict/rounds, heavy and light workflow run IDs, job results,
duplicate-workflow result, measured wall/runner time, dependencies, privacy
audit, semantic-integrity statement, and the exact next milestone base. CI run
IDs are transient handoff data and must never be pinned in another tracked
commit.
