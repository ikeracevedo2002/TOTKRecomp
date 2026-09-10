# Milestone 33: Semantic Refinement-Transaction Resource

## Status and lineage

Milestone 33 resolves the historical `512` immutable refinement-transaction
frontier without increasing it. The transaction count remains exact diagnostic
accounting, but ordinary refinement no longer has a fixed transaction event
ceiling. The ordinary finite termination profile is the semantic aggregate
analysis ledger, including boundary-finalization work. A positive finite
transaction ceiling remains available when a caller explicitly requests the
old compatibility/debug guard.

- Required base SHA: `84435e205e9abe31d08d4b05b705bf879b50095d`.
- Stacked branch: `milestone-33-semantic-refinement-transaction-resource`.
- Parent branch: `milestone-32-semantic-candidate-assessment-resource`.
- Parent milestone PR: `#37`.
- No rebase, merge from `main`, or squash of prior milestone history was used.
- Pre-existing `.gitignore`, `.DS_Store`, and `src/.DS_Store` changes were
  preserved unmodified and were never staged.

No Nintendo content, executable-derived fixture, private report, private
configuration, absolute path, host pointer, timestamp, PID, or machine-local
metadata is committed.

## Required baseline and M32 reproduction

Before production edits, `HEAD` was verified as the required M32 SHA and the
working-tree status recorded the three unrelated machine-local changes above.
The existing build workflow passed at the exact base:

```text
343/343 CTest tests
30,385 assertions in 343 test cases
```

The ignored local four-module configuration was then run with no resource
override or execution workaround:

```bash
./build/run-entry --local-config config/local.m17.local.json --entry dt-init \
  --report build/reports/m33-m32-reproduction.json
```

The result was byte-identical to the M32 private local evidence report:

| Measurement | M32 reproduction |
| --- | ---: |
| schema / report size | `18` / `40,986,349` bytes |
| SHA-256 | `b6b79936fc864341778ebb21bd68a5a3eb0c1bac6c334d32a583b8c0f2cfb3de` |
| stop | `indirect_target_refinement_budget_exceeded` |
| source PC / instruction / opcode | `main:0x7200000148` / `blr x8` / `0xd63f0100` |
| pending target | `main:0x72001351d0` |
| pointer provenance | guest load at `main:0x720456bb48` |
| map generation | `513` |
| candidate assessments | `514` first, `0` generation reassessments |
| successful promotions | `513` |
| aggregate transactions | `512/512` |
| aggregate boundary passes | `513/2,048` |
| aggregate functions analyzed / reanalyzed / reused | `1,094/200,000` / `6/100,000` / `139,888` |
| aggregate instructions / blocks / edges | `27,988/8,000,000` / `5,764/2,000,000` / `8,156/4,000,000` |
| aggregate bytes / invalidated records | `111,952/268,435,456` / `3/100,000` |

The report comparison used a byte comparison and SHA-256; no private report or
configuration was copied into the repository.

## What an immutable refinement transaction means

The production charging site is `FunctionMapBuilder::build` in
`src/switchrecomp/analysis/function_map.cpp`. The builder sets
`accounting.refinement_transactions = 1` exactly when `options.reuse_map` is
present. That option means that a frozen, identity- and layout-matching prior
module map is copied into speculative candidate state for a new immutable
map-generation build. It is not a count of observations, assessments,
promotions, module maps carried forward, or guest dispatch entries.

The complete production lifecycle is:

```text
guest indirect observation and provenance
  -> structural assessment and certification
  -> sparse candidate admission and deterministic worklist selection
  -> one assessment in the relevant immutable generation
  -> speculative function/process-map build
  -> ownership, CFG, boundary, provider, and certification validation
  -> aggregate reservation
  -> immutable map publication and generation advance
  -> dispatch, or rollback with the old map retained
```

The path has these transaction variants:

1. Initial module-map construction has no reusable prior map, so it charges
   zero transactions.
