# Milestone 4 — AArch64 decoder and control-flow analysis

## Scope

Milestone 4 adds reusable `SwitchRecomp` infrastructure that turns executable
guest bytes into normalized AArch64 instructions and a deterministic bounded
control-flow graph.

The implementation provides:

- a Capstone 5.0.3 backend hidden behind `AArch64Decoder`;
- explicit little-endian, 4-byte-aligned instruction fetch through `GuestMemory`;
- SwitchRecomp-owned instruction IDs, registers, operands, memory operands,
  conditions, PC-relative values, disassembly, and control-flow categories;
- checked target arithmetic for `B`, `BL`, `B.cond`, `CBZ/CBNZ`, `TBZ/TBNZ`,
  `ADR`, `ADRP`, and literal loads;
- basic blocks, typed edges, direct call candidates, unresolved indirect-flow
  records, range/resource limits, executable-target validation, and block
  splitting;
- deterministic `aarch64-analyze` output for locally supplied raw code images.

## Non-goals

This milestone does not implement semantic IR, LLVM lowering, native execution,
CPU emulation, relocation application, import or symbol resolution, whole-TOTK
function recovery, indirect-target recovery, jump-table analysis, or validation
against a real game binary.

## Decoder backend

Capstone v5.0.3 is fetched at a pinned upstream tag/commit and used only as a
decoder/disassembly backend. Capstone types, IDs, pointers, and lifetimes do not
appear in SwitchRecomp public APIs. The decoder copies all required data into
owned project structures so a future backend can replace Capstone without
changing CFG or lifting consumers.

## Address and instruction model

Guest addresses remain integer values in the guest address domain. They are
never cast to host pointers. The fetcher requires executable memory, checks
alignment, reads exactly four bytes, and assembles the word explicitly as
little-endian. `DecodedInstruction` preserves its guest address and raw opcode,
then adds a project-owned ID, normalized operands/registers, disassembly, and
explicit flow metadata. A backend-recognized instruction may still have
`InstructionId::Unknown`; if its backend metadata proves ordinary fallthrough,
CFG traversal can continue. Unknown control-flow behavior terminates that path
with an unresolved diagnostic.

## Control flow and CFG

`B` has one direct branch edge. `BL` records a direct call target and keeps the
return-site fallthrough; call targets are not recursively analyzed by default.
`BR`, `BLR`, and `RET` preserve their source/return registers. Conditional
branches expose taken and not-taken edges. Traps and exception-boundary
instructions terminate normal traversal explicitly.

Analysis starts from an explicitly supplied entry address and uses a worklist,
visited leaders, sorted CFG storage, and finite instruction/block/worklist
limits. Direct targets must be aligned and executable before traversal. A valid
executable target outside an optional analysis range is retained as an external
edge. If a later target lands in the middle of a previously formed block, that
block is split so each instruction belongs to exactly one canonical block.

## Unsupported and unresolved cases

Decode failure, truncated/unmapped fetch, non-executable access, invalid direct
targets, arithmetic overflow/underflow, and resource-limit exhaustion return
structured errors. Valid indirect branches/calls are not fatal: they are stored
as unresolved flow with the source register and no fabricated target. Valid
instructions without a normalized lifting ID remain usable for CFG traversal if
their control-flow behavior is known.

## Testing

The synthetic test suite contains 91 test cases, including 18 Milestone 4 test
cases and 177 Milestone 4 assertions. It covers scalar, memory, FP/SIMD,
exclusive/atomic, system, PC-relative, direct/indirect control flow, traps,
little-endian fetch, permissions, truncation, invalid opcodes, loops, diamonds,
self-loops, calls, limits, invalid targets, deterministic rendering, and block
splitting. No Nintendo or TOTK input is required.

## Future Milestone 5 contract

Milestone 5 can consume a `ControlFlowGraph` whose instructions already contain
registers, immediates, memory operands, conditions, direct targets, original
opcodes, and stable instruction boundaries. It must not need to parse raw bytes
again to reconstruct those properties. Semantic lifting remains a separate
layer and is not implied by `backend_decoded` or `normalized`.
