# Milestone 20: indirect function-entry certification

## Git and baseline

The branch is `milestone-20-indirect-function-entry-certification`. It was
created from the exact M19 head `cb4110984b21184e81385b1e2b51d705f87d9437`,
whose exact M19 base is `f571c21e7d306bfc80f3cba294bd4d31cfb2607e`.
The implementation commit is `bf8515f31011f49ecc0809a3cd5a23627514cb9a`.
The final documentation commit SHA is reported in the completion handoff
because a commit cannot contain its own final SHA. PR #25 is open against the
M19 branch.

The pre-change CTest baseline was 233/233. M20 adds eight focused test cases;
the current suite is 241/241.

## Investigation

The prior boundary was `main:0x7200000148`, `blr x8` with `x8` equal to
`main:0x7200001520`. The runtime register provenance was
`guest_load:0x720456ab68`.

The storage slot is in `main` and has a recorded dynamic relocation at the
exact process guest address. Its type is `R_AARCH64_RELATIVE`, its relocation
source is `RELA`, its relocation index is 479600, and its addend is 5408
(`0x1520`). Loader arithmetic therefore resolves the slot to
`main:0x7200001520`, and the applied slot value is read back and verified.
There is no dynamic `STT_FUNC` declaration for this target, and the relative
relocation does not declare function-pointer semantics. No symbol, export,
MOD0 entry, or direct `BL` independently names this exact target in the
available metadata. The slot is consequently retained as supporting
relocation provenance, not promoted by itself.

The target is uniquely owned by `main`, is mapped executable, and is aligned.
Its bounded CFG contains 12 blocks, 59 instructions, and 17 accounted edges,
with no unresolved control flow or precise ownership overlap. Its first
instruction is `stp x29, x30, [sp, #-0x20]!` (`0xa9bf7bfd`) and its next guest
PC is `main:0x7200001524`.

## Evidence model and policy

M20 adds typed, serializable function-entry evidence kinds for symbols,
dynamic symbols, direct call targets, relocation function targets, relocated
function pointers, exports, known providers, bounded CFG candidates, observed
indirect calls, and guest-loaded pointers. Evidence records retain source and
target modules, exact guest addresses, relocation identity, symbol identity,
function metadata, slot readback, and stable details. Accepted and rejected
evidence are sorted explicitly.

The engine now reports a `FunctionCandidate` and a rule-based
`FunctionCertificationResult`. A canonical map record also has an explicit
entry trust status: `candidate`, `trusted`, or `conflict`. Executability,
alignment, module ownership, register values, guest-load provenance,
relocation rebasing, or decoding alone never certify an entry.

For an observed indirect call, the existing M18 bounded refinement remains
available: the callsite must be an observed guest control-flow edge, the
target must have unique executable ownership, bounded CFG analysis must
produce structurally valid precise ranges, and no existing function may
overlap them. For the real M20 target, the proof chain additionally includes
the exact loader-relocated slot and verified readback. The relative relocation
is explicitly reported as “rebasing only; no function metadata”; the observed
call plus the complete bounded CFG supplies the function-entry certification
rule. This preserves the M18 synthetic policy without turning arbitrary
executable data or loaded pointers into functions.

Promotion is transactional: the assessment is immutable, and successful
refinement rebuilds and validates a new frozen process-wide map before the
caller installs it. Repeated observations and same-entry direct/indirect
evidence are idempotent. Exact entries are never duplicated. Precise interior
ownership and any incompatible overlap remain rejection outcomes; display
envelopes do not substitute for precise instruction ownership. Cross-module
targets use process virtual addresses while relocation/provider evidence keeps
its source module attribution.

## `main:0x72000001a4`

The exact instruction word is `0x9b4c7d4a`, decoded by Capstone as
`smulh x10, x10, x12`. The previous failure was a project normalization gap:
Capstone recognized the instruction but the project-owned decoder mapped it to
`Unknown`. M20 adds the generic `InstructionId::Smulh` normalization and
classifies it as an ordinary fall-through instruction.

This is intentionally a structural decoder addition, not a semantic IR
claim. `SMULH` remains outside the current lifting subset. If execution
reaches it, the executor stops with the typed unsupported-instruction
boundary; discovery does not reject an otherwise proven function merely
because future computation is not yet liftable. No proprietary surrounding
bytes are committed.

The decoder/lifter post-pass also received a generic invariant fix: atomic
block rewriting no longer changes constant value definitions into
instruction definitions. That bug became reachable only after the candidate
function could be lifted.

## Real execution proof

The private four-module (`rtld`, `main`, `subsdk0`, `sdk`) controlled run uses
the existing metadata-selected entry and process setup. It now reaches the
former callsite, certifies
`main:0x7200001520`, and enters that address as ordinary guest code. The
certification report preserves both runtime provenance
`guest_load:0x720456ab68` and the separate certification evidence chain.

The target was newly promoted exactly once. The first target instruction was
executed at `main:0x7200001520`; its opcode is `0xa9bf7bfd`, and the next guest
PC is `main:0x7200001524`. Runtime fallback invocation count is zero. The run
continued to the next genuine typed boundary at `main:0x72000001a4`:
`SMULH X10,X10,X12`, opcode `0x9b4c7d4a`, with typed
`unsupported_instruction` status.

The final report records 182 guest instructions, maximum call depth 3, four
direct calls, one indirect call, one function transfer, four returns, and
zero runtime fallbacks. The former M19 instruction marker was not observed in
the final post-refinement trace, so the legacy
`instructions_after_former_blocker` counter is 0; the former ownership
boundary itself was crossed and the target-entry fields above are the direct
proof of that crossing. `sdk:0x7204717c30` was not naturally reached, so no
UMULH operands or result are claimed and it was not invoked manually.

## Determinism, schema, and privacy

The deterministic execution report schema is 7 because M20 adds an externally
meaningful typed certification/evidence contract and guest-entry proof fields.
Two identical private runs produced byte-identical reports. Both
`m20-final-1.json` and `m20-final-2.json` hash to
`f01cf0f4ac02ec95c7389181b7d9194568d46687ef029aa6cd5281b4237f45d0`.
Reports contain guest metadata, addresses, single instruction facts, typed
provenance, and hashes only; no local paths or proprietary byte dumps are
serialized.

Bootstrap remains unchanged: no bootstrap model was added and no new process
state was invented. The run stops at the observed unsupported instruction;
M20 does not prove that TOTK boots, that the entire executable set is
executable, or that arbitrary indirect calls are solved.

## Validation matrix

| Validation | Result |
| --- | --- |
| Standard AppleClang build | pass |
| CTest before M20 | 233/233 |
| CTest with M20 | 241/241 |
| GCC | pass, 241/241 |
| ASan + UBSan | pass, 241/241 |
| TSan | pass, 241/241 |
| LLVM 18 | pass in PR #25 CI |
| MSVC | pass in PR #25 CI |
| New dependencies | 0 |

Private NSO files, keys, local configuration, extracted executable data,
private reports, and absolute private filesystem paths are not tracked.