2. Single-module `refine_function_map` supplies the published frozen map as
   `reuse_map`, so a successful qualifying refinement charges exactly one.
3. Process-level refinement rebuilds only the target module. An existing
   target module map is supplied as `reuse_map`, while unchanged module maps
   are carried forward as frozen values and do not create reconstruction
   transactions. If the target module has no previous map, a new target map is
   constructed without reuse and charges zero transaction units.
4. Invalidation/reanalysis and no-invalidation reusable refinements both use
   the same reusable-map build and therefore both charge one transaction.
   Cross-module ownership and boundary reconciliation affect certification and
   which target map is rebuilt; they do not create a second transaction for
   carried-forward maps.

After the reusable-map assignment, `FunctionMapBuilder::build` always enters
its boundary-finalization loop. Nonzero budget validation makes at least one
pass possible, and the loop increments `boundary_finalization_passes` before
checking whether the first pass reached a fixed point. A successful build
therefore has at least one boundary-finalization pass, including a new target
module build and a reusable build. The initial map also has a pass but has no
transaction charge.

This is a control-flow property of every successful builder path, not an
inference from the TOTK counters. The only transaction assignment is the
reusable-map assignment; the only successful return after map construction is
after the finalization loop and frozen-map validation.

## Forensic conclusion and termination proof

Let `T_i` be the transaction count of successful refinement build `i`, and
`P_i` its boundary-finalization-pass count. For every successful build:

```text
T_i = 1 if and only if a reusable prior map is supplied
T_i = 0 otherwise
P_i >= 1
```

Consequently, for every reachable prefix of successful builds:

```text
cumulative_transactions = sum(T_i)
  <= sum(P_i) = cumulative_boundary_finalization_passes
```

The transaction dimension is therefore a diagnostic projection of already
finite semantic work, not an independent finite-work dimension. Reuse can
avoid reanalyzing many records, but it cannot publish a successful immutable
refinement without running boundary finalization. This remains true for
newly analyzed functions, reused functions, invalidated records, reanalysis,
no-invalidation refinement, module-map reconstruction, and cross-module
refinement.

Ordinary productive termination follows from the combined M28/M32/M27 model:

- the structural candidate universe is finite and derived from checked aligned
  executable instruction slots;
- each candidate is assessed at most once in one relevant immutable map
  generation;
- each successful publication advances the immutable generation;
- each successful refinement publication performs boundary-finalization work,
  and all aggregate semantic dimensions have finite configured limits;
- newly introduced module maps are also bounded by their function,
  instruction, block, edge, byte, and boundary work even though their
  transaction count is zero;
- explicit transaction compatibility mode adds one additional finite check,
  never an unlimited sentinel.

Thus only finitely many candidate/generation assessment pairs can be reached.
Non-productive execution is separately bounded by the finite stagnant-round
resource. Duplicate observations coalesce into existing sparse records, and a
same-generation duplicate assessment is rejected. A failed aggregate
reservation records typed exhaustion, does not publish the speculative map,
does not advance generation, does not mark the candidate successfully
processed, and leaves the candidate pending as the deterministic witness.
The exhaustion state and the candidate's last-assessed generation prevent a
zero-cost retry loop. No partial aggregate counter or execution state is
published.

## Architectural change and compatibility

`IndirectTargetRefinementAnalysisBudgets::max_transactions` is now an
`optional<size_t>`:

- `nullopt` is the ordinary deterministic state: transaction accounting is
  enabled, but no ordinary transaction ceiling is configured;
- an engaged positive finite value is an explicit compatibility/debug ceiling;
- the existing CLI option remains
  `--refinement-max-analysis-transactions N`;
- local JSON remains
  `{"refinement_analysis":{"max_transactions":N}}`, including the existing
  nested `budgets` form;
- CLI configuration takes precedence over local configuration;
- zero, overflowed, non-integer, and unrepresentable CLI/local values are
  rejected;
