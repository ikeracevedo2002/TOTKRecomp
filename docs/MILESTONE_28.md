# Milestone 28: Structural Candidate Cardinality

## Status

M28 removes the ordinary fixed `256` indirect-candidate target ceiling. It
does so by deriving a finite target universe from the immutable executable
process image, keeping only observed target records, and retaining the M27
certification, immutable-generation, transactional, and aggregate-work
guards. It does not force execution, seed a private target, inject a PC, add a
private-address rule, or special-case TOTK.

## M27 baseline and reproduction

M28 is stacked directly on M27 commit
`4e0e9f9a2a42e66bc801db717ec3045910159fd6`, with no rebase onto `main` or
another milestone. The pre-edit branch was
`milestone-27-aggregate-refinement-resource`, and the ordinary clean-code
baseline was `287/287` tests.

The unchanged M27 private four-module run used the existing ignored local
configuration and ordinary defaults. It reproduced:

- report size: `35,114,732` bytes;
- SHA-256: `9ba3e72c42b10fd5a8184377d371ae9cf993b09b306b31b5f4ac7947ef1864fe`;
- guest instructions / blocks: `14,843 / 2,679`;
- function transitions / maximum call depth: `65 / 3`;
- direct calls / indirect calls / returns: `468 / 252 / 719`;
- runtime fallbacks: `0`;
- execution attempts / productive rounds / stagnant rounds: `257 / 256 / 1`;
- unique observations / candidates / assessments: `1,008 / 256 / 256`;
- terminal resolutions / successful promotions: `0 / 256`;
- immutable generations: `256`;
- module maps rebuilt / reused: `256 / 256`;
- reconsiderations / pending work: `0 / 1`.

The aggregate M27 ledger was `580` newly analyzed functions, `6`
reanalyzed functions, `19,250` instructions, `3,708` blocks, `5,586` edges,
`77,000` bytes, `256` boundary passes, `3` invalidated records, `255`
transactions, and `36,574` reused functions.

The exact former frontier was:

- source: `main:0x7200000148`, an existing `blr x8`;
- target: `main:0x7200130180`;
- pointer provenance: guest load at `main:0x720456b340`;
- state: naturally observed and pending after M27 stopped at
  `unique_candidates = 256/256`, with one pending candidate.

## Candidate-cardinality forensic analysis

The old ordinary default was declared as
`IndirectTargetRefinementBudgets::max_unique_candidates = 256`. In
`IndirectTargetRefinementWorklist::observe`, a non-trusted observation first
formed the target identity `(target_module, target)`. The first such target
charged `unique_candidates`; once the counter reached `256`, the new identity
was retained as overflow pending and no candidate record was admitted.

The full `IndirectTargetCandidateIdentity` also contains source module,
source function, source PC, control-flow kind, target register, pointer
provenance, and optional guest-load address. M27 stored one work item per that
full provenance identity but charged the cardinality quota per target
module/address. That was finite for the quota but did not make the persistent
record representation itself match the quota.

An admitted record moves through observation, pending, assessment, terminal
rejection or trusted-existing resolution, immutable promotion, and possible
generation-dependent reconsideration. Observation counts, evidence, and
guest-side provenance are merged deterministically. A failed assessment or
refinement never publishes a partial map. Before M28, the first 256 naturally
discovered targets were promoted and the 257th target remained pending solely
because of the historical count.

## M28 candidate scaling model

The ordinary default now has no configured candidate-count constant. The
worklist receives a `candidate_universe` derived from the loaded immutable
process image. For each uniquely named module it counts the aligned four-byte
instruction slots covered by that module's executable ranges, after checked
range-end arithmetic and deterministic range normalization. The per-module
counts are summed because the candidate cardinality identity includes module
name and target address. Zero-sized ranges contribute zero. Overflow,
duplicate module identities, and unrepresentable slot totals fail closed with
typed errors.

The measured private process-image bound was `13,578,588` executable guest
instruction slots. The ordinary effective target-record bound was therefore
`13,578,588`, not a replacement magic count. The representation is sparse:
the implementation stores one ordered work item per observed target
module/address pair and retains the additional source/provenance identities in
the representative observation and provenance history. It never allocates a
bitmap or record for every theoretical slot.

Structural eligibility is determined before persistent candidate admission.
The target must be nonzero, aligned, mapped for a complete four-byte fetch,
executable, and uniquely owned by an immutable process module (or an
equivalent finalized map in the library-only path). Impossible or
non-executable observations remain in the execution assessment with their
provenance and typed decision, but do not consume candidate records or the
structural slot count. This is only a storage/admission classification; it is
not function-entry evidence and cannot authorize dispatch.

Candidate-specific report accounting distinguishes:

- `structural_universe_limit`: the derived executable-slot bound;
- `effective_unique_candidate_limit`: the structural bound or the minimum of
  it and an explicit legacy guard;
- `unique_candidates`: admitted target-module/address records;
- `candidate_records`: the actual sparse record count;
- `structurally_ineligible_candidates`: unique impossible observations that
  were retained only as typed assessment evidence;
- duplicate observations and full provenance history.

