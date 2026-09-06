# Milestone 9 — Threads, TLS, atomics, and memory ordering

Milestone 9 adds the first executable concurrency boundary to SwitchRecomp.
It is limited to synthetic and legally supplied standalone AArch64 functions;
it does not claim that TOTK or Horizon services execute.

## Scope and ownership

The decoder produces project-owned metadata for the supported AArch64 subset:

- `LDXR`/`STXR`, `LDAXR`/`STLXR`, and `LDAR`/`STLR` in byte, halfword, word,
  and doubleword forms;
- `CLREX`, `DMB`, `DSB`, and `ISB`;
- `MRS`/`MSR` for `TPIDR_EL0`, plus read-only `TPIDRRO_EL0`;
- explicit width, acquire/release order, barrier option, and system-register
  identity in the normalized instruction.

Pair-exclusive instructions (`LDXP`, `LDAXP`, `STXP`, `STLXP`), LSE atomics,
`WFE`/`WFI`, and Horizon synchronization services remain explicit deferred
boundaries. They must not be silently treated as ordinary loads or stores.

## Native thread and TLS model

`GuestThread` maps one guest thread to one joinable native `std::thread`.
`ThreadManager` owns stable project-defined IDs and reports the lifecycle
`Created → Running → Exited → Joined`. Detached threads are not part of this
milestone. Thread entry exceptions are captured and returned through `join()`
and `join_all()`; they do not cross the native thread boundary uncaught.

Every guest thread has its own `CpuState`, `RuntimeContext`, and
`ExclusiveReservation`. `TPIDR_EL0` and `TPIDRRO_EL0` are fields in that
per-thread CPU state. The generated-function ABI remains:

```cpp
std::uint32_t generated(CpuState* cpu, RuntimeContext* runtime);
```

The runtime context also carries the stable guest-thread ID and a pointer to
the shared memory coordinator. This keeps TLS and monitor state out of global
mutable storage.

## Shared memory and ordering

`SharedRuntimeState` owns the synchronization boundary around a
`GuestMemory`. Shared byte accesses, scalar atomics, vector accesses, and
exclusive operations validate the full range while holding the coordinator's
lock. Guest memory is never exposed to lifted code as a host pointer and the
implementation does not use `std::atomic_ref` over guest bytes.

The IR uses project-owned `ir::MemoryOrder` values. Loads accept relaxed,
acquire, or sequentially consistent order; stores accept relaxed, release, or
sequentially consistent order. Acquire and release operations map to the
corresponding host fences only inside the runtime helper, after the guest
access has been validated. DMB, DSB, and ISB retain their kind and encoded
option in `MemoryBarrier`; invalid options are structured errors.

Exclusive reservations are per thread and use deterministic 64-byte granules.
An exclusive store succeeds only when address, width, granule, and generation
match the current reservation. It returns status zero on success and one on a
valid reservation failure. Any ordinary synchronized write invalidates every
overlapping granule. Invalid widths, misalignment, arithmetic overflow,
unmapped ranges, and denied permissions remain observable `ErrorCode` values.

## IR, interpreter, and LLVM

The M9 verifier checks the arity, types, width, order, barrier, and system
register metadata of:

`AtomicLoad`, `AtomicStore`, `ExclusiveLoad`, `ExclusiveStore`,
`ClearExclusive`, `MemoryBarrier`, `ReadSystemRegister`, and
`WriteSystemRegister`.

The interpreter is the reference implementation. Its M9 path calls the same
stable C ABI helpers used by the optional LLVM 18 backend. The LLVM path lowers
the concurrency operations to helper calls, registers those symbols with the
JIT, and uses a CPU-state layout that includes both TPIDR fields. No exception
is allowed to cross the generated-function ABI.

## Evidence and reproducibility

All committed fixtures use recorded 32-bit AArch64 words and synthetic guest
memory. The end-to-end tests cover decoder → CFG → lifter → verifier →
interpreter, and, when LLVM 18 is available, the same function through LLVM IR
and the native JIT. They assert TLS isolation, reservation invalidation,
contention retry behavior, acquire/release message passing, barriers, and
alignment/range/permission failures.

Local validation commands:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DTOTKRECOMP_ENABLE_LLVM=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure

cmake -S . -B build-llvm -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DTOTKRECOMP_ENABLE_LLVM=ON \
  -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
cmake --build build-llvm --parallel
ctest --test-dir build-llvm --output-on-failure

cmake -S . -B build-sanitize -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DTOTKRECOMP_ENABLE_SANITIZERS=ON
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure

cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DTOTKRECOMP_ENABLE_THREAD_SANITIZER=ON
cmake --build build-tsan --parallel
ctest --test-dir build-tsan --output-on-failure
```

The pull request records the actual test counts and CI job results for the
revision under review. This document intentionally does not invent coverage
or external-reference numbers. Commercial game data, keys, firmware, SDKs,
and extracted assets are not required or accepted by this milestone.
