# TOTKRecomp agent operating contract

This file is persistent project context for future agent sessions. The
external Master Spec is authoritative for milestone scope, architecture,
invariants, stop conditions, and prohibited shortcuts. Pi must not silently
redesign those decisions.

## Execution model

Pi is the sole project agent. It owns the full milestone lifecycle: implement
the scoped change, perform focused builds and tests, run private real
validation when a milestone requires it, create committed checkpoints,
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

## Progress metric

The only progress KPI is the real frontier: the highest number of guest
instructions actually executed on real input, together with the exact next
`stop_reason`.

- Treat the static `unsupported instructions` counter as a scanner diagnostic.
  Never present it as a progress KPI, and never use its decrease as the
  headline result of a milestone.
- A milestone that only lowers `unsupported` while the real run stops at the
  same guest address, or earlier, has not advanced capability. Describe it as
  coverage or diagnostic work, never as a capability or frontier advance.
- State frontier before, frontier after, and the next `stop_reason` in every
  milestone report. If the frontier did not move, say that in the first line.
- Accept frontier movement only from a reproduced real run under the frozen
  measurement contract below. Synthetic lifts prove semantics, not frontier.

## Frozen measurement contract

Freeze the input before implementing, not after measuring.

Before a milestone changes code it must have a validated module-set manifest:

```text
module logical name, byte size, SHA-256, load order, recovery/version identity
expected frontier and expected next stop_reason for that exact input
```

- If the run does not reproduce the expected frontier, declare the milestone
  BLOCKED. Do not reinterpret the mismatch, and do not measure against a
  different, smaller, or newer input to obtain a better number.
- Re-derive and revalidate the manifest whenever the module set, load order, or
  recovery version changes. An unvalidated manifest is not an input.
- Keep the manifest and its hashes in the private runtime directory; commit
  only the non-proprietary schema and the validation outcome.

## Current blocker state

The historical provider/completeness blocker is resolved for the selected exact
four-module contract: the manifest-verified `rtld`, `main`, `subsdk0`, and `sdk`
set is coherent, and `__nnmusl_init_dso` resolves to guest `sdk`. That history
remains recorded in the earlier milestone evidence.

Current real execution stops later at the `sdk` TLS/bootstrap boundary: the
controlled run intentionally initializes `TPIDR_EL0` to synthetic zero, and the
guest subsequently reads `0x1f8` from that unmapped address. No faithful runtime
bootstrap/TLS evidence is available yet. Refusing to invent that state is
correct behaviour, not a defect to route around.

- Treat the missing faithful TLS/bootstrap evidence as the primary project
  bottleneck. It has priority over unreachable instruction-family convergence.
- Do not spend a milestone on another instruction family in order to avoid the
  blocker. A family that is unreachable from the current frontier is not
  capability work.
- Do not clear the blocker by asserting completeness, weakening the policy,
  adding a host stub, or inventing rtld/bootstrap/TLS state. Clear it only with
  faithful bootstrap evidence and a manifest-verified exact input contract.

## Time budget

Declare a wall-clock budget for every real run before starting it.

- Exceeding the declared budget is a blocking defect, not a note in the
  results section. Report it as a blocker and stop the attempt.
- Refinement consumes roughly 98% of wall time. It is the optimisation target;
  guest execution is not.
- Record budget, measured wall/user/system time, and the refinement share of
  wall time in the same section of the milestone report.

## Validation proportional to risk

Match validation cost to the semantic risk of the delta.

- Run sanitizers, TSan in particular (roughly 18 minutes), nightly or on demand
  for semantic and concurrency changes. Do not run them at every checkpoint.
- Run the byte-identical A/B pair of the real run exactly once, at the final
  checkpoint.
- Use the light lane for documentation-only deltas. They justify no C++ build,
  no sanitizer, and no real run.
- Keep focused L1/L2/L3 checks as the default per-edit loop.

## Architecture authority and no fake progress

Do not advance TOTK by forcing PCs, forcing indirect targets, inventing
providers, hardcoding register values, returning fake runtime success, or
arbitrarily increasing limits. Unsupported behavior remains an honest,
observable boundary until the governing specification authorizes the next
capability.

## Instruction-family fan-out

Keep one instruction family inside a bounded set of layers: decoder
normalization, coverage predicate, Semantic IR, verifier, interpreter, and
lowering.

- When the coverage record and the lifter predicate diverge for a family, fix
  the shared predicate. That divergence is a structural defect; do not reward
  it with more code.
- Do not increase the number of layers a family touches without a stated
  reason in the milestone document.

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
- Do not leave a branch whose name contradicts the milestone number it
  contains. Rebuild or rename the branch rather than documenting the mismatch.
- Do not merge or opportunistically rebase from `main`.
- Preserve unrelated dirty state exactly; do not reset, stash, clean, stage, or
  commit it.
- Use committed checkpoints as stable references for self-review and
  validation when needed; no second-agent review worktree is required.
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
compile-error detectors. Documentation-only edits do not justify C++ builds
or sanitizer runs.

## CI run IDs

Never create a commit solely to update a tracked document from one “latest CI
run ID” to another. Workflow run IDs are transient and belong in the final
external handoff.