No separate candidate-record quota is necessary because the persistent record
key is now exactly the target module/address key whose finite universe is
derived above. Candidate assessment work remains separately bounded by the
existing finite `max_candidate_assessments` ledger, and M27 aggregate CFG work
and transaction resources remain unchanged.

## Legacy compatibility

The public `--refinement-max-candidates` option remains available, and local
refinement configuration may use `max_unique_candidates`. It is now an
optional explicit legacy ceiling. When supplied, it is reported with its
provenance and is intentionally enforced; a small value produces typed
`unique_candidates` exhaustion with the new identity preserved as pending.
When absent, ordinary operation uses only the derived structural universe.
The old `max_promotions` and `max_map_rebuilds` compatibility behavior from
M27 is unchanged.

## Certification and transactional invariants

Indirect-target certification is unchanged. A candidate still requires the
existing checked executable-memory and alignment gates, unique ownership,
bounded instruction decoding and CFG analysis, boundary reconciliation,
provider/relocation evidence handling, conflict analysis, and immutable
function-map publication. A structurally possible address is not a trusted
function entry.

Refinement continues to build a new frozen map from the prior immutable
generation and publishes it only after all validation and resource checks
succeed. On candidate exhaustion, failed certification, aggregate exhaustion,
boundary conflict, verifier failure, or map-build failure, the prior
generation, ownership, provider state, dispatch state, and analysis cache
remain unchanged. The focused transaction test exercises a candidate ceiling
after a successful immutable promotion and confirms that the published map is
unchanged when the next candidate is rejected at admission.

## Termination argument

The exact persistent candidate identity is the ordered pair
`(target_module, target)`. In ordinary process execution, a structurally
eligible target must occupy one aligned four-byte slot in one immutable
executable module range. The checked structural universe therefore bounds the
number of candidate records and unique candidate admissions.

The narrower source/control-flow/register/pointer identity is evidence inside
that target record, not a second unbounded candidate-record namespace.
Duplicate observations are recognized by stable guest-side identity and
coalesced into the record's deterministic provenance history. No observation
can create a new target record after the structural universe is exhausted.

For each admitted record, the state transition is monotonic within a
generation: pending becomes promoted, trusted-existing, or terminally
rejected. A promoted record may be reconsidered only after a later immutable
map generation changes the assessment-relevant ownership state. Each such
reconsideration is charged and bounded by the existing finite assessment and
aggregate transaction/work dimensions. Candidate metadata is sparse and is
bounded by the same structural record count; actual CFG/reanalysis work is
bounded by the M27 checked aggregate ledger. Execution attempts that make no
monotonic transition consume the finite stagnant-round allowance. Every
counter increment and structural slot sum uses checked arithmetic or explicit
overflow rejection. Failed operations return before publication, so rollback
cannot create an unaccounted retry path.

## Synthetic validation

The focused M28 suite contains nine test cases and passed `2,786`
assertions:

- a synthetic executable image with `300` aligned slots admitted `299`
  distinct candidates and completed `299` certified immutable promotions under
  unchanged ordinary aggregate defaults, demonstrating both lifecycle work
  and more than 256 successful promotions through the production assessment
  and refinement machinery;
- one/multiple-module, segment-boundary, alignment, zero-sized-range,
  duplicate-module, and checked-overflow structural-bound cases;
- exact explicit legacy ceiling exhaustion at `2/2`, with no off-by-one
  admission and pending overflow work preserved;
- duplicate target observations with different source/provenance identities
  coalesced into one sparse record while history remained available;
- unmapped, non-executable, misaligned, and executable-segment-end targets
  retained as typed ineligible assessments without candidate records;
- deterministic non-sorted discovery order and sparse record counters;
- candidate exhaustion after a successful immutable promotion with the prior
  frozen map unchanged.

The focused run also covers the old M18/M20 certification path indirectly
through the real CFG/refinement case; the complete regression suite continues
to run the direct M18, M20, M22, M23, M24, M25, M26, and M27 tests.

## Private execution result

The first ordinary private M28 run reached beyond the former frontier. The
former target was processed normally:

- source: `main:0x7200000148`, `blr x8`;
- target: `main:0x7200130180`;
- pointer provenance: guest load at `main:0x720456b340`;
- assessment: structurally eligible and assessed;
- certification: `certified`, high confidence, status `validated` CFG;
- promotion: successful immutable promotion, map generation `256 -> 257`;
- guest entry: yes, through normal dispatch;
- first decoded instruction: `stp x29, x30, [sp, #-0x20]!` at
  `main:0x7200130180`;
- next guest PC: `main:0x7200130184`;
- forced jump, seed, PC override, and budget override: none;
- exact pointer-slot provenance remained the guest load at
  `main:0x720456b340`; runtime fallbacks remained zero.

With ordinary defaults, execution then reached a different existing global
execution guard rather than candidate cardinality:

