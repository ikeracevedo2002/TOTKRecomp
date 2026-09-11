# Milestone 31: Semantic function-transition frontier

## 1. Status

Complete. The investigation reproduced the M30 frontier, reconciled every
charged unit, and established that the old resource charged legitimate call
entries together with true function transfers. The general correction keeps
the default value `1,000`, but charges that resource only for successful
non-call function transfers; normal calls remain governed by their explicit
call-stack and execution resources.

## 2. Repository state

- Required base SHA: `5eddb61f4d1caec43168168aa98861925a3a10e4`.
- Stacked branch: `milestone-31-semantic-transition-frontier`.
- Parent branch: `milestone-30-generation-scoped-stack-memory`.
- Final production implementation commit: `4402c9f` (`feat: add semantic
  function-transition accounting`). The documentation commit follows it; the
  final branch tip is reported by the M31 handoff and PR.
- Unrelated pre-existing `.gitignore`, `.DS_Store`, and `src/.DS_Store`
  changes were preserved and were not included in M31 commits.

The clean base built successfully and its complete standard suite was
`317/317` before M31 files were added.

## 3. M30 reproduction

Before changing production behavior, the ordinary private four-module
configuration was run with the exact M30 command profile: the private local
configuration selected `dt-init`, with no execution override, forced PC or
entry, target seed, indirect-target seed, guest-memory patch, instruction
skip, fake return, raised resource, or IR hard-limit workaround.

The schema-16 M30 report reproduced byte-for-byte:

| Measurement | Value |
| --- | ---: |
| report size | `38,587,538` bytes |
| SHA-256 | `79367631238e1c4243012e28141d5cf519067c8067b69588dd58a9aa8f89e144` |
| stop | `function_transition_limit_exceeded` |
| stop/current PC | `main:0x72000c353c` |
| transition resource | `1,000/1,000` |
| attempts/generation | `473/472` |
| guest instructions | `17,850` |
| guest blocks | `3,538` |
| IR operations | `105,451` |
| slices/yields/resumes/mid-block resumes | `1,934/1/1/1` |
| max call depth | `3` |
| direct/indirect calls | `468/467` |
| returns | `933` |
| function transfers | `65` |
| runtime fallbacks | `0` |
| productive/stagnant rounds | `472/1` |
| observations/candidates/assessments/promotions | `1,438/472/472/472` |
| pending refinement work | `0` |

M30 memory invariants in this reproduction were healthy: non-stack baseline
`95,322,368`, peak live mapped bytes `96,370,944`, final live mapped bytes
`95,322,368`, peak live stacks `1`, and stacks created/reclaimed/live
`473/473/0`.

The first M31 diagnostic run retained the same old accounting semantics and
produced a schema-17 diagnostic report of `40,170,199` bytes with SHA-256
`479f0129e95ca37264079c7889f82054a0282b7eb98e46839d1286a73994a371`.
That report is private and is not committed.

## 4. Existing accounting audit

The old implementation used `executed_functions.size()` as the global
function-transition resource. The only successful append sites were:

1. the initial `enter_function` call;
2. the target entry of `dispatch_call` for `BL`/`BLR`;
3. the target entry of `dispatch_transfer` for cross-function `B`/`BR`.

`enter_function` checked the vector size before lifting and appending the new
entry. `dispatch_call` first validated the continuation and call depth, saved
the caller frame, recorded the exact architectural return PC, and then
entered the target. `dispatch_transfer` preserved X30, pushed no call frame,
and appended the target once. The return path restored the explicit suspended
frame and its expected continuation PC; it emitted `FunctionResume` but did
not call `enter_function`. Runtime-import continuation and tail-invocation
paths likewise resumed an existing contract rather than entering a guest
function. M29 slice, resume, and mid-block-resume paths only continued the
interpreter frame.

The counter increments for direct and indirect call *attempts* occur before
target dispatch. Therefore a failed terminal direct call is included in
`direct_calls`, but not in `executed_functions` or the old resource.

## 5. Exact old reconciliation

For the M30 run, the exact resource identity is:

```text
old charged entries
  = initial entries
  + successful direct-call target entries
  + successful indirect-call target entries
  + successful non-call function-transfer target entries
  = 1 + 467 + 467 + 65
  = 1,000
```

The separately reported call counters are:

```text
direct_calls   = 467 successful direct calls + 1 terminal direct-call attempt = 468
indirect_calls = 467 successful indirect calls
function_transfers = 65 successful transfers
returns = 933
```

