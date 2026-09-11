# Milestone 32: Semantic Candidate-Assessment Resource

## Status and lineage

Milestone 32 is complete. The old ordinary `512` candidate-assessment ceiling
was shown to charge monotonic first-time candidate progress rather than an
independent unbounded source of work. Ordinary accounting is now semantic and
generation-aware. The former real frontier was crossed; execution stopped at
the pre-existing finite transaction boundary, which is the recommended M33
frontier.

- Required M31 base: `2d52f8e4896392d1083282ea09f2cf164c8c7bea`.
- Branch: `milestone-32-semantic-candidate-assessment-resource`.
- Parent branch: `milestone-31-semantic-transition-frontier`.
- Parent PR: `#36`.
- Implementation commit: `1e3dcaf`.
- Documentation commit: follows this implementation commit.
- Unrelated pre-existing `.gitignore`, `.DS_Store`, and `src/.DS_Store`
  changes remained uncommitted.

No proprietary input, private configuration, private report, host path, or
machine-specific data is committed.

## Clean baseline and M31 reproduction

At the required SHA, the complete standard suite was `333/333` with `25,628`
assertions. The ignored local four-module configuration was used without a
resource override or execution workaround:

```bash
./build/run-entry --local-config config/local.m17.local.json --entry dt-init \
  --report build/reports/m32-m31-reproduction.json
```

The ordinary M31 reproduction matched the required deterministic report:

| Measurement | Value |
| --- | ---: |
| schema / size | `17` / `40,965,888` bytes |
| SHA-256 | `da7ff24e0741f69159cee1866ea1aaee51d374d630ba0a6272aeb06b564c38f0` |
| stop | `indirect_target_refinement_budget_exceeded` |
| current/source PC | `main:0x7200000148` |
| instruction / opcode | `blr x8` / `0xd63f0100` |
| observed target | `main:0x7200135180` |
| target register / provenance | `x8` / guest load at `main:0x720456bb40` |
| guest instructions / blocks / IR | `18,427` / `3,703` / `109,272` |
| direct calls / indirect attempts / successful indirect entries | `468` / `508` / `507` |
| returns / non-call transfers / maximum depth | `975` / `65` / `3` |
| successful function entries | `1,041` |
| function-transition resource | `65/1,000` |
| execution attempts / productive rounds / stagnant rounds | `513` / `513` / `0` |
| immutable generation / promotions | `512` / `512` |
| candidate records / assessments / pending records | `513` / `512` / `1` |
| pending candidate | `main:0x7200135180` |
| reconsiderations / terminal resolutions / fallbacks | `0` / `0` / `0` |
| transactions | `511/512` |

Aggregate refinement was `1,092/200,000` functions analyzed, `6/100,000`
reanalyzed, `139,358` reused, `27,954/8,000,000` instructions,
`5,756/2,000,000` blocks, `8,146/4,000,000` edges,
`111,816/268,435,456` bytes, `512/2,048` boundary passes,
`3/100,000` invalidated records, and `511/512` transactions.

M30/M31 memory invariants were also reproduced: non-stack baseline and final
live mapped bytes `95,322,368`, peak live mapped `96,370,944`, peak live stacks
`1`, and stacks created/reclaimed/live at stop `513/513/0`.

## Candidate lifecycle audit

An observed indirect target passes through these production stages:

```text
observation and provenance merge
  -> executable/alignment/structural eligibility
  -> sparse (target_module, target) record admission
  -> pending selection
  -> one assessment in the current immutable generation
  -> static/CFG analysis, ownership, and boundary reconciliation
  -> immutable refinement transaction and publication
  -> promotion, generation advance, retry/dispatch
```

The worklist key is `(target_module, target)`. Provenance is evidence, not
identity, so duplicate observations merge into one sparse record. Trusted-existing
and terminal decisions resolve pending work without creating a candidate.

The worklist stores `processed_generation` and the last assessment generation.
A pending record cannot be assessed twice in one generation. A processed record
can become pending again only after an actual relevant immutable map-generation
change; that reconsideration is charged separately from first-time assessment.
The pending summary retains rollback witnesses even when selection excludes a
same-generation item.

The old global assessment charge was `begin_candidate_assessment()`. Candidate
records, observations, duplicate observations, promotions, terminal resolutions,
trusted-existing hits, reconsiderations, map rebuilds, generation, productive/
stagnant rounds, and transaction counts are now checked at each increment or
addition. Overflow fails closed with typed `counter_overflow`; there is no
wrapping or unlimited sentinel.