- provenance is stable (`explicit_cli_override`,
  `local_configuration_override`, or `not_configured`).

`SIZE_MAX` is not used as an unlimited marker. Transaction accounting remains
checked, including aggregate addition, generation addition, byte addition,
module-map counters, and the transaction counter. A failed checked addition
sets typed `counter_overflow` exhaustion and leaves the published state
unchanged. The aggregate reservation continues to check every semantic
dimension atomically before the caller assigns the speculative process map.

Schema version is `19`. Reports retain aggregate `transactions` consumption
and add a typed `transaction_resource` object containing:

- ordinary termination mode;
- whether a ceiling is configured;
- configured limit or JSON `null`;
- limit provenance;
- transactions consumed;
- the dominating resource and invariant;
- explicit transaction compatibility exhaustion, if any; and
- counter-overflow failure, if any.

The human diagnostic for an explicit transaction rejection includes the exact
dimension, consumed count, limit, module, generation, pending count, and
deterministic next candidate when present.

## Synthetic validation

`tests/milestone33_refinement_transaction_resource_tests.cpp` is registered in
the normal test target. It contains five production-path cases and passed as
`6,250` assertions in `5` test cases:

1. A real synthetic executable image with `514` executable slots performs
   `513` real structural assessments, CFG/function-map refinements, immutable
   publications, and reusable transactions with ordinary defaults. It crosses
   the historical `512` transaction boundary without changing the boundary
   resource.
2. Initial construction, disjoint no-invalidation reuse, and a branch-boundary
   invalidation/reanalysis build verify initial `0` versus reusable `1`
   transaction accounting and assert `transactions <= boundary passes`.
3. An explicit ceiling of `2` commits exactly two real refinements. The third
   speculative map fails at `RefinementAnalysisTransactions` with consumed
   `2`, limit `2`, generation `2`, the exact module, and the exact pending
   candidate. The old map fingerprint and generation remain unchanged; the
   third map exists only in speculative state. Same-generation retry is
   rejected.
4. Ordinary and explicit reports verify schema 19, `null`/`not_configured`,
   explicit finite configuration, the dominating resource, and the formal
   invariant.
5. A maximum-representable transaction increment is committed once and a
   second increment is rejected at the exact maximum without wrapping.

The full post-change suite is `348/348` tests and `36,635` assertions. The M27,
M28, M31, and M32 suites remain green; the historical M32 transaction-boundary
test explicitly enables `max_transactions=512` to retain its compatibility
assertion, while the dedicated M33 ordinary test proves the architectural
change.

## Final real execution

The ordinary ignored four-module workflow was run twice with no resource
override. Both reports were exactly `41,233,243` bytes with SHA-256
`4b9ef350e2c056d3af2dfb3a6cdd2e95249852508f72575be83ec39ffaf5ddf0`, and
`cmp` returned zero. Both runs used schema `19`.

The former M32 candidate was naturally crossed:

| Measurement | Result for `main:0x72001351d0` |
| --- | --- |
| source / target provenance | `main:0x7200000148`, guest load at `main:0x720456bb48` |
| certification | certified, high confidence |
| certification evidence | observed indirect call, verified rebased guest slot, bounded CFG |
| decision / promotion | trusted new entry, eligible, promoted |
| map generation | `513 -> 514` |
| guest entry | yes, through normal dispatch |
| first instruction / opcode | `stp x29, x30, [sp, #-0x10]!` / `0xa9be7bfd` |
| next PC | `main:0x72001351d4` |

No PC, target, seed, provider, relocation, branch, return value, or register
was forced. The existing certification and evidence rules were unchanged.

The former transaction gate did not fail. The final ordinary run performed one
initial map publication, `527` successful reusable-map transactions, and no
failed transaction reservations or rollback transactions. The report's
`528` successful promotions and `528` map generations reconcile as initial
map plus `527` reusable refinements. Aggregate transaction consumption was
`527`, while boundary-finalization consumption was `528`, satisfying the
invariant.