There were `933` return events and `933` function-resume events. Neither was
an additional charged entry. The equality is exact; no unexplained re-entry
or dispatch/resume term remains.

## 6. M30 frontier forensic breakdown

The unmodified accounting plus typed diagnostic run classified all `1,000`
charged entries as follows:

| Category | Count |
| --- | ---: |
| initial entry | `1` |
| direct-call entry | `467` |
| indirect-call entry | `467` |
| true non-call function transfer | `65` |
| function resume/re-entry | `0` |
| other | `0` |

The `FunctionTransitionEvidence` records used typed categories, boundary
kind, source and target function/module, source PC and decoded instruction,
call depth, link-register action, expected return PC, frame action, exact
target ownership, canonical boundary validity, prior target/edge counts, and
guest progress counters. The bounded report also includes deterministic
module and depth aggregates.

| Aggregate | Value |
| --- | ---: |
| unique function targets | `491` |
| unique source→target edges | `610` |
| unique source PCs/call sites | `519` |
| repeated target count | `509` |
| repeated edge count | `389` |
| maximum target repetition | `313` (`main:0x72000c353c`) |
| maximum edge repetition | `272` (`main:0x72000f2d40 → main:0x72000c353c`) |
| maximum consecutive target repetition | `239` |
| maximum consecutive edge repetition | `239` |
| same-function transitions | `0` |

The charged source/target module matrix was:

| Source → target | Count |
| --- | ---: |
| main → main | `988` |
| main → sdk | `5` |
| sdk → sdk | `6` |

Charged entry count by call depth after entry was `0:1`, `1:524`,
`2:458`, and `3:17`. All `65` transfer records had distinct canonical
source and target entries; no transfer was same-function. Across the
charged history, source ownership and canonical-boundary validation were
valid for every source-bearing record, and all targets were exact trusted
function entries.

The hottest target was repeated because the guest repeatedly called a helper,
not because the host re-entered a paused activation. The final long indirect
call run advanced through distinct table targets from
`main:0x7200133100` through `main:0x72001344b0`; guest blocks, instructions,
and IR operations increased on each observed group of calls.

## 7. Terminal witness

The resource stop itself was raised after the current function had been
selected, so its top-level report intentionally had no fabricated source PC,
instruction, target register, or pointer provenance. The separate bounded
terminal-attempt record identifies the transition that would have consumed
unit `1,001`:

| Field | Value |
| --- | --- |
| sequence | `1,001` |
| category | direct guest call |
| source | `main:0x72001344b0` |
| source PC | `main:0x72001344dc` |
| source instruction | `bl #0x72000c353c` |
| source opcode | `2550021144` |
| target | `main:0x72000c353c` |
| target ownership | exact trusted function entry |
| source ownership | true |
| canonical boundary valid | true |
| call depth | `1 → 2` |
| expected return PC | `main:0x72001344e0` |
| X30 action | written architectural `PC+4` |
| call frame | pushed |
| target entered previously | true, `313` prior entries |
| source→target edge previously seen | false |
| guest progress at attempt | `17,850` instructions, `3,538` blocks, `105,451` IR ops |
| target register/provenance | not applicable for direct `BL` |

The target was a real trusted entry and the source instruction requested a
normal call. This is the distinction between the budget stop and the
transition that would have consumed the next unit; no pointer-producing
instruction was invented.

## 8. Hypotheses tested and conclusion

- **H1, legitimate long call sequence — proven.** The M30 sequence contains
  `468` direct and `467` indirect call attempts, `933` matched returns, and a
  maximum depth of only `3`. The hot repeated target and final indirect-call
  table show monotonically increasing guest instruction/block/IR counters and
  changing target/call-site progression. The synthetic shallow-call fixture
  also completes more calls than a tiny transition value under the final
  model.
- **H2, intra-function branch misclassification — ruled out.** Internal `B`
  edges remain CFG flow. For every charged non-call transfer in the old real
  trace, source and target were distinct canonical function entries, source
  PC ownership was valid, and same-function count was zero. Synthetic
  intra-function branching records no transfer.
- **H3, resume/re-dispatch double charge — ruled out.** M30 had one yield,
  one resume, and one mid-block resume, while the transition composition
  contains no resume entry. M29 slice and mid-block tests force many slices
  without adding a function entry.
