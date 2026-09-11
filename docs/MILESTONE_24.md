# Milestone 24: Bounded Execution-Closure Analysis

## Status

M24 is **Complete** for the generic implementation and the exact private
four-module gate. The execution tool now separates finite execution-closure
analysis from whole-module discovery, reaches guest execution, naturally
reaches the former M22 frontier, and continues until the next typed semantic
frontier.

## M23 failure and measured cause

The M23 execution command used one effective whole-module envelope of 5,000
functions, 200,000 instructions, 50,000 blocks, 100,000 edges, 10,000 seeds,
and 16 MiB of analyzed bytes. The exact private diagnostic stopped while
initially seeding `sdk` at 5,000/5,000 functions, with 5,000 pending entries
and a deterministic next seed. No CFG work or indirect-target certification
was rejected by that stop.

The cause is broad initial fan-out combined with whole-module CFG work. The
private `sdk` dynamic table contains 24,098 defined `FUNC` symbols before
relocation-derived evidence is counted; that one source already exceeds the
old 5,000-function limit. A comparable public local whole-module run measured
31 initial seeds expanding to 613 canonical functions through 1,596 direct-call
discoveries, with 36,393 decoded instructions and 188,191 conflict-pair
processing steps. This establishes why a small execution frontier could still
pay for a large module-wide analysis.

## Analysis model

`run-entry` starts with the generic finite `execution_closure` profile. It
retains only seeds required by the selected verified entry, selected focus
provider entries, and trusted startup entries. Direct guest calls expand the
closure transitively. Runtime indirect targets are added only through the
existing M20 certification and M23 immutable refinement path. Whole-module
analysis remains available explicitly for translation and diagnostics.

Every effective budget is carried in one typed `AnalysisBudgets` value. Each
dimension records whether it came from the library default, execution-tool
profile, explicit CLI override, local configuration, or a checked structural
bound. The builder records deterministic per-module accounting for seed
provenance, duplicate coalescing, excluded candidates, canonical functions,
CFG analysis, direct-call expansion, graph consumption, ownership and conflict
processing, boundary finalization, immutable reconstruction, pending work, and
typed exhaustion.

Structural ceilings are derived from executable AArch64 instruction capacity.
Repeated CFG accounting is bounded by the finite boundary-finalization pass
count. These ceilings are safety bounds, not targets, and are never represented
as unlimited or disabled values.

Provider resolution remains process-wide and completeness-gated. Closure
pruning does not remove provider evidence: symbol/provider indexing happens
before function-map construction. An incomplete or ambiguous provider search
continues to fail closed. If an observed target belongs to a module omitted
from the initial closure, refinement constructs a new frozen map for that
module before promotion is accepted.

Finalized maps remain immutable. Duplicate evidence is retained while
equivalent canonical work is coalesced. Ownership conflicts, callable-entry
trust, and indirect certification rules are unchanged. No analysis cache or
in-place map mutation was introduced.

## Synthetic validation

The M24 test file covers finite profile and structural-bound provenance,
duplicate/provenance coalescing, transitive direct-call closure, irrelevant
candidate pruning, multi-module closure construction, exact typed function/
instruction/block/edge/byte/seed exhaustion, complete and incomplete provider
search, ambiguous provider rejection, and seed-order determinism. Existing
M18, M20, M22, and M23 tests remain green. The execution report schema is now
11 because the deterministic analysis summary and map-generation evidence are
externally visible.

## Private real-run evidence

Under the exact local four-module configuration, execution-closure analysis
used only the `main` and `sdk` maps initially required by the path. The final
controlled run recorded 50 canonical functions, 100 CFG analyses, 6,116
instructions, 1,153 blocks, 1,737 edges, and 24,464 analyzed bytes across
those maps. No analysis budget exhausted.

The historical indirect frontier was reached naturally at source
`main:0x7200000148` (`blr x8`). The target
`main:0x7200034bd0` came from a guest load at
`main:0x720456ab88`, with verified relocation evidence, and was promoted by
the existing certification policy. Its bounded candidate was five blocks,
26 instructions, and seven edges. The promotion changed the immutable map
generation from 9 to 10 and was the tenth successful promotion; the final run
recorded 21 promotions and 21 immutable map rebuilds.

Execution continued after that frontier to 1,787 guest instructions, 1,397
beyond the historical M22 total of 390. It reached 3 maximum call depth, zero
runtime fallbacks, 22 refinement rounds, 21 unique indirect candidates, 526
observations, 21 assessments that were candidates for refinement, and 21
promotions. The next genuine blocker was an unsupported instruction at
`main:0x72000ea7f8`, reported as the legacy interpreter's Milestone 9 opcode
boundary. `UMULH` was naturally reached at `main:0x72000001a4`; no jump or
address-specific seed was used.

Two runs from the same final source and private configuration produced
byte-identical deterministic reports. Host timings and private filesystem
locations are not report identity fields.

## Scope discipline

No guest runtime/bootstrap behavior, import fabrication, service emulation,
hardcoded target address, target-specific pruning, or speculative semantics
were added. Private binaries, local configuration, private paths, and private
reports remain outside Git.
