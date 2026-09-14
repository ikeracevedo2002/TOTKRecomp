# Milestone 44 validation record

This is the tracked, non-proprietary validation outcome for Milestone 44. The
private manifest, executable bytes, full reports, profiler logs, and host
measurement files remain ignored as required by `AGENTS.md`; this record does
not reproduce or embed any of them.

## Frozen input contract

| field | validated outcome |
| --- | --- |
| contract identifier | `m44-provider-unlocked` (private contract file) |
| logical module set | `rtld`, `main`, `subsdk0`, `sdk` |
| module sizes | `7,140`, `36,549,681`, `3,428,817`, `6,132,082` bytes respectively |
| load order | `rtld`, `main`, `subsdk0`, `sdk` |
| recovery/version identity | recovery ExeFS manifest, exact identity retained privately |
| coherence | `verified` |
| SHA-256 values | validated privately; intentionally not committed |
| expected baseline | 32,265 guest instructions; `unknown_guest_function` at `main + 0x3a2650` |
| observed baseline | matched exactly; provider `__nnmusl_init_dso` resolves to guest `sdk` |
| frozen profile reference | 1,187.09 s in the private contract; this is a historical profile reference, not the `/usr/bin/time` wall field below |

The private check was run before implementation and again at final validation
with `scripts/verify_measurement_contract.py`. It returned `CONTRACT_MATCH`.

## Baseline decomposition

The baseline profiler used the frozen input, execution-closure profile, and
four analysis workers. Timings below are nested instrumentation measurements;
they are not additive. The 1,201.59-second wall measurement contains the
refinement interval and report generation.

| measured component | seconds / count | evidence meaning |
| --- | ---: | --- |
| total wall | 1,201.590 | observed `/usr/bin/time` real; user 1,187.960, system 11.270; 1,187.09 s is the separately frozen profile reference (14.50 s / 1.2% difference) |
| analysis/refinement interval | 1,112.028416 | 92.62% of measured wall |
| guest execution and prefix replay | 851.703098; 743 attempts | cumulative 12,166,864 guest instructions |
| lifting / execution lookup | 288.324366; 7,759 calls | `lift_for_execution` cumulative phase |
| IR verification | 87.777223; 695,442 calls | cumulative verifier time |
| candidate handling | 1.754755; 742 assessments | assessment phase and candidate count |
| map reconstruction/publication | 123.873983; 742 rebuilds | refinement publication phase |
| function-map build | 122.712455; 743 builds | all map-build phase timings |
| CFG work | 1,560 analyzed; 6 reanalyzed | aggregate refinement CFG accounting |
| ownership / boundaries | 2 boundary passes; 4 ownership normalizations | no separate wall timer existed in the baseline profiler |
| conflict processing | 46.169497; 73,161,627 pair checks | complete conflict scan timing and pair count |
| finalized-map validation | 61.636848 | full/incremental map validation timing |
| immutable reuse copying | 7.947034; 287,656 records and CFGs | reuse-copy timing and practical copy count |
| allocation churn | 743 controlled stacks created/reclaimed; 779,091,968 cumulative stack bytes | final live mapped bytes 95,322,368; peak 96,370,944; peak regions 17 |
| report generation | 9.652235 | deterministic report serialization |

The profiler therefore identifies prefix replay and repeated immutable map
publication as the dominant measured costs. The post-change comparable wall
measurement is 564.45 s: 53.02% lower (2.13x faster), below the 600 s gate.
The two baseline numbers are retained rather than silently reconciled: 1,187.09
s is the contract's reference value, while 1,201.590 s is the timed run from
which the phase decomposition was collected.
The compact development reproducer is 0.07 s, and frontier extraction is 1.73
s. These figures are not static unsupported-instruction counts.

## Five-frontier real-run ledger

Each row is one real run after the previous accepted state, on the same frozen
input. `diagnosis/fix` is an explicit field rather than an invented human-time
estimate: the original run ledger did not separately time human diagnosis and
editing. The available machine-timed diagnosis/fix validation is recorded in
that column. The shared fixture suite took 0.07 s; the named focused fix tests
were 0.03–0.06 s. An `unrecorded` user/system value is a measurement limitation
of the historical wrapper, not a fabricated value.