- **H4, return restoration charge — ruled out.** The `933` return/resume
  pairs restore explicit caller frames and expected PCs. They do not call
  `enter_function`; the focused return test checks the resulting entry count.
- **H5, real tail-transfer cycle — ruled out for the M30 run.** The `65`
  real transfers are exact and finite, with guest progress between repeated
  work. A synthetic A→B→A cycle still stops at its exact finite transfer
  limit, so the protection remains present.
- **H6, function-map boundary defect — no evidence.** The real trace has
  exact trusted target ownership, valid source ownership, valid canonical
  boundaries, and no same-function transfer. No private address or one-off
  boundary merge was justified.
- **H7, coarse resource model — proven.** The old vector-size resource mixed
  legitimate completed call activity with the non-returning transfer churn it
  needed to bound. The resource is therefore replaced by a semantically
  precise finite transfer resource, not enlarged or disabled.

## 9. Implementation

The execution report schema is now `17`. It adds
`execution.transition_accounting`, containing:

- model name `non_call_function_transfer_v2`;
- unchanged configured limit and charged total;
- total successful function entries and typed category counts;
- deterministic target, edge, call-site, repetition, module, and depth
  aggregates;
- typed terminal attempted-transition evidence;
- a stable bounded history and its `history_limit`/truncation state.

The `max_function_transitions` default remains exactly `1,000`. A successful
cross-function `B`/`BR` dispatch consumes one unit. A normal `BL`/`BLR`
target entry does not consume this resource, but remains in the bounded
function-entry history and remains subject to call depth, guest-block, event,
IR, refinement, memory, and other finite contracts. Return restoration and
interpreter scheduling never consume it.

The history bound is derived as `max_guest_blocks + 1`, with the maximum
value handled without wraparound. It is diagnostic storage, not an implicit
execution reset or a replacement stop. No third-party dependency was added.

## 10. Synthetic validation

`tests/milestone31_transition_frontier_tests.cpp` contains 16 deterministic
cases and 175 assertions covering:

1. exact direct-call accounting and return contract;
2. certified indirect-call accounting;
3. explicit return resume;
4. multiple resumable IR slices;
5. M29 mid-block resume;
6. true tail transfer, preserved X30, and no frame push;
7. intra-function CFG branch;
8. shallow repeated calls beyond a tiny transfer budget;
9. pathological tail-transfer cycle;
10. recursion and independent call-depth stopping;
11. below/exact/one-beyond transfer-boundary behavior;
12. exact terminal transfer witness without fake pointer provenance;
13. byte-identical deterministic aggregation;
14. generation-scoped stack cleanup after a transfer stop;
15. category/resource reconciliation;
16. zero-budget rejection and overflow-safe derived history bounds.

The standard rejection path also continues to reject zero budgets. The
derived history-bound calculation handles `max_guest_blocks == SIZE_MAX`
without addition overflow, and the focused accounting assertions verify that
the resource count, category sum, executed-entry count, and transfer count
remain independent quantities with exact identities.

## 11. Real private execution after implementation

The same ordinary private command was run twice after implementation. The
first and second reports were both `40,965,888` bytes with SHA-256
`da7ff24e0741f69159cee1866ea1aaee51d374d630ba0a6272aeb06b564c38f0`.
`cmp` reported byte identity.

The former M30 location was crossed naturally. The final real run measured:

| Measurement | Value |
| --- | ---: |
| stop | `indirect_target_refinement_budget_exceeded` |
| stop module/function | `main/0x7200000030` |
| stop PC/instruction | `main:0x7200000148`, `blr x8` |
| target | `main:0x7200135180` |
| target register/provenance | `x8`, guest load at `main:0x720456bb40` |
| guest instructions | `18,427` |
| guest blocks | `3,703` |
| IR operations | `109,272` |
| slices/yields/resumes/mid-block resumes | `2,017/1/1/1` |
| direct/indirect calls | `468/508` attempts |
| successful direct/indirect entries | `468/507` |
| returns | `975` |
| successful true transfers | `65` |
| transition resource | `65/1,000` |
| total successful function entries | `1,041` |
| max call depth | `3` |
| attempts/generation | `513/512` |
| successful promotions | `512` |
| pending refinement work | `1` |
| runtime fallbacks | `0` |