Failed certification and non-promoting analysis use the existing typed decision
path. Aggregate-resource exhaustion records a rollback, leaves the prior
published map unchanged, and preserves the pending candidate for a later
generation. Refinement still constructs a new immutable map before publication;
no provider, ownership, boundary, generation, or execution state is
half-published.

## Exact reconciliation of M31's 512 units

The `512` completed assessments reconcile exactly as follows:

| Semantic category | Count |
| --- | ---: |
| first assessment of newly admitted record | `512` |
| generation-dependent reassessment | `0` |
| trusted-existing resolution | `0` |
| terminal rejection/resolution | `0` |
| assessment ending before refinement | `0` |
| failed refinement / rollback | `0` |
| other | `0` |
| **total** | **`512`** |

All `512` ended in successful immutable promotion. There were `513` records:
the first `512` were assessed and promoted, and the naturally observed
`main:0x7200135180` record remained pending. No same-candidate,
same-generation duplicate and no reconsideration occurred.

`successful promotions = 512`, `immutable generation = 512`, and
`transactions = 511` have different meanings. The first refinement built the
initial module map without a reusable prior module map and therefore charged
zero refinement transactions. The remaining `511` published refinements reused
the prior map and charged one transaction each. Promotion and generation each
advance once per published candidate, so their equality is expected.

## Hypotheses and root cause

- The historical ceiling was a coarse proxy for finite first-time progress:
  proven. M31 had 512 first assessments, 512 promotions, no reassessments,
  and one additional admitted pending record.
- Independent retry/reconsideration work: not observed in M31 and rejected as
  an explanation for the frontier. The code now requires a changed relevant
  generation and prevents same-generation repeats.
- Duplicate observations/provenance manufacturing identities: rejected.
  Identity is the module/address pair and evidence merges into that sparse
  record.
- Rollback endlessly requeueing work: rejected. Rollback preserves the old
  map, records a typed rollback, and needs a later generation; aggregate
  resources remain the independent finite guard.
- A missing compensating bound elsewhere: rejected. Structural admission,
  immutable generation changes, aggregate refinement resources, and no-progress
  guards already provide the finite dimensions.

Thus ordinary first assessments are bounded by the executable process image's
structural candidate universe introduced by M28. Reassessment is separately
bounded by finite generation-changing refinement work and the one-assessment-
per-candidate-per-relevant-generation invariant. The implementation does not
materialize the candidate-by-generation Cartesian product.

## New model and compatibility

`max_candidate_assessments` is now an optional explicit legacy ceiling. With no
override, the report uses `structural_candidate_generation_v1`:

```text
finite sparse candidate records
× finite relevant immutable generation changes
× at most one assessment per record per relevant generation
```

The report schema is `18` and exposes typed accounting for first assessments,
reassessments, total assessments, same-generation attempts, map generation,
last/next candidate, assessment generation, structural bound/termination model,
and typed exhaustion. It does not serialize an unbounded history or a theoretical
Cartesian product.

Explicit CLI and local-JSON `max_candidate_assessments` values remain finite,
positive, and enforceable. Their provenance is reported as
`explicit_cli_override` or `local_configuration_override`; absent ordinary
configuration is reported as `not_configured`. An explicit value of `2` stops
after exactly two assessments with the pending third candidate as witness.

## Synthetic validation

The dedicated production-path suite is
`tests/milestone32_candidate_assessment_resource_tests.cpp` and contains ten
focused cases (`10/10`, `50` assertions). It exercises real target assessment,
CFG/refinement, immutable publication, and worklist state, including:

- `513` legitimate first assessments under ordinary defaults from a synthetic
  image with `514` structurally valid executable slots; no test raises the
  ordinary resource value;
- sparse record and duplicate-observation accounting;
- same-generation duplicate prevention and actual generation-dependent
  reconsideration;
- distinct first/reassessment counters and finite candidate cycles;
- terminal rejection and trusted-existing resolution;
- aggregate transaction rollback with logical preservation of the old map;
- explicit legacy ceiling exact boundary and provenance;
- deterministic report fields and ordering; and
- checked counter overflow.

The scaling test promotes `512` records, fully assesses the `513th`, then stops
at the independently unchanged transaction boundary (`512/512`), proving the
architectural change crosses the old assessment frontier rather than merely
using a larger magic count. Existing M27 aggregate-resource, M28 structural
universe, M29 resumable-IR, M30 stack, and M31 transition tests remain green.

## Former real frontier verification

The former pending candidate was naturally observed from:

```text
source: main:0x7200000148
instruction: blr x8 (0xd63f0100)
target: main:0x7200135180
x8 provenance: guest load at main:0x720456bb40
```

