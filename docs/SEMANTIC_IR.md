# SwitchRecomp Semantic IR

Milestone 6 introduces a small, project-owned intermediate representation between
the Capstone-backed AArch64 decoder/CFG and any host code generator:

```text
AArch64 bytes → decoded instruction → CFG → Semantic IR → interpreter/LLVM
```

The IR is the semantic contract. It deliberately does not expose Capstone types,
LLVM types, host pointers, or C++ object layouts. This makes the interpreter a
useful independent execution path and keeps future decoder/backend changes local.

## Values, types and identity

The current scalar types are `i1`, `i8`, `i16`, `i32`, and `i64`. Values and blocks
use monotonically assigned `ValueId` and `BlockId` wrappers. IDs are deterministic,
comparable, printable, and independent of host addresses.

Guest addresses are values in the guest `i64` domain. They are never converted to
host pointers by the IR or by generated code.

## Operations

The implemented operation set is:

- `constant`, `read_reg`, `write_reg`;
- wrapping `add`, `sub`, `and`, `or`, and `xor`;
- `cmp_eq` and `cmp_ne` producing `i1`;
- checked `guest_load` and `guest_store` for 32-bit and 64-bit accesses;
- `branch`, `cond_branch`, `return`, and `nop`.

Each operation has an optional source location containing the guest PC, original
32-bit opcode, and `InstructionId`. The deterministic printer renders this mapping
so diagnostics can point back to the source instruction.

## Blocks and verification

Every block has a stable ID, a guest start address, ordered operations, and exactly
one final terminator. The verifier rejects missing or duplicate IDs, invalid block
targets, undefined values, use-before-definition, wrong operand counts/types,
invalid register widths, invalid memory sizes, and operations after terminators.

Verification is required before interpretation or LLVM lowering.

## Register semantics

`CpuState` owns X0–X30, SP, PC, NZCV, FPCR, FPSR, and portable V0–V31 `Vector128`
storage. Centralized helpers implement the important scalar rules:

- W reads use the low 32 bits;
- a W write replaces the complete X register with a zero-extended value;
- XZR/WZR read as zero and ignore writes;
- SP is a separate field and is never an alias for XZR.

FP/SIMD state is represented for forward compatibility but is not lifted in this
milestone.

## Guest memory

`guest_load` and `guest_store` receive a guest integer address and call
`GuestMemory`. Mapping, bounds, permissions, and unmapped access errors remain in
the existing memory model. LLVM code calls stable C ABI helpers; it does not emit
`inttoptr`, host loads, host stores, or GEPs into `GuestMemory`.

## Interpreter

The interpreter executes a verified function against `CpuState` and
`GuestMemory`. It returns an explicit `ExecutionResult` containing the final guest
PC and operation count. A configurable step limit prevents accidental infinite
loops. Invalid blocks, undefined values, memory faults, and step exhaustion are
returned as structured errors.

## LLVM boundary

The LLVM backend accepts only `IrFunction`. It creates a host function with the
internal ABI `i32 (RuntimeExecutionContext*)`. Runtime accesses are performed by
the C ABI helpers in `runtime/abi.hpp`, where a zero return means success and a
non-zero return transfers control to a generated error block. Every generated
function and module is verified before JIT execution or object emission.

LLVM is discovered through `find_package(LLVM CONFIG)`. The preferred pinned
toolchain for this milestone is LLVM 22.1.8. Builds without LLVM still compile the
core, interpreter and CLI; requesting lowering/JIT/object emission then returns
`llvm_unavailable` rather than silently using the interpreter.

## Implemented AArch64 subset

The lifter consumes the existing CFG and supports `NOP`, register/immediate `MOV`,
`ADD`, `SUB`, register `AND`/`ORR`/`EOR`, optional `MOVZ`/`MOVK`, base-plus-immediate
`LDR`/`STR`/`LDUR`/`STUR`, `B`, `CBZ`, `CBNZ`, and standalone `RET`.

Writeback, register-offset and unsupported shifted/extended addressing forms are
rejected. `BL`, `BR`, `BLR`, flags-producing arithmetic, conditional branches,
FP/SIMD, atomics, barriers, traps, exceptions, and other unsupported instructions
fail explicitly with source PC/opcode diagnostics. `RET` returns to the host harness;
the guest call ABI is intentionally a later milestone.

## Evolution

The public IR can later add flag values, guest calls, indirect dispatch, FP/vector
types, atomic memory effects, exception edges, SSA promotion, and additional
backends without making Capstone or LLVM the semantic source of truth.