The next frontier is the existing finite indirect-refinement candidate
assessment resource: `512/512`, with `513` candidate records and one pending
candidate. Aggregate refinement consumption was `1,092/200,000` functions
analyzed, `6/100,000` reanalyzed, `139,358` reused, `27,954/8,000,000`
instructions, `5,756/2,000,000` blocks, `8,146/4,000,000` edges,
`111,816/268,435,456` bytes, `512/2,048` boundary-finalization passes,
`3/100,000` invalidated records, and `511/512` transactions. The final
candidate was not dispatched or trusted: its certification/refinement
provenance remains required.

At this refinement stop the call stack snapshot contains the depth-zero
`main:0x7200000030` activation with its synthetic expected return address.
No transition terminal attempt exists because candidate assessment stopped
before an uncertified indirect target could become an executable function
entry. The stop's target/register/provenance are consequently the genuine
indirect boundary evidence, not a transition-budget witness.

## 12. Guest-memory and execution-resource preservation

M30 ownership invariants remain intact in both final private reports:

- non-stack baseline: `95,322,368` bytes;
- peak live mapped bytes: `96,370,944`;
- final live mapped bytes: `95,322,368`;
- peak live stacks: `1`;
- stacks created/reclaimed/live: `513/513/0`;
- live controlled-stack bytes at report time: `0`;
- static mappings remain live.

The final run did not change the call-depth limit (`128`), event limit
(`4,096`), guest-block limit (`1,000,000`), slice quantum (`4,096`),
explicit IR hard-limit behavior, stack size/guard gap, structural candidate
bound, stagnant-round bound, or any aggregate analysis limit. Indirect
candidate identity, pointer provenance, deterministic ordering, certification,
trusted/conflict states, and immutable refinement transactions were unchanged.

## 13. Determinism

| Report | Size | SHA-256 |
| --- | ---: | --- |
| private run A | `40,965,888` | `da7ff24e0741f69159cee1866ea1aaee51d374d630ba0a6272aeb06b564c38f0` |
| private run B | `40,965,888` | `da7ff24e0741f69159cee1866ea1aaee51d374d630ba0a6272aeb06b564c38f0` |

The reports were byte-identical. Stable enum names, ordered vectors, target
history order, module matrix order, depth order, and guest-side addresses
are used; no pointer identity or unordered-container iteration is serialized.

## 14. Validation

- Clean pre-M31 baseline: `317/317`.
- Final standard suite: `333/333` (`25,628` assertions).
- M31 focused: `175` assertions in `16` cases.
- M30 focused: `12,120` assertions in `10` cases.
- M29 focused: `307` assertions in `10` cases.
- GCC-named local configuration (Apple Clang 17 at `/usr/bin/g++`):
  `333/333`.
- ASan/UBSan: `333/333` (macOS leak detection disabled because the runtime
  rejects `detect_leaks=1`; AddressSanitizer and UBSan remained enabled).
- TSan: `333/333`.
- Optional-LLVM configuration: `333/333`, but `LLVM_DIR` was not found, so
  the LLVM backend was disabled; this is not an LLVM 18 result.
- GNU GCC, LLVM 18, and MSVC were not installed on this macOS host. GitHub
  Linux/GCC, LLVM 18, ASan/UBSan, TSan, and Windows/MSVC checks all passed in
  both the push and PR workflows: `10/10` configured jobs passed.

## 15. Dependencies and privacy

M31 adds zero third-party dependencies. The repository contains no TOTK
NSO/XCI/NSP content, keys, title keys, private executable dumps, full private
reports, private local configuration, or machine-specific absolute paths.
The reports and private executable set used above remain local and ignored.
The documented game addresses are limited to deterministic frontier facts
already used by project diagnostics.

## 16. Pull request and recommendation

PR [#36](https://github.com/ikeracevedo2002/TOTKRecomp/pull/36) is open as a
stacked PR on `milestone-30-generation-scoped-stack-memory`. Its exact
base, old reproduction, accounting equation, forensic cause, unchanged
`1,000` default, natural old-frontier crossing, new refinement frontier,
tests, local sanitizer status, `10/10` terminal-passing CI matrix, dependency
count, and privacy statement are recorded in the PR body.

M32 should address the measured `indirect_target_refinement_budget_exceeded`
frontier through the existing proof-preserving refinement resources. It must
start from the pending candidate `main:0x7200135180` and its guest-load
provenance, preserving certification and all finite aggregate limits; it must
not use the former transition frontier as justification for weakening target
trust or raising unrelated limits.