- blocker: `ir_operation_limit_exceeded`;
- consumed / allowed: `100,000 / 100,000` IR operations;
- source/current function: `main:0x72000c353c`;
- stopping instruction: `stp x9, x1, [x0]` at `main:0x72000c3550`;
- target/pointer provenance: not applicable to this ordinary instruction stop;
- candidate state: no pending candidate, no candidate exhaustion;
- execution attempt / refinement round: attempt `414`, after `413`
  productive rounds and one stagnant round;
- immutable generation: `413`.

This is the first observed next blocker after candidate scaling. M28 does not
raise or redesign the IR-operation budget.

## Real-run measurements

The final private run measured:

| Metric | M28 |
| --- | ---: |
| guest instructions | 17,030 |
| guest blocks | 3,303 |
| function transitions | 65 |
| maximum call depth | 3 |
| direct calls | 468 |
| indirect calls | 408 |
| returns | 874 |
| runtime fallbacks | 0 |
| execution attempts | 414 |
| productive rounds | 413 |
| stagnant rounds | 1 |
| unique observations | 1,320 |
| unique candidates | 413 |
| candidate assessments | 413 |
| terminal resolutions | 0 |
| successful promotions | 413 |
| immutable generations / map rebuilds | 413 / 413 |
| module maps rebuilt / reused | 413 / 413 |
| reconsiderations | 0 |
| pending work | 0 |

The M27 aggregate dimensions measured:

| Dimension | Consumed | Limit |
| --- | ---: | ---: |
| newly analyzed functions | 894 | 200,000 |
| reanalyzed functions | 6 | 100,000 |
| instructions | 24,588 | 8,000,000 |
| blocks | 4,964 | 2,000,000 |
| edges | 7,156 | 4,000,000 |
| bytes | 98,352 | 268,435,456 |
| boundary passes | 413 | 2,048 |
| invalidated records | 3 | 100,000 |
| transactions | 412 | 512 |
| reused functions | 91,838 | not a charge limit |

M28 candidate accounting was `13,578,588` structural target slots,
`413` sparse candidate records/unique target candidates,
`1,320` unique observations, `216,766` duplicate-coalesced observations,
`218,086` total observations received, and zero structurally ineligible
observations in this private run. The legacy candidate ceiling was absent.

## New exact frontier

The exact next frontier is an existing execution-resource boundary, not an
indirect candidate:

- source/current function: `main:0x72000c353c`;
- source instruction / PC: `stp x9, x1, [x0]` at `main:0x72000c3550`;
- target: none;
- pointer provenance: none;
- candidate state / assessment / certification / promotion / guest entry: not
  applicable; all observed indirect candidates were processed and no work was
  pending;
- blocker: `ir_operation_limit_exceeded`;
- resource: `ir_operations = 100,000/100,000`;
- pending work: `0`;
- execution attempt: `414`;
- completed refinement rounds: `413` productive and `1` stagnant;
- immutable generation: `413`;
- classification: resource-related global execution guard.

The report does not claim execution closure or game completion. The next
milestone should begin from this measured IR-operation frontier only.

## Determinism

Two fresh ordinary private runs from the final source were compared byte for
byte. No report normalization or post-processing was used:

- run 1: `37,643,135` bytes,
  `c6cd5e02cb1dbf2e96f7aadc247622993fd5efd77009ab52e22622b3eb4fda18`;
- run 2: `37,643,135` bytes,
  `c6cd5e02cb1dbf2e96f7aadc247622993fd5efd77009ab52e22622b3eb4fda18`;
- comparison: byte-for-byte identical.

## Validation and privacy

The final standard suite passed `296/296` tests. The focused M28 run passed
`2,786` assertions in nine test cases. ASan/UBSan passed `296/296`, TSan
passed `296/296`, and the repository's GCC-configured local build passed
`296/296` (the host's `/usr/bin/g++` resolves to Apple Clang 17). LLVM 18
and Windows/MSVC were not locally available on this macOS host, so those CI
targets were not claimed as local passes. The final GitHub Actions matrix
then passed Linux/GCC, Linux/GCC/LLVM 18, Linux/GCC/ASan+UBSan,
Linux/GCC/TSan, and Windows/MSVC. No sanitizer configuration was weakened
and no new dependency was added.

The private executable set, local configuration, generated reports, private
addresses, and machine paths remain ignored workspace evidence. No NSO,
ExeFS, XCI/NSP/NCA content, key, title key, dump, private report, or
machine-specific configuration is committed. Synthetic tests use only
project-owned generated bytes.

## Known limitations

- The structural bound is a target-module/address bound, while the full
  observation identity remains reportable provenance inside each record.
- Candidate assessment work, aggregate analysis work, transaction work, and
  global execution budgets remain independent finite guards.
- The private run stopped at the unchanged IR-operation budget before any
  claim of complete execution closure could be made.
- The library-only architectural fallback universe is finite but conservative
  when no process image is supplied; the ordinary process path always uses the
  exact loaded-image derivation.

## Recommended Milestone 29

Investigate only the measured `main:0x72000c3550` stop caused by the existing
`ir_operations = 100,000/100,000` execution guard. Do not raise that budget
speculatively; first determine whether the instruction is a genuine execution
semantic frontier or whether the global IR ledger needs a proof-preserving
resource model.
