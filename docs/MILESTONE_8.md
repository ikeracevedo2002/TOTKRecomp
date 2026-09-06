# Milestone 8 — AArch64 FP/SIMD

Milestone 8 makes the AArch64 floating-point and 128-bit SIMD register file a
first-class part of SwitchRecomp's architectural model. It is deliberately
limited to synthetic and legally supplied guest code: the repository contains
no Nintendo binaries, keys, SDKs, firmware, or extracted game assets.

## Architectural contract

`runtime::CpuState` contains one shared `V0`–`V31` register file. Each register
is `runtime::Vector128 { lo, hi }`, where `lo` is guest bits 63:0 and `hi` is
guest bits 127:64. The scalar S and D names are views of the low 32 and 64
bits, while Q uses all 128 bits. The state also contains FPCR and FPSR.

The decoder never exposes Capstone types to the IR. It normalizes vector
arrangements, lane indices, scalar FP immediates, and an explicit
`SimdOperation`. The lifter then emits typed IR operations such as
`FpBinary`, `FpConvert`, `VectorExtractLane`, `VectorBinary`, and
`GuestLoadVector`.

Scalar arithmetic is implemented by a reference runtime that consumes raw
IEEE-754 bit patterns. Guest rounding mode, default-NaN policy, signaling-NaN
handling, divide-by-zero, invalid-operation, overflow, and sticky FPSR state
are modeled explicitly. The host floating-point environment is scoped to a
single operation and is never used as guest architectural state.

## Supported minimum

- Scalar `FMOV`, `FADD`, `FSUB`, `FMUL`, `FDIV`, `FNEG`, `FABS`, `FSQRT`,
  `FCMP`/`FCMPE`, `FCSEL`, `FMIN`, and `FMAX`.
- Scalar `SCVTF`, `UCVTF`, `FCVTZS`, `FCVTZU`, S↔D `FCVT`, and `FRINTN`,
  `FRINTP`, `FRINTM`, `FRINTZ`.
- `DUP`, `INS`, `UMOV`, `SMOV`, `EXT`, `ZIP1/2`, `UZP1/2`, and `TRN1/2`.
- B/H/S/D arranged logical, integer add/sub/multiply, FP add/sub/multiply/
  divide, and the normalized comparison families.
- Checked S/D/Q single and pair memory forms through the existing guest-memory
  runtime boundary. Q accesses always use 16-byte vector helpers.

Fused multiply-add, atomics, system instructions, function-map dispatch, and
whole-module execution remain explicit future work. Unsupported forms return a
structured `UnsupportedInstruction` error; they are not silently lowered to
host instructions.

## Backend rule

The interpreter is the executable reference. The optional LLVM backend lowers
the same IR and calls stable C-linkage runtime helpers for FP/SIMD operations,
so LLVM availability does not change architectural semantics. The generated
CPU-state type includes the vector register array and FPCR/FPSR fields with
compile-time layout assertions in the runtime.

## Validation

Milestone 8 tests cover register aliasing, raw IEEE edge cases, rounding and
FPSR behavior, lane extraction/insertion and permutations, checked vector
memory, decoder normalization, IR verification, interpreter execution, and
interpreter/LLVM agreement when LLVM 18 is available. The CLI supports
`--show-vector-state` to print V0–V31, FPCR, and FPSR after execution.