| transition | guest instructions before → after | terminal stop (module, PC) | blocker class and exact diagnosis | reproduction (wall; user/system) | diagnosis / fixture / fix timing | expensive runs | validation |
| --- | ---: | --- | --- | --- | --- | ---: | --- |
| frontier 1 | 74,803 → 76,392 | `unsupported_instruction` (`main`, `0x720062cd34`) | ISA/lifter: `fneg v0.4s, v1.4s`; vector FP-unary validation/lift added | 490.79 s; 681.90/16.39 s | diagnosis extraction 1.73 s (shared); fixture 0.07 s (shared); M35 fix test 0.03 s | 1 | compact evidence SHA `75f1d6…`; no pending candidate; 1,517 promotions |
| frontier 2 | 76,392 → 78,408 | `unknown_guest_function` (`main`, `0x7200000148`), target `0x7202478740` | ownership/function boundary: certified target still lacked an exact trusted entry; retained fail-closed evidence before structural diagnosis | 487.47 s; 678.97/16.13 s | diagnosis extraction 1.73 s (shared); fixture 0.07 s (shared); M20 evidence test 0.03 s | 1 | compact evidence SHA `89780e…`; target relocation, CFG, indirect-call, and guest-load evidence recorded |
| frontier 3 | 78,408 → 78,491 | `unsupported_instruction` (`main`, `0x720233f088`) | unrelated direct-call closure at `main + 0xf6948` contains undecodable `0xe7ffdefe`; failed record retained without CFG dereference | 490.18 s; user/system not captured by wrapper | diagnosis extraction 1.73 s (shared); fixture 0.07 s (shared); M22 closure test 0.03 s; decoder fix tested; two diagnostic reruns | 1 accepted run (2 diagnostic reruns) | compact evidence SHA `f9da7c…`; candidate promoted, malformed callee remained failed |
| frontier 4 | 78,491 → 87,919 | `memory_fault` (`sdk`, `0x72047bb870`) | TLS/bootstrap: `TPIDR_EL0=0` under the controlled contract leads to unmapped read `0x1f8`; no state invented | 316.02 s; user/system not captured by wrapper | diagnosis extraction 1.73 s (shared); fixture 0.07 s (shared); resume/diagnostic fix test 0.06 s | 1 | compact evidence SHA `912ee0…`; no pending candidate; budget respected |
| frontier 5 confirmation | 87,919 → 87,919 | same `memory_fault` (`sdk`, `0x72047bb870`) | same faithful-bootstrap blocker; no frontier advance is claimed | 316.20 s; user/system not captured by wrapper | diagnosis extraction 1.73 s (shared); fixture 0.07 s (shared); resume validation 0.06 s; byte-identical terminal report | 1 | compact evidence SHA `912ee0…`; identical to frontier 4; zero fake progress |

The frontier-5 row is deliberately a real terminal confirmation, not an
advancing frontier and therefore does **not** satisfy the requested fifth new
frontier. The latest state is 87,919 executed guest instructions and the next
stop is the honest `sdk` TLS/bootstrap boundary. Dynamic-symbol evidence maps
the stopped function at `sdk + 0x13186c` to
`nn::os::detail::InternalCriticalSectionImplByHorizon::IsLockedByCurrentThread`;
its TLS-relative read requires a faithful Horizon current-thread/bootstrap
object, not merely writable memory. The selected ExeFS set contains only the
four frozen modules and supplies no launch-time Horizon thread state. Clearing
this boundary therefore requires new external bootstrap evidence or an
explicitly revalidated launch contract; a host stub, selected TLS address,
forced register, or weakened completeness check is prohibited.

## Final validation

- Final checkpoint: `d6a10ac` (lineage starts at `cedea44`).
- Normal local suite: 424/424 registered tests passed.
- Local AppleClang TSan suite: 424/424 passed with no race report.
- Final real A/B under the final code checkpoint: both completed under the
  declared 600-second budget (317.078 s / 319.831 s), with user/system times
  588.875/5.787 s and 596.476/5.861 s. Full reports and compact evidence were
  byte-identical; report SHA-256 was
  `34f1480812eb616e4da5a9cb4fa1f6e6b1eaabbe4630ba198aef82c1067ef848`.
- Remote CI completed successfully across Linux/GCC, Linux/LLVM 18,
  ASan/UBSan, TSan, MSVC, and the CI gate. The transient workflow identifier
  is intentionally kept in the external handoff rather than this tracked
  document.
- Repository privacy check found no tracked local content, private reports,
  executable artifacts, keys, or `.DS_Store`; `git diff --check` passed.