The next natural typed frontier was not a guessed instruction frontier. It was
the pre-existing execution event resource:

```text
stop: event_limit_exceeded
events: 4096/4096
execution slices: 2048
```

The stop has no guest source PC or target because the event budget, rather than
an instruction certification or refinement failure, ended the run. This is the
exclusive M34 measurement from this milestone.

### Final report evidence

| Category | Consumption / result |
| --- | --- |
| stop | `event_limit_exceeded`, `stop_pc=0x0`, diagnostic `execution event limit exhausted` |
| guest instructions / blocks / IR operations | `18,647` / `3,765` / `110,727` |
| direct / indirect calls | `468` / `523` |
| returns / function transfers | `991` / `65` |
| maximum call depth | `3` |
| entries / transition charge | `1,057` successful entries / `65/1,000` charged non-call transfers |
| attempts / productive / stagnant rounds | `529` / `528` / `1` |
| observations / unique observations / candidates | `332,971` / `1,550` / `528` |
| assessments: total / first / generation reassessments | `528` / `528` / `0` |
| same-generation assessment attempts | `0` |
| promotions / map generation | `528` / `528` |
| pending / failed / rollback / terminal resolutions | `0` / `0` / `0` / `0` |
| trusted-existing hits / coalesced duplicates | `332,443` / `331,421` |
| aggregate functions analyzed / reanalyzed / reused | `1,124/200,000` / `6/100,000` / `147,958` |
| aggregate instructions / blocks / edges | `28,498/8,000,000` / `5,884/2,000,000` / `8,306/4,000,000` |
| aggregate bytes / boundary passes / invalidated records | `113,992/268,435,456` / `528/2,048` / `3/100,000` |
| aggregate transactions | `527`, ordinary limit `null` |
| runtime fallbacks | `0` |
| guest-code entry | `true` |

Guest-memory and stack accounting remained bounded and deterministic:

```text
controlled stack mappings created/reclaimed/live: 529/529/0
cumulative controlled-stack bytes: 554,696,704
cumulative mapped bytes: 650,019,072
peak live mapped bytes / final live mapped bytes: 96,370,944 / 95,322,368
peak controlled-stack bytes: 1,048,576
peak live controlled stacks: 1
live regions: 16; peak regions: 17; call-stack snapshot: 0
```

The exact next frontier is intentionally left unchanged for M34. M33 does not
raise the event budget, alter boundary-finalization limits, add an instruction,
change function transitions, or change stack lifetime behavior.

## Validation and reproducibility

Local validation completed:

- complete standard suite: `348/348`, `36,635` assertions;
- focused M33 suite: `5/5`, `6,250` assertions;
- M27/M28/M31/M32 regression filters: green;
- ordinary real run twice: byte-identical and SHA-identical;
- `git diff --check`: clean.

The local default compiler is AppleClang 17 on macOS; `/usr/bin/gcc` is the
AppleClang driver, not GNU GCC. LLVM 18 development tools are not installed on
this host, and MSVC is not available on macOS. The repository's GitHub Actions
run `34478394571` passed all five authoritative jobs: Linux/GCC, Linux/GCC /
LLVM 18, Linux/GCC / ASan + UBSan, Linux/GCC / TSan, and Windows/MSVC. The
workflow emitted only the existing GitHub Actions Node.js 20 deprecation
annotation; no job failed. No sanitizer suppression or dependency was added.

The reports are deterministic JSON: no timestamps, host addresses, PIDs,
unordered iteration, or unbounded event history was introduced. The ignored
local executable set and configuration remain outside version control.

## Recommendation for Milestone 34

Address only the measured next frontier: the existing execution event resource
at `4096/4096`. First audit its event categories and semantic work, then make
the smallest proof-preserving change supported by that audit. Do not infer a
new instruction or raise an unrelated refinement resource from the M33 run.
