# TOTKRecomp agent operating contract

This file is persistent project context for future agent sessions. The
external Master Spec is authoritative for milestone scope, architecture,
invariants, stop conditions, and prohibited shortcuts. Codex and Pi must not
silently redesign those decisions.

## Roles

Codex is the orchestrator and primary implementer. It implements the scoped
change, performs focused builds and tests, runs private real validation when a
milestone requires it, creates committed checkpoints, launches Pi, consumes Pi
reports, fixes justified blocking defects, and runs final CI.

Pi is the independent verifier. It reviews exact committed SHAs, analyzes
invariants, attempts adversarial tests, and reports findings. Pi does not
modify production code; Codex owns corrections.

The normal topology is:

```text
external architect -> Codex -> checkpoint SHA -> detached review worktree
                   -> tmux Pi verifier -> report -> Codex fixes -> final validation
```

## Architecture authority and no fake progress

Do not advance TOTK by forcing PCs, forcing indirect targets, inventing
providers, hardcoding register values, returning fake runtime success, or
arbitrarily increasing limits. Unsupported behavior remains an honest,
observable boundary until the governing specification authorizes the next
capability.

## Privacy

Never commit Nintendo binaries, NSOs, XCI/NSP content, extracted game assets,
keys, firmware, proprietary SDKs, private configurations, private real
execution reports, machine identifiers, absolute private paths, build output,
agent logs, provider credentials, API tokens, or `.DS_Store` files. Runtime
communication belongs outside the tracked repository.

## Git and checkpoints

- Follow exact stacked lineage supplied by the Master Spec.
- Do not merge or opportunistically rebase from `main`.
- Preserve unrelated dirty state exactly; do not reset, stash, clean, stage, or
  commit it.
- Review committed checkpoints in a detached worktree, never Codex's mutable
  implementation worktree.
- Record transient checkpoint, review, and CI identifiers in the external
  handoff, not tracked project documents.

## Build ladder

Use the smallest level that answers the question:

```text
L0 — inspect/edit only
L1 — compile affected target(s)
L2 — focused new/changed tests
L3 — relevant regression filters
L4 — complete standard suite, only when implementation is stable
L5 — sanitizers, only when final code requires them
L6 — remote full CI, only near a code-complete checkpoint
```

Prefer persistent Ninja build directories. Avoid unnecessary full rebuilds,
reconfiguration, sanitizer reruns, cleaning, and remote pushes used only as
compile-error detectors. Documentation-only edits do not justify C++ builds
or sanitizer runs.

## CI run IDs

Never create a commit solely to update a tracked document from one “latest CI
run ID” to another. Workflow run IDs are transient and belong in the final
external handoff.