It was assessed through the normal structural and CFG path. Address validation,
executable ownership, relocation-backed pointer-slot verification, and unique
candidate evidence were true; relocation rebasing was not treated as function
metadata. CFG analysis produced a `new_entry` candidate and no boundary
reconciliation was required. The candidate was certified with high confidence,
promoted into immutable state, and entered as guest code. The immutable
generation changed `512 -> 513`; the first guest PC was `main:0x7200135180`,
with decoded opcode `0xa9be7bfd` at that address and next PC
`main:0x7200135184`. No target-specific production constant or seed was added.

## Final private execution and next frontier

The same ordinary local command was run twice with identical ignored inputs.
Both runs produced a schema-18 report of `40,986,349` bytes with SHA-256
`b6b79936fc864341778ebb21bd68a5a3eb0c1bac6c334d32a583b8c0f2cfb3de` and
stopped at the next target, `main:0x72001351d0` (x8 loaded at
`main:0x720456bb48`), for the same source `blr x8` at
`main:0x7200000148`. The exact report metrics are:

| Measurement | M32 value |
| --- | ---: |
| stop | `indirect_target_refinement_budget_exceeded` |
| guest instructions / blocks / IR | `18,441` / `3,707` / `109,365` |
| direct calls / indirect attempts / successful indirect entries | `468` / `509` / `508` |
| returns / non-call transfers / maximum depth | `976` / `65` / `3` |
| successful function entries / transition resource | `1,042` / `65/1,000` |
| execution attempts / productive / stagnant | `514` / `514` / `0` |
| observations / unique observations | `318,278` / `1,522` |
| candidate records / first / reassessments / total | `514` / `514` / `0` / `514` |
| promotions / map generation / pending / reconsiderations | `513` / `513` / `1` / `0` |
| failed / rollback assessments | `0` / `1` |
| aggregate transactions | `512/512` |
| runtime fallbacks | `0` |

Aggregate refinement was `1,094/200,000` functions analyzed, `6/100,000`
reanalyzed, `139,888` reused, `27,988/8,000,000` instructions,
`5,764/2,000,000` blocks, `8,156/4,000,000` edges,
`111,952/268,435,456` bytes, `513/2,048` boundary passes,
`3/100,000` invalidated records, and `512/512` transactions.

The next exact finite frontier is therefore `RefinementAnalysisTransactions`
at `512/512`, after the former target was assessed, certified, promoted, and
entered. This milestone deliberately does not raise, remove, or reinterpret
`max_transactions`. The prior immutable map remained unchanged when the next
assessment's aggregate reservation rolled back.

## Memory, determinism, and validation

M32 memory accounting remained healthy: non-stack baseline and final live mapped
bytes `95,322,368`, peak live mapped `96,370,944`, peak live stacks `1`, and
stacks created/reclaimed/live `514/514/0`.

Run A and Run B were byte-identical:

```text
Run A: 40,986,349 bytes; b6b79936fc864341778ebb21bd68a5a3eb0c1bac6c334d32a583b8c0f2cfb3de
Run B: 40,986,349 bytes; b6b79936fc864341778ebb21bd68a5a3eb0c1bac6c334d32a583b8c0f2cfb3de
cmp: identical
```

Validation completed locally:

| Environment | Result |
| --- | --- |
| standard CTest | `343/343` |
| M32 focused suite | `10/10` |
| M31/M30 regression suites | passed within `343/343` |
| GCC-configured build, serial CTest | `343/343` |
| ASan + UBSan | `343/343` |
| TSan | `343/343` |
| LLVM-configured build | `343/343`; no LLVM 18 backend installed (`LLVM_DIR-NOTFOUND`) |
| real LLVM 18 / MSVC | unavailable on this macOS host |

The parallel GCC/LLVM test attempt exposed only a pre-existing temporary-path
collision between simultaneous test processes; the GCC suite was rerun serially
and passed. No sanitizer suppression was added and no dependency was added.
GitHub CI for the stacked PR is the authoritative Linux/LLVM 18/MSVC coverage
when available.

## Privacy and M33 recommendation

The ignored local configuration and private reports remain outside commits.
Synthetic tests use only project-owned generated bytes. No Nintendo binaries,
ExeFS/XCI/NSP/NCA data, keys, extracted dumps, private paths, timestamps, PIDs,
or host pointers enter the repository or deterministic report schema.

Recommend Milestone 33 address only the measured
`RefinementAnalysisTransactions = 512/512` frontier, with its own forensic
termination analysis. No further resource is changed by M32.
