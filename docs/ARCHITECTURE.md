# TotkRecomp Architecture and Implementation Plan

> Status: Proposed architecture with Milestones 0–9 implemented
> Repository snapshot: 2026-09-06
> Target: The Legend of Zelda: Tears of the Kingdom for Nintendo Switch  
> Current repository state: Initial C++20 build/test foundation, target-manifest model, common safety utilities, CI, strict NSO0 header parsing, bounded NSO image materialization with SHA-256 verification and explicit BSS, checked host-backed guest memory loading, MOD0/dynamic/RELA metadata discovery, dynamic symbol/relocation application, expanded AArch64 Semantic IR and lifting, synthetic tests, and deterministic inspection/coverage reports are committed; no supported game build has been committed.

This document is the primary engineering RFC for TotkRecomp. It describes the intended architecture, the evidence behind the design, the work required to validate it, and the boundaries of what is currently known.

It is deliberately conservative. A proposed component is not evidence that the component exists, that TOTK uses a particular API, or that a complete port is feasible. Any item that depends on the exact game build or on reverse engineering is marked **Needs verification**.

## 1. Scope and status vocabulary

### Existing

The repository contains the Milestone 0 bootstrap, Milestone 1 strict NSO0
header parser, Milestone 2A image materializer, Milestone 2B guest loader,
Milestone 3 MOD0/dynamic metadata parser, Milestone 4 decoder/CFG, Milestone 5
dynamic linking, Milestone 6 Semantic IR/lifting, Milestone 7 expanded
AArch64 semantics and coverage, Milestone 8 FP/SIMD state and semantics, and
the controlled Milestone 9 thread/TLS/atomic runtime subset:
a C++20 source tree, CMake build, common safety utilities, manifest validation,
tests, CI, strict NSO0 inspection, safe NSO image
materialization, integrity verification, a checked guest-memory loader, and a
deterministic `nso-inspect` report, an LLVM-independent typed Semantic IR, a
verifier, a reference interpreter, and an optional LLVM lowering/JIT backend for
synthetic standalone functions, normalized scalar memory/control-flow semantics,
and a local whole-range coverage scanner. Milestone 8 adds shared V-register
state, FPCR/FPSR, reference FP/NEON operations, and checked vector memory.
Milestone 9 adds joinable native guest threads, per-thread CPU/TLS state,
checked shared memory, exclusive reservations, acquire/release ordering, and
barriers through a stable runtime ABI. There is still no game-specific runtime,
metadata for a supported game version, renderer, or supported game build to
preserve.

### Proposed

The design in this RFC: a reusable SwitchRecomp layer, a game-specific TotkRecomp layer, a staged static recompiler, a native runtime, and a native graphics path.

### Future

Work that should happen only after the required evidence, test coverage, and preceding milestones exist. Future items must not be presented in README files or release notes as implemented.

### Needs verification

An item that requires inspection of a legally obtained, exact target build, controlled runtime traces, public technical research, or a prototype. This label is mandatory for TOTK-specific claims that cannot be established from public evidence.

## 2. Project definition

TotkRecomp is an experimental static recompilation project. Its intended input is a legally obtained and appropriately prepared build of the TOTK executable and its associated game data. Its intended output is host-native code plus a native runtime that provides the behavior the recompiled code expects.

The conceptual pipeline is:

```
AArch64 executable
        |
        v
NSO/MOD0 parsing
        |
        v
function and data analysis
        |
        v
AArch64 decoding and control-flow recovery
        |
        v
SwitchRecomp semantic IR
        |
        v
LLVM IR / native object code
        |
        v
TotkRecomp runtime and game-specific integration
        |
        v
host operating system and native graphics API
```

Static recompilation is not the same as decompilation. The first implementation target is behavioral preservation of machine code, not human-readable C++ or a source-level recreation of the game.

### 2.1 What the project is

- A recompiler and analysis toolchain for a specific AArch64 game build.
- A native execution runtime for the translated code.
- A compatibility layer for only the platform behavior required by the target.
- A game-specific integration project containing build metadata, patches, hooks, runtime overrides, and renderer integration.
- A research project whose correctness must be demonstrated with small deterministic tests before large-scale translation is attempted.

### 2.2 What the project is not

- A complete Nintendo Switch emulator.
- A replacement for a Nintendo Switch firmware installation.
- A generic binary translator that can accept every Switch program without metadata.
- A decompilation of TOTK.
- A license to redistribute Nintendo code, game data, firmware, SDKs, keys, or assets.
- Proof that native recompilation alone makes the game run at an arbitrary frame rate.
- A promise that any module, service, renderer boundary, or version works before it is measured.

The runtime may contain emulator-like mechanisms such as guest addresses, handles, synchronization, and service dispatch. Those mechanisms exist to preserve the behavior required by the recompiled game; they are not an objective to reproduce the whole Switch.

## 3. Design principles

1. **Correctness before speed.** A slow, observable, deterministic implementation is more valuable than an optimized implementation that silently corrupts state.
2. **Exact-build targeting.** Addresses, symbols, layouts, and patches belong to one executable build unless metadata explicitly says otherwise.
3. **No silent fallbacks.** Unknown instructions, imports, service calls, relocations, and graphics operations must produce actionable diagnostics.
4. **Guest semantics remain explicit.** Guest addresses and guest function pointers are integers in the guest address domain until a controlled runtime boundary resolves them.
5. **On-demand platform compatibility.** Implement only the Horizon, SDK, filesystem, input, audio, timing, and graphics behavior observed to be required.
6. **Separation of generic and game-specific code.** Generic Switch infrastructure must not contain TOTK addresses or patches.
7. **Reproducible generated output.** Analysis metadata and generated artifacts need stable schemas, hashes, and versioned toolchains.
8. **Traceability.** Every generated function, patch, relocation, import, and native crash should map back to a guest address and target build.
9. **License hygiene.** Research can inform the design without automatically becoming a code dependency.
10. **Legally bounded inputs and outputs.** The project must never require committing Nintendo executables, keys, firmware, SDKs, or copyrighted game assets.

## 4. Architecture boundaries

The recommended split is conceptual rather than a promise of two repositories.

```
SwitchRecomp
├── format
├── analysis
├── aarch64
├── ir
├── codegen
├── runtime
├── horizon
├── graphics abstraction
└── developer tools

TotkRecomp
├── exact-build manifests
├── symbols and function metadata
├── TOTK-specific patches and hooks
├── import/service requirements
├── asset and filesystem integration
├── renderer adaptation
├── shader metadata/cache
└── debugging and release configuration
```

### 4.1 SwitchRecomp

SwitchRecomp should eventually be reusable by another AArch64 Switch binary. It owns:

- NSO and related module-format parsing.
- MOD0 and dynamic metadata parsing, with versioned schemas.
- AArch64 decoding, instruction classification, and semantic lifting.
- Function discovery and control-flow analysis.
- Guest address and guest memory abstractions.
- Module registration, symbol resolution, and relocation application.
- Generic call dispatch for guest function pointers.
- A minimal runtime for memory, threads, synchronization, TLS, time, handles, and diagnostics.
- Generic service and graphics interfaces.
- Tooling that does not contain TOTK-specific addresses.

### 4.2 TotkRecomp

TotkRecomp should own:

- The first supported TOTK version and module hashes.
- Game-specific function boundaries, symbols, globals, vtables, jump tables, and confidence annotations.
- Patches and hooks.
- Resolved imports and service requirements.
- Game-specific runtime behavior.
- Renderer integration and shader/resource conventions.
- Input mapping, save paths, configuration, and diagnostics relevant to the port.
- Build manifests and generated output for the exact target.

A game-specific patch must not be added to SwitchRecomp merely because it makes TOTK boot.

## 5. Why static recompilation is viable, and why this target is difficult

Static recompilation can be effective when a program's code can be analyzed ahead of time and its platform-dependent behavior can be supplied by a compatible runtime. Existing projects demonstrate several useful patterns:

- N64Recomp splits a binary into functions, emits literal C, uses metadata, handles direct calls and indirect function lookups, and expects a separate runtime.
- Zelda64Recomp builds a game-specific port around recompiled output, a runtime, graphics integration, configuration, and separately compiled patch functions.
- XenonRecomp converts Xbox 360 PowerPC instructions into C++, passes an explicit CPU state, resolves indirect functions with a generated lookup strategy, and supplements automatic analysis with manual metadata.
- UnleashedRecomp demonstrates that native rendering and pipeline preparation can be added after recompilation rather than treating the original GPU as the final rendering abstraction.
- PS2Recomp separates analyzer, recompiler, runtime, stubs, syscall dispatch, and tests.
- RecompOne demonstrates a C# recompiler/runtime split and shows that a runtime can model platform services without linking the original console SDK.
- Skate3Recomp demonstrates the scale of game-specific metadata, patching, manifests, generated code, and a native renderer.

TOTK is substantially harder than those examples in several dimensions:

- AArch64 is a modern 64-bit ISA with FP/SIMD, weak memory ordering, exclusive atomics, and system-register interactions.
- A large retail C++ game is likely to contain heavy use of indirect calls, vtables, callbacks, templates, statically linked SDK code, worker threads, and asynchronous resource streaming.
- NSO modules are not a self-describing source-level program. Stripped code, compiler optimizations, tail calls, jump tables, data/code ambiguity, and inlined libraries make function recovery uncertain.
- A native renderer must preserve the game's observable graphics behavior without assuming that a high-level public symbol boundary survives linking.
- Correctness is not limited to instruction translation; object layouts, address identity, timing, synchronization, files, services, shader semantics, and error behavior all matter.

Therefore the first definition of success is not “launch TOTK.” It is a trustworthy pipeline that can inspect an exact module, load it into a guest address model, translate small functions, execute them against a reference, and fail with useful evidence when coverage is incomplete.

## 6. Translation strategy decision

### Decision

Use a three-stage representation:

```
AArch64 → SwitchRecomp semantic IR → LLVM IR → host object/native code
```

The custom semantic IR is the architectural contract. LLVM IR is the lowering and optimization target, not the only source of guest semantics.

### Rationale

A direct AArch64-to-LLVM design is attractive because LLVM has mature optimizers and host backends. It becomes awkward when the translator must preserve:

- 32-bit and 64-bit register write rules.
- NZCV flag production and lazy flag materialization.
- SP's special AArch64 behavior.
- Guest addresses that cannot be treated as host pointers.
- PC-relative data references and guest code addresses.
- Indirect calls whose target is a guest address.
- Exclusive monitors and memory ordering.
- Exact FP/NEON behavior where host lowering differs.
- Unsupported or intentionally trapped instructions.
- Source mapping from an instruction to a generated operation.

A semantic IR permits analysis and validation before host-specific lowering. It also makes a future non-LLVM backend possible without making that backend an initial requirement.

### Options

| Option | Advantages | Costs | Decision |
| --- | --- | --- | --- |
| Literal C/C++ | Simple output, easy to inspect, proven by N64/Xenon projects | Very large output, weak semantic structure, optimizer and compile-time pressure, awkward precise flags and guest memory | Useful prototype or fallback |
| Direct LLVM IR | Strong optimizer and x86-64/ARM64 backends, typed SSA, mature object generation | High coupling to LLVM, awkward guest state and address semantics, difficult diagnostics if semantics are emitted incorrectly | Not the first architectural boundary |
| Custom IR lowered to LLVM | Explicit guest semantics, testable stages, good optimization path, controlled debug mapping | More code and schema design up front | Recommended |

### 6.1 Decoder and lifting dependencies

- **LLVM MC:** preferred first decoder candidate if LLVM is already a required dependency. It provides AArch64 instruction decoding/printing and avoids maintaining an entire opcode table immediately. It does not replace semantic lifting or control-flow analysis.
- **Capstone:** useful for a fast analysis prototype and human-readable disassembly. It is a disassembler, not a complete correctness specification for lifting.
- **Remill:** valuable research and prototype candidate because it lifts machine code to LLVM-oriented semantics. It should not become foundational until AArch64 coverage, generated-code size, debug mapping, exception behavior, and license integration are demonstrated with this project.
- **Custom decoder:** eventually appropriate for a deliberately limited, audited semantic subset or for handling gaps. It must have differential tests for every implemented instruction family.
- **Ghidra:** optional analysis and metadata-export tool. It is not a runtime dependency.
- **Binary Ninja / IDA:** optional analyst tooling only. They must not be required for users to build the open-source toolchain.

The first implementation should wrap one decoder behind an internal interface:

```cpp
struct DecodedInstruction {
    GuestAddress address;
    uint32_t opcode;
    InstructionId id;
    std::array<Operand, 5> operands;
    std::string disassembly;
};
```

The interface must preserve the original opcode and guest address even if a different decoder is used later.

## 7. Nintendo Switch executable loading

### 7.1 NSO model

Public reverse-engineering references describe NSO as the main Switch executable format. The NSO header identifies the `NSO0` magic, version and flags, file/memory offsets and sizes for `.text`, `.rodata`, and `.data`, BSS size, module identifier, compressed sizes, optional embedded/dynamic-string/dynamic-symbol regions, and per-segment hashes. Compressed segments have historically used LZ4; newer flags and system versions can change compression behavior. These details are version-sensitive and must be verified against the target file.

The parser must not assume that the presence, offset, or meaning of every optional field is identical across system versions.

Conceptual module layout:

```
NSO
├── NSO0 header
├── .text file payload → guest executable range
├── .rodata file payload → guest read-only range
├── .data file payload → guest read/write range
├── .bss → zero-filled guest read/write range
├── embedded metadata (if present)
├── dynamic strings/symbols (if present)
└── module metadata / MOD0-related structures
```

The exact target build may contain additional loader metadata or module-specific structures. **Needs verification.**

### 7.2 Parser and image-materialization responsibilities

The implemented NSO0 stages must:

1. Read little-endian fields with checked bounds and overflow-safe arithmetic.
2. Validate the magic, supported header version, file ranges, and non-overlap rules.
3. Record both file offsets and guest memory offsets without treating guest values as host pointers.
4. Decode compression and hash flags while preserving the metadata/header distinction.
5. Materialize uncompressed sections by exact-size copy and compressed sections through bounded raw LZ4.
6. Verify requested SHA-256 hashes over the materialized bytes and reject mismatches.
7. Represent BSS as an owned, explicitly zero-filled buffer with its guest offset.
8. Enforce configurable per-segment and total materialization limits before allocation.
9. Expose ZBIC as an explicit unsupported-compression error; it is not a standard zstd stream.
10. Produce a stable module report suitable for diffing in Git.

The current report contains at least:

```
module name
NSO header version and flags
module identifier/build id
.text/.rodata/.data file and guest ranges
compressed and materialized sizes
BSS range
hash status
compression and materialization status
BSS size and zero-initialization status
```

MOD0, dynamic tables, string/symbol tables, relocations, and entry/init/fini
candidates remain future work. The materializer intentionally returns an owned
host-side `NsoImage`; `NsoGuestLoader` consumes that image in the next layer and
does not make the parser or materializer aware of guest mappings.

The implementation records the format decisions against public references:
[Switchbrew's NSO0 description](https://switchbrew.org/wiki/NSO0),
[hactool's NSO0 implementation](https://github.com/SciresM/hactool/blob/master/nso.c),
and [upstream LZ4 v1.9.4's block API](https://github.com/lz4/lz4/blob/v1.9.4/lib/lz4.h).

### 7.2.1 Guest memory loader

Milestone 2B adds a portable logical guest memory layer. `GuestMemory` stores
sorted, non-overlapping half-open ranges in owned host-side byte vectors. A guest
address is a `uint64_t` value in the guest address domain; it is never converted
to a host pointer and no fixed host virtual address is reserved.

`load_nso` accepts an explicit module base and maps the materialized image as:

| Region | Guest base | Permissions | Backing |
| --- | --- | --- | --- |
| `.text` | module base + NSO text offset | `R-X` | materialized bytes |
| `.rodata` | module base + NSO rodata offset | `R--` | materialized bytes |
| `.data` | module base + NSO data offset | `RW-` | materialized bytes |
| `.bss` | module base + data end | `RW-` | zero-filled bytes |

All base-plus-offset and range calculations use checked 64-bit arithmetic.
Zero-sized segments are explicit no-ops. Reads and writes must be fully
contained in one mapping; adjacent mappings are valid but a single operation
does not cross their boundary. Writes enforce permissions, while executable
state and mapping metadata are queryable. The loader stages all four mappings
before committing them, so overlap, limit, and allocation failures leave an
existing `GuestMemory` unchanged.

The current implementation is intentionally not a page table, MMU, native
address mirror, or CPU memory subsystem. Milestone 5 adds an explicit
loader-time privileged write path for relocation application while preserving
normal guest write permission checks.

### 7.3 MOD0 and dynamic information — Milestone 3

MOD0 is a module metadata structure used by the Switch loader ecosystem and is described in public Switch research as a replacement for a conventional `PT_DYNAMIC` program header. The implementation treats the MOD0 layout as a versioned parser schema, not as an unchecked collection of fixed offsets.

The loader should resolve the chain in this order:

```
NSO header
  → decompressed module image
  → validated MOD0/module metadata
  → dynamic table and related ranges
  → strings, symbols, relocations, init/fini/TLS metadata
```

Milestone 3 implements the normal loaded-image chain. It reads the 8-byte
module-start slot at the loaded text base, derives the MOD0 address from its
`magic_offset`, validates the `MOD0` signature, and resolves the base MOD0
fields relative to the MOD0 header with checked signed arithmetic. The base
header records dynamic, BSS, exception-info, and runtime module-object
addresses. Later system-version extension ranges are opt-in because the public
layout uses the same post-header space for version-dependent data.

Dynamic entries are read explicitly as little-endian ELF64 records. The parser
requires `DT_NULL` within a configurable limit, preserves every raw entry,
allows repeated `DT_NEEDED`, rejects duplicate singleton tags, and retains
unknown tags for forward-compatible inspection. Switch NSO dynamic pointer
values remain module-relative until resolved into `GuestAddress` values.
`DT_STRTAB`, `DT_SYMTAB`, RELA, REL, and JMPREL metadata are range-checked
against `GuestMemory`; `Elf64_Rela` records preserve signed addends and expose
the ELF64 symbol/type split. No parser in this milestone writes guest memory.

The actual MOD0 fields, pointer bases, dynamic tags, and loader conventions for
the selected TOTK build remain **Needs verification** where the exact target
build could differ. A module report states which optional metadata is present.

### 7.4 Dynamic symbols and relocations — Milestone 5

The dynamic linking layer owns `DT_STRTAB`/`DT_STRSZ`, ELF64 `dynsym`, and RELA
metadata independently from the NSO parser. Symbol-table size is derived only
from validated `DT_HASH` or `DT_GNU_HASH` metadata and bounded by configured
limits. Symbol values remain module-relative until a resolver adds the guest
module base. Undefined imports are explicit and can be satisfied by a generic
external registry.

The semantic relocation pipeline supports the AArch64 ABI types `NONE`,
`ABS64`, `GLOB_DAT`, `JUMP_SLOT`, and `RELATIVE`. It uses checked `S + A` / `B + A`
arithmetic, writes little-endian guest values through
`GuestMemory::loader_write`, and stages all writes so a failed relocation does
not partially modify the image. REL, lazy binding, symbol versioning,
multi-module link order, and Horizon resolution remain future layers. See
[`RELOCATIONS.md`](RELOCATIONS.md).

### 7.5 Semantic IR and expanded AArch64 lifting — Milestones 6–8

Milestones 6, 7, and 8 provide the executable recompilation path without making
LLVM the architectural contract:

```
AArch64 → decoder/CFG → SwitchRecomp Semantic IR
                              ├── verifier/interpreter
                              └── LLVM lowering → native JIT
```

The IR is typed, deterministic, source-mapped, explicitly terminated, and
independent of LLVM at its public interface. `runtime::CpuState` models X0–X30,
SP, PC, NZCV, FPCR, FPSR, and the shared V0–V31 register file with correct W/X,
S/D/Q, and XZR/WZR semantics. Guest loads/stores go through checked
`GuestMemory` helpers and never reinterpret a guest address as a host pointer.
The lifter supports documented integer, NZCV, bitfield, conditional-select,
scalar-memory, pair-memory, PC-relative, FP, NEON, and internal-branch forms.
It rejects unsupported operand forms, divide instructions, fused FP multiply-
add, atomics, and system instructions explicitly. The interpreter and
optional LLVM backend execute synthetic standalone functions through the same
ABI, and `aarch64-analyze` provides deterministic whole-range coverage reports;
this does not execute TOTK.

### 7.6 Multiple modules

The architecture supports a module graph such as:

```
main
├── subsdk0
├── subsdk1
├── ...
└── sdk / runtime modules
```

This is a conceptual model, not a claim about the exact number or names of TOTK modules. The target manifest must enumerate only modules observed in the selected build.

Each module record should include:

- Logical name.
- Original module identifier/build ID.
- SHA-256 of the relevant input.
- Guest base and ranges.
- Segment protections.
- Entry/init/fini candidates.
- Dynamic metadata ranges.
- Exported/imported symbols.
- Relocations.
- Function metadata source and version.
- Whether it is recompiled, provided by the runtime, or currently unsupported.

## 8. Relocations and symbol resolution

Relocations must be fully resolved or explicitly represented before translated code can execute.

### 8.1 Relocation model

The AArch64 ELF ABI distinguishes static and dynamic relocations and defines relocation families for absolute values, PC-relative values, call/jump sites, GOT generation, TLS, and other purposes. NSO loader metadata may encode or reference a platform-specific subset. The implementation must parse the exact relocation records present in the target rather than assuming that every generic ELF feature appears in every NSO.

The internal form should preserve the original record:

```cpp
struct GuestRelocation {
    GuestAddress place;
    uint32_t type;
    int64_t addend;
    GuestAddress symbol_value;
    std::string symbol_name;
    ModuleId defining_module;
    RelocationStatus status;
};
```

### 8.2 Required initial cases

The first implementation should investigate and then cover, with fixtures:

- Relative relocations.
- Absolute 64-bit and 32-bit data relocations where present.
- PC-relative `ADR` / `ADRP`-family references.
- `ADD`/load/store sequences that materialize addresses.
- Call and jump relocations.
- GOT-like indirections.
- Symbol-based inter-module relocations.
- TLS-related relocations if the target uses them.
- Relocations into vtables, callback tables, jump tables, and global data.

The names RELA, GOT, and PLT are ELF concepts; the actual Nintendo loader representation may not be a conventional desktop PLT. The implementation must report the source format rather than forcing it into a desktop linker model.

### 8.3 Resolution algorithm

1. Parse and validate all module records.
2. Build a module-relative symbol index.
3. Match local definitions before external imports.
4. Resolve weak/optional symbols according to observed loader semantics.
5. Resolve inter-module references using the target manifest.
6. Apply relocations to guest memory or encode an equivalent generated reference.
7. Validate instruction-field ranges and alignment.
8. Record every applied relocation in a relocations log.
9. Freeze read-only pages only after relocation.
10. Refuse to start if a required relocation remains unresolved.

Preserving original guest-visible addresses is preferred. If the native code cannot encode a guest address directly, it must use a guest-address constant and runtime translation.

## 9. Guest address space and memory

### 9.1 Address type

```cpp
using GuestAddress = uint64_t;
```

A `GuestAddress` is an address in the original program's address domain. It is not automatically a host pointer and must not be cast directly to a function pointer.

```
guest virtual address
        |
        v
GuestMemory::translate()
        |
        v
native backing allocation
```

Generated code must use typed helpers or a controlled base mapping:

```cpp
void* translate(GuestAddress address, Access access);
```

### 9.2 Mapping options

| Model | Description | Advantages | Risks |
| --- | --- | --- | --- |
| Fixed-offset mapping | Reserve a host range and use `host = base + guest_address` or a rebased equivalent | Fast loads/stores, simple generated code | Address collisions, ASLR, sparse reservations, host architecture differences, unsafe assumptions |
| Page-table mapping | Map guest pages to host allocations through a page index | Portable, sparse, protects gaps, useful for diagnostics | More overhead and a hot-path lookup |
| Hybrid | Reserve common ranges when possible, fall back to page translation for sparse/exceptional regions | Fast common path with correctness fallback | More implementation and testing complexity |

Host ASLR must be treated as normal behavior. A fixed reservation can fail because the requested range is occupied, and the loader must not disable host security features or silently relocate guest addresses without updating every guest-visible reference. The selected mapping mode and any rebasing must be recorded in diagnostics.

### Decision

Start with a page-aware memory manager and a clear translation API. Add a fixed-offset or rebased fast path only after tests prove that guest addresses, protections, and diagnostics remain correct.

Milestone 2B implements the first portable slice of that plan as a logical
sorted vector of host-backed regions. It validates half-open ranges, owns every
backing buffer, enforces R/W/X permissions, and deliberately does not expose a
guest-to-host pointer translation API. Page tables, page protections, sparse
backing, and native fast paths remain future runtime work.

The memory manager should support:

- Segment mapping from NSO metadata.
- Zero-filled BSS.
- Read/write/execute protection state.
- Guard pages for stacks and selected allocations.
- Sparse unmapped regions.
- Guest heap reservations.
- Guest thread stacks.
- TLS blocks.
- File-backed or streamed resource mappings where required.
- Memory fault diagnostics containing guest address and access type.
- Optional poisoning or watchpoints in debug builds.
- Explicit endianness and alignment helpers.
- Host allocation granularity and page-size abstraction.

On Windows, the eventual implementation can use `VirtualAlloc` and `VirtualProtect`; on POSIX systems, `mmap` and `mprotect` are candidates. These are proposed host backends, not current repository dependencies.

### 9.3 Pointer rules

- Guest pointers stored in guest memory remain `GuestAddress` values.
- A guest pointer is translated only at the boundary that accesses guest memory.
- A guest code pointer is resolved through the dispatcher, not executed as a host pointer.
- Native pointers returned by host APIs must never be written into guest memory unless the target ABI explicitly expects a compatible opaque handle representation.
- Guest structures are decoded with explicit layouts and alignment; host C++ layout is not used as proof of guest layout.
- Pointer provenance and null/invalid ranges must be checked in diagnostic builds.

## 10. CPU state and ABI

### 10.1 Architectural state

The minimum state model must be able to represent:

```
X0-X30
SP
PC
NZCV
FPCR
FPSR
V0-V31 (128-bit SIMD/FP registers)
```

Additional system registers or exception state are added only when observed or required by a test.

A debug/slow-path context can be:

```cpp
struct CpuState {
    uint64_t x[31];
    uint64_t sp;
    GuestAddress pc;
    uint32_t nzcv;
    uint32_t fpcr;
    uint32_t fpsr;
    Vector128 vreg[32];
};
```

This is a semantic model, not a requirement that every generated function permanently spill every register.

### 10.2 State representation decision

Start with correctness-oriented state helpers and use native locals/LLVM SSA within a basic block or function when liveness analysis proves it safe.

- **Literal context:** easiest to validate and diagnose; slower and larger.
- **SSA/native locals:** faster and easier for LLVM to optimize; requires correct handling of calls, flags, memory, exceptions, and ABI boundaries.
- **Recommended progression:** context at public/indirect/exception boundaries, SSA for proven local values, explicit spills at calls or slow paths.

A 32-bit write to `Wn` must apply AArch64 zero-extension semantics to `Xn`. SP is not interchangeable with X31. NZCV must be generated or materialized correctly for conditional consumers; lazy flags are allowed only when their dependencies are tracked.

### 10.3 Host call ABI

Use a stable internal generated-function ABI first, for example:

```cpp
using GuestFunction = void (*)(RuntimeContext&, CpuState&);
```

This universal ABI is intentionally conservative. Later, a function-signature pass can lower known direct calls to typed native signatures. Direct-call specialization must never change guest-visible behavior.

The implementation must document:

- Which registers carry arguments and returns.
- How floating-point/vector arguments are represented.
- How stack arguments are accessed.
- Which registers are preserved across a generated call.
- How PC/current-function metadata is updated.
- How a guest return differs from a host return.
- What happens when a function has an unresolved or unknown signature.

## 11. AArch64 decoding and translation

The translator should be organized by semantic families rather than by an unstructured opcode switch.

Required families include:

- Integer arithmetic and comparisons.
- Logical operations.
- Shifts and rotates.
- Multiply, multiply-accumulate, divide.
- Bitfield and bit-manipulation operations.
- Loads/stores, sign extension, pair operations, literals.
- Literal loads, including PC-relative LDR literal forms.
- Address formation: `ADR`, `ADRP`, add-immediate sequences.
- Conditional branches, unconditional branches, calls, returns.
- Conditional select and conditional compare.
- Floating-point arithmetic, conversions, comparisons, loads/stores.
- SIMD/NEON arithmetic, lane operations, permutations, and loads/stores.
- Atomics and exclusive load/store.
- Memory barriers.
- System-register accesses that affect observable behavior.
- Traps, SVC-like operations, and intentional unsupported instructions.

The translator should preserve:

```
guest address
raw opcode
decoded instruction
source basic block
generated operation range
runtime calls inserted
```

### 11.1 Unsupported instructions

There must never be a silent “return zero” or “skip instruction” fallback for an unsupported instruction.

```
UnsupportedInstruction {
    guest_pc: 0x...,
    opcode: 0x...,
    disassembly: "...",
    function: "...",
    module: "...",
    feature_family: "...",
    required_action: "implement | patch | runtime trap"
}
```

The tool should support three explicit modes:

- **Strict:** stop analysis/code generation at the first unsupported behavior.
- **Diagnostic:** emit a trap stub and continue producing a report.
- **Experimental:** allow an explicitly configured approximation, with a visible warning and runtime counter.

Only strict mode is acceptable for a correctness milestone.

## 12. Function discovery and control flow

A large stripped retail binary cannot depend only on symbols. Function discovery should combine:

1. Known module entry points and loader metadata.
2. Exported/imported symbol addresses.
3. Direct `BL` targets.
4. Direct branch targets that are valid code addresses.
5. Relocation references to code.
6. Exception/unwind metadata when present and understood.
7. Known prologue/epilogue patterns as heuristics only.
8. Vtable and callback references.
9. Jump-table target recovery.
10. Analyst-provided seeds from Ghidra or equivalent tooling.
11. Manual function boundaries and exclusions.

### 12.1 CFG traversal

For each seed:

- Decode exactly aligned AArch64 instructions.
- Classify fall-through, conditional, unconditional, call, return, indirect-branch, and trap edges.
- Track block boundaries caused by branch targets and intra-function entries.
- Distinguish call edges from tail-call edges.
- Mark data-reachable regions and stop decoding when evidence says a range is not code.
- Record unresolved indirect edges instead of inventing targets.
- Continue analysis to a fixed point.
- Compare inferred boundaries against metadata from other sources.
- Assign confidence and a reason to every function boundary.

A function may have multiple valid entry points or local labels. The metadata model must represent entry blocks separately from the canonical function name.

### 12.2 Hybrid analyst workflow

Automatic analysis is required for scale, but a hybrid workflow is more realistic:

```
Ghidra / analyst export
        +
static SwitchRecomp analysis
        +
manual overrides
        |
        v
versioned canonical metadata
```

The analyzer must not silently overwrite hand-reviewed metadata. Conflicts should be reported with both sources and a resolution field.

## 13. Metadata and Ghidra integration

### Decision

Use versioned JSON as the canonical machine-readable metadata format. TOML may be used for small human-authored configuration files, but generated analysis metadata should have one primary schema.

JSON is chosen because it is widely supported, diffable, schema-validatable, and capable of representing nested functions, edges, relocations, jump tables, imports, and confidence data. A formatter must produce deterministic key ordering and arrays where order is semantically stable.

Example shape:

```json
{
  "schema_version": 1,
  "game": "totk",
  "build": {
    "version": "TBD",
    "region": "TBD",
    "main_sha256": "TBD"
  },
  "modules": [],
  "functions": [],
  "symbols": [],
  "jump_tables": [],
  "relocations": [],
  "imports": [],
  "patches": []
}
```

The schema must support at least:

- Module name, build ID, hash, guest ranges, and protections.
- Function guest start/end and additional entries.
- Name, source, confidence, and analysis notes.
- Basic blocks and control-flow edges.
- Direct call targets and unresolved indirect-call sites.
- Jump tables and default targets.
- Symbols, aliases, vtables, RTTI observations, and globals.
- Import/export identities and required runtime bindings.
- Relocation records or references to the relocation report.
- Patch/hook declarations.
- Generated tool version and input hashes.
- Unsupported instruction records.
- Review status and analyst notes.

A minimal Ghidra exporter should export functions, labels, references, data ranges, jump tables, vtables/RTTI when identified, imports/exports, and function signatures only when the analyst has evidence for them. It must not fabricate C++ types from names.

## 14. Direct calls, indirect calls, function pointers, and vtables

### 14.1 Direct calls

A known `BL` target should become a direct native call when:

- The target function was translated successfully.
- The target belongs to a compatible loaded module or has a registered runtime binding.
- The generated ABI preserves required guest state.
- The call site does not depend on a return-address value or exception behavior that the specialization would remove.
- The call is recorded in the guest/native address map.

Direct calls reduce dispatcher overhead and give LLVM more optimization visibility. They must be disabled automatically when metadata is uncertain.

### 14.2 Indirect calls

A typical C++ sequence may load a function pointer from an object or vtable and execute `BLR`. The loaded value is a guest code address.

```cpp
using GuestAddress = uint64_t;
using GuestFunction = void (*)(RuntimeContext&, CpuState&);

GuestFunction lookup_guest_function(GuestAddress target);
void dispatch_guest_call(GuestAddress target,
                         RuntimeContext& runtime,
                         CpuState& state);
```

The target guest address remains observable and is used for diagnostics, profiling, patch matching, and callback identity.

### 14.3 Dispatcher design

Candidate structures:

| Strategy | Strength | Weakness |
| --- | --- | --- |
| Hash map | Simple and dynamic | Hash overhead and poor locality in hot calls |
| Sorted ranges + binary search | Compact and easy to validate | Multiple comparisons per call |
| Generated dense table | Very fast for dense addresses | Huge memory cost for sparse guest addresses |
| Perfect hash | Very fast for a fixed set | Regeneration and dynamic modules complicate it |
| Page-indexed table | One page lookup plus slot lookup, sparse-friendly | Requires address-space metadata and module updates |
| Address transform | Extremely cheap when layout permits | Depends on strong address assumptions |
| Linker-generated map | Can provide compact generated target metadata | Depends on the native toolchain and is not sufficient for dynamic modules by itself |

Recommended path:

1. Implement a validated sorted map for bring-up.
2. Add a page-indexed dispatch table for the common case.
3. Generate direct call edges for known targets.
4. Add a perfect-hash or generated table only after profiling a real workload.
5. Keep a slow diagnostic path that validates target range and module ownership.

Modules loaded or unloaded at runtime must update the dispatch structure atomically from the point of view of guest threads.

### 14.4 Callbacks and unknown signatures

For unknown callback signatures, use the universal `CpuState` ABI and a registration record describing:

- Guest target.
- Expected argument/register convention.
- Owning module.
- Lifetime and unload behavior.
- Whether the callback may re-enter guest code.
- Exception and thread constraints.

Do not use a C++ cast from an arbitrary address to an arbitrary typed function pointer.

## 15. Jump tables

Jump tables are a source of false function boundaries and invalid tail-call classifications.

The analyzer should recognize patterns involving:

- Bounds checks and default branches.
- `ADRP`/add address formation.
- Loads from read-only tables.
- Scaled index calculations.
- Absolute target tables versus PC-relative offset tables.
- Relocation-backed entries.
- `BR` through a computed target.
- Compiler-generated switch lowering variants.

Each recovered table should contain:

```table address
entry width
index expression
lower/upper bounds
default target
target list
addressing mode
relocation evidence
confidence
```

If the table can be proven complete, code generation may emit a native switch. Otherwise, retain a safe bounds check and route the target through the guest dispatcher. Never treat unvalidated table data as a host pointer.

## 16. Runtime organization

Proposed logical runtime:

```
runtime/
├── memory/
├── modules/
├── cpu/
├── calls/
├── threads/
├── synchronization/
├── tls/
├── time/
├── filesystem/
├── handles/
├── services/
├── input/
├── audio/
├── graphics/
├── logging/
└── diagnostics/
```

Each subsystem must have a small public interface and a testable implementation. The runtime should expose a service registry rather than hard-coding every import into generated functions.

A runtime context should contain:

- Module registry.
- Guest memory.
- Dispatcher.
- Thread/TLS registry.
- Handle table.
- Service registry.
- Virtual filesystem.
- Input/audio/graphics adapters.
- Logger and trace sinks.
- Build manifest and feature flags.
- Last guest PC/current function for diagnostics.

The runtime is intentionally smaller than a Switch emulator. Unsupported operations must identify the originating guest address, import/service identity, arguments when safe, thread, and module.

## 17. Horizon and Nintendo SDK compatibility

### Decision

Do not emulate all of Horizon OS. Implement only the behavior required by observed TOTK execution.

```
Implement on demand from observed TOTK behavior.
```

The initial compatibility strategy is:

1. Recover external imports and recognizable SDK boundaries.
2. Bind stable functions to native runtime implementations where semantics are simple.
3. Model handles, events, shared memory, and service calls only when the target uses them.
4. Instrument every unimplemented boundary.
5. Add a deterministic test for each implementation.
6. Keep version-specific bindings in TotkRecomp when their identity is not generic.

Potential domains include:

- Threads, events, mutexes, semaphores, and interrupt-like synchronization.
- IPC/service handles and request/response buffers.
- Shared memory.
- Timing and clocks.
- Filesystem and save data.
- HID/controller input.
- Audio.
- Account/profile queries.
- Applet/window behavior if required.
- GPU/graphics services if the chosen interception boundary requires them.
- SVC or low-level kernel interactions if actually reached.

SVC interception, service IDs, handle values, request layouts, and exact `nn::` symbols are **Needs verification**. Do not infer support from a name alone.

### 17.1 SDK code cases

There are two operational cases:

1. An SDK function remains externally identifiable and can be rebound by symbol/import metadata.
2. SDK code is statically linked or inlined into the game.

For case 2, use a combination of:

- Signature and pattern matching.
- Relocation and call-graph evidence.
- Known public research.
- Ghidra annotations.
- Function behavior tests.
- Selective patching only after the pattern is build-specific and reviewed.

Do not assume that a public SDK symbol name exists in a stripped retail build.

## 18. Filesystem and asset model

The project must never redistribute Nintendo game data. The runtime can consume files supplied by the developer/user from a legally obtained copy, subject to the applicable rights and project policy.

Proposed logical mounts:

```
romfs:/   read-only game data
save:/    writable user save data
cache:/   generated shader/resource caches
host:/    explicitly configured development files
```

Proposed host layout:

```
game/
user/
cache/
logs/
```

The exact dump/extraction layout is **Needs verification** and must be selected only after observing the target's expected paths and file formats.

The virtual filesystem must cover:

- Path normalization and mount translation.
- Read-only enforcement for `romfs:/`.
- Save-data isolation.
- File handles and lifetime.
- Directory enumeration.
- Synchronous and asynchronous reads.
- Large-file offsets and 64-bit sizes.
- Alignment and mapped-file behavior.
- Streaming and prefetch.
- Compression/decompression adapters where the game expects them.
- Deterministic errors and missing-file diagnostics.
- Optional file access tracing with guest callsite.

Performance work should wait until a representative loading trace exists. The first implementation should prioritize correct path and handle semantics.

## 19. Threading, TLS, synchronization, and atomics

### 19.1 Thread model

Map a guest thread to a native thread where possible:

```
guest thread
    ↓
native std::thread / platform thread
    ↓
guest TLS + guest stack + CpuState
```

The runtime should provide:

- Thread creation/join/exit.
- Guest stack allocation and guard pages.
- Thread names.
- TLS slot allocation and lookup.
- Priority and affinity as best-effort host mappings.
- Events, mutexes, semaphores, condition variables, barriers, and spinlocks.
- Deterministic test hooks.
- Thread registration and crash reporting.

A full guest scheduler is not an initial requirement. It becomes necessary only if traces prove that the game depends on scheduling behavior that cannot be represented by native threads and synchronization.

### 19.2 ARM64 memory ordering

AArch64 is a weakly ordered architecture. Host x86-64's stronger ordering does not make an incorrect translation correct, and host ARM64 builds must preserve the same semantics.

The translator must model:

- Acquire loads.
- Release stores.
- Acquire-release read-modify-write operations.
- Sequentially consistent operations.
- Full and directional barriers.
- Atomic compare-and-swap.
- Exclusive load/store sequences.
- Failure/retry behavior of `LDXR`/`STXR`-like operations.
- Alignment and size constraints.
- Volatile/device-like accesses if actually used.

Use LLVM atomic orderings and fences only after mapping the guest operation to the required semantics. A plain host load/store is not an acceptable replacement for an acquire/release or exclusive sequence merely because it passes a single-thread test.

### 19.3 Exclusive monitor strategy

Start with a runtime abstraction that can be differential-tested:

```cpp
ExclusiveToken begin_exclusive(GuestAddress address, size_t size);
bool store_exclusive(ExclusiveToken token,
                     GuestAddress address,
                     Value value);
```

A later optimized lowering may use host atomic instructions, but it must preserve invalidation when another guest/native thread writes the monitored location. The implementation must document whether the monitor is per-thread and what granularity is modeled.

## 20. Exceptions and unwinding

Exception support must be treated as a staged subsystem, not ignored.

Investigate whether the selected build uses:

- C++ exception tables and personality functions.
- `__cxa` or compiler runtime functions.
- Structured exception/unwind metadata.
- Longjmp/setjmp-like control flow.
- Signals or deliberate fault handling.
- Exceptions across generated-function boundaries.
- Destructors that must run during unwinding.

Initial milestones may prohibit entering an unsupported exception path, but they must produce a crash report that identifies the guest PC and handler metadata. Do not convert every exception into a process abort without measuring whether the game uses exceptions during initialization, resource loading, or gameplay.

A possible staged model:

1. Parse and report unwind/exception metadata.
2. Support normal generated calls and returns.
3. Support explicitly identified non-throwing regions.
4. Add guest-aware throw/catch and personality integration.
5. Validate destructor and thread interactions.
6. Only then enable broad optimization that removes context state or changes call boundaries.

## 21. Graphics architecture

### 21.1 End state

The objective is not:

```
TOTK
  → emulate a Maxwell GPU
  → Vulkan
```

The preferred end state is:

```
recompiled TOTK
  → chosen Nintendo graphics API boundary
  → TotkRecomp canonical render interface
  → native Vulkan backend
```

The exact boundary is **Needs verification**. Candidate layers include:

- A high-level `nn::gfx`-like abstraction, if meaningful calls survive and preserve enough state.
- NVN-facing calls, if the higher layer is inlined or does not expose enough information.
- Lower-level command/resource translation only as a fallback when higher-level interception is not viable.

### 21.2 Boundary decision

Choose the highest boundary that:

- Can be identified reliably in the exact target build.
- Preserves resource, shader, pipeline, synchronization, and presentation information.
- Avoids reproducing unnecessary hardware quirks.
- Can be reached from generated code without patching unbounded portions of the game.
- Supports deterministic tracing and replay.

A lower boundary may be required for correctness. It must not be chosen merely because its name is familiar.

### 21.3 Canonical render interface

The native renderer should consume a project-owned representation of:

- Device and queue operations.
- Buffers and images.
- Texture formats and views.
- Samplers.
- Render targets and depth/stencil attachments.
- Descriptor/resource bindings.
- Uniform and storage buffers.
- Vertex/index/indirect buffers.
- Graphics and compute pipelines.
- Blend, raster, depth/stencil, viewport, scissor, and multisample state.
- Barriers and layout transitions.
- Draw, dispatch, copy, clear, resolve, and present operations.
- GPU/CPU synchronization.

The interface should be traceable and replayable. It must not expose Vulkan types to game-specific code.

### 21.4 Backend sequence

- First backend: Vulkan, because it is explicit, cross-platform, and has mature validation tools.
- Later backends: D3D12 and Metal behind the same abstraction if real demand exists.
- macOS Vulkan support may use a translation layer; this is a deployment choice, not a promise of performance or feature parity.
- Do not implement three backends before the canonical interface and one backend pass conformance tests.

## 22. Shaders and pipelines

The proposed shader path is:

```
target shader binary
        ↓
format/ISA decoder
        ↓
project shader IR
        ↓
SPIR-V
        ↓
Vulkan module/pipeline
```

The exact source format, Maxwell instruction subset, register conventions, constant-buffer layout, and binding model for TOTK are **Needs verification**.

The shader subsystem must address:

- Shader binary identification and hashing.
- Vertex, fragment, geometry, tessellation, and compute stages if observed.
- Register and constant mapping.
- Texture/sampler bindings.
- Image and storage accesses.
- Derivatives and precision behavior.
- Barriers and shared memory for compute.
- Specialization constants.
- Clip/depth conventions.
- Pipeline state coupling.
- Reflection metadata.
- Validation errors with guest shader/pipeline identity.

Research candidates include Ryujinx, yuzu-derived public research, nouveau, envytools, and Maxwell documentation. They are references, not automatically compatible code dependencies. Each imported file or algorithm needs a license and provenance review.

SPIR-V is a binary intermediate language used by Vulkan and other Khronos APIs. The compiler should emit a pinned, validated SPIR-V version and run validation in development builds.

### 22.1 Pipeline cache

The eventual cache key should include:

```
game build hash
shader stage hashes
translated shader version
resource/binding layout
specialization values
render-target formats
pipeline state
backend/device identity where required
```

Use persistent caches, background compilation, and asset-driven precompilation only after correctness. Cache invalidation must be explicit whenever translator or renderer semantics change.

## 23. Input and audio

### 23.1 Input

Use a portable host input layer such as SDL behind a project-owned abstraction. The abstraction should eventually support:

- Xbox and PlayStation controller layouts.
- Switch-compatible controllers.
- Keyboard and mouse.
- Analog ranges and dead zones.
- Rumble.
- Gyro and accelerometer where available.
- Hot-plug and device identity.
- Rebindable actions.
- Deterministic input replay for tests.

Guest controller state must be represented in the format expected by the translated game/runtime boundary. Do not pass a host library's struct directly into guest memory.

### 23.2 Audio

Start with a small output/mixing abstraction and implement only observed requirements:

- Output device selection.
- Sample formats and channel layouts.
- Buffer submission.
- Streaming.
- Mixing.
- Synchronization with game time.
- Pause/resume and device loss.
- Diagnostics for underruns.

SDL Audio or another portable backend is a candidate. The exact Nintendo audio service behavior is **Needs verification**. Do not build a complete audio service model before an audio trace or boot dependency proves it is required.

## 24. Frame rate and timing

Separate these two goals:

```
native CPU recompilation
≠
game-logic timing modification
```

Recompiling CPU code does not automatically remove assumptions tied to a 30 FPS update loop.

Initially preserve timing semantics. Later, any frame-rate work must be isolated and testable:

- Delta-time calculation.
- Fixed-step simulation.
- Animation clocks.
- Physics.
- Camera interpolation.
- Particle updates.
- Presentation pacing.
- Cutscenes and scripted events.
- Input sampling.
- Audio synchronization.

A patch that increases presentation rate without proving simulation correctness is not a valid performance milestone.

## 25. Patches and hooks

Patches belong in TotkRecomp metadata and source, not in generic SwitchRecomp.

The patch system should eventually support:

- Whole-function replacement.
- Native runtime override.
- Single-instruction replacement.
- Mid-function hook.
- Call-site replacement.
- Import binding override.
- Diagnostic instrumentation.
- Renderer integration.
- Bug fixes.
- Separately staged timing/FPS patches.

Example conceptual metadata:

```toml
[patches]
schema_version = 1

[[patches.instruction]]
module = "main"
address = "0x..."
bytes = "..."
reason = "TBD"

[[patches.function]]
module = "main"
address = "0x..."
replacement = "totk::replacement_name"
reason = "TBD"
```

All patches must declare:

- Exact build hash or version range.
- Guest address or symbol identity.
- Original bytes/function hash when available.
- Replacement or hook.
- Preconditions.
- Expected behavior.
- Risk.
- Test or validation evidence.

Patches must fail closed if the original bytes do not match. Never apply an address-only patch to an unknown build.

## 26. Logging, tracing, and crash diagnostics

Structured observability is a core feature, not an afterthought.

Recommended categories:

```
CPU
MEMORY
RELOCATION
MODULE
ANALYSIS
CALL
HORIZON
SERVICE
FS
THREAD
SYNC
TLS
GPU
SHADER
AUDIO
INPUT
PATCH
PERFORMANCE
```

Example:

```
[CPU] Enter guest=0x7101234560 function=unknown_main thread=7
[HORIZON] Unimplemented service call id=... guest_pc=0x...
[MEMORY] Invalid read guest=0x... size=8 access=read
[GPU] Pipeline key=... stage=fragment
```

Unknown behavior must include enough context to reproduce it:

- Guest PC and current function.
- Native generated-function identity.
- Module and build hash.
- Thread/TLS identity.
- Arguments or register subset when safe.
- Recent service calls.
- Recent memory faults.
- Current mounts and resource path where relevant.
- Trace sequence number.

A crash report should include:

```
guest PC
native PC
guest registers
SP and guest stack bytes/frames
native stack
current guest function
thread and TLS
loaded modules
recent calls/services
recent memory faults
last patch/hook
renderer/shader state if applicable
```

The generated-code map must support:

```
native crash address
    → generated object/function
    → guest function
    → original guest instruction
```

## 27. Differential testing

Differential testing is one of the foundations of the project.

For a test function or instruction block:

```
reference AArch64 execution
        |
        | compare
        v
recompiled native execution
```

Compare where applicable:

- X0-X30.
- SP and PC.
- NZCV.
- FPCR/FPSR.
- V0-V31.
- Guest memory writes and byte ranges.
- Return values.
- Exceptions/traps.
- Atomic success/failure.
- Observable calls.
- Deterministic ordering for controlled tests.

Reference candidates:

- Unicorn for small instruction/function harnesses.
- QEMU-based user-mode or system harnesses.
- Native AArch64 execution on ARM64 hardware for selected tests.
- A small project-owned interpreter for instructions where reference ambiguity is unacceptable.

Reference engines are test tools, not runtime requirements. Their licenses and reproducibility must be recorded before integration.

Test design requirements:

- Generate or hand-author operands that cover edge cases.
- Mask intentionally unspecified state.
- Compare memory only within declared regions.
- Test alignment faults and page permissions explicitly.
- Test NaN/rounding/FP status behavior where required.
- Test branch conditions and 32-bit zero-extension.
- Test concurrent atomic patterns with deterministic schedules where possible.
- Store failing seeds, opcodes, inputs, and metadata versions.

Every instruction family must earn broader use by passing a corresponding test suite.

## 28. Unit and integration test layout

Proposed layout:

```
tests/
├── format/
│   ├── nso/
│   ├── mod0/
│   └── malformed/
├── relocations/
├── aarch64/
│   ├── integer/
│   ├── memory/
│   ├── branches/
│   ├── fp/
│   ├── neon/
│   └── atomics/
├── analysis/
├── memory/
├── dispatcher/
├── runtime/
├── horizon/
├── filesystem/
├── threading/
├── graphics/
├── shaders/
└── differential/
```

Fixtures must be synthetic, generated, or legally distributable. Do not commit commercial game binaries or copyrighted game assets.

Examples:

- Synthetic NSO-like headers with compressed and uncompressed sections.
- MOD0/dynamic fixtures covering known parser layouts.
- Relocation records with expected guest addresses.
- AArch64 snippets assembled from source with recorded toolchain version.
- Guest pointer/vtable dispatch tests.
- Page protection and invalid-access tests.
- Native thread/TLS/synchronization tests.
- Filesystem mount/path/async tests.
- Render IR replay tests.
- Shader IR/SPIR-V validation tests.

## 29. Build and generated-artifact strategy

The current repository uses CMake with pinned FetchContent dependencies for the
core toolchain. LLVM is an optional host-provided backend dependency.

### Proposal

- CMake as the build-system interface.
- Ninja for reproducible local/CI builds.
- Clang as the primary compiler for the first implementation.
- C++20 for core/runtime code unless a specific dependency forces another standard.
- LLVM 18.1.3 is the tested backend release (Ubuntu Noble package
  `1:18.1.3-1ubuntu1`) and is found through its pinned CMake package directory;
  it is not built through FetchContent.
- Optional SDL, Vulkan headers/loader, and analysis tools kept behind clear feature flags.
- CI initially validates formatting, unit tests, analyzer fixtures, strict translation tests, and at least one host build.

The minimum versions are recorded in `docs/DEPENDENCIES.md` and enforced by
the current CMake configuration where the dependency is host-provided.

Proposed targets:

```
switchrecomp-common
switchrecomp-format
switchrecomp-memory
switchrecomp-aarch64
switchrecomp-analysis
switchrecomp-ir
switchrecomp-lifter
switchrecomp-runtime-core
switchrecomp-interpreter
switchrecomp-codegen-llvm (optional)
nso-inspect
aarch64-analyze
aarch64-lift
```

The names are proposed. Repository naming should be settled once the first build exists.

Proposed build pipeline:

```
user-provided exact build
        ↓
hash/version validation
        ↓
NSO inspection
        ↓
metadata extraction and review
        ↓
control-flow analysis
        ↓
semantic IR generation
        ↓
LLVM IR/object generation
        ↓
native linking
        ↓
TotkRecomp executable and runtime
```

Keep generated output outside hand-written source trees:

```
build/generated/functions/
build/generated/metadata/
build/generated/shaders/
build/reports/
build/cache/
```

Generated files should be deterministic and keyed by:

- Source module hash.
- Metadata hash.
- Translator version.
- LLVM/toolchain version.
- Renderer/shader translator version.
- Relevant configuration.

Do not commit gigantic generated sources without a measured reason, a reviewable regeneration procedure, and a storage strategy.

## 30. Host targets

No host platform is established by the current repository.

Recommended order:

1. Windows x86-64 as the first user-facing PC target.
2. Linux x86-64 for development/CI and portability.
3. Windows ARM64 and Linux ARM64 after the semantics and runtime are stable.
4. macOS ARM64 only after renderer features, windowing, and toolchain support are tested.

The C++ runtime and metadata tools should remain as portable as practical. Platform-specific windowing, memory protection, thread priority, audio, and graphics code must be isolated.

### 30.1 Native ARM64 hosts

ARM64 hosts are important but not an initial shortcut. A guest ARM64 instruction can sometimes lower efficiently to host ARM64, but the project still must handle:

- Guest versus host address domains.
- Relocations and module registration.
- Guest ABI and runtime services.
- Switch-specific system behavior.
- Guest memory permissions and diagnostics.
- Graphics, audio, input, and timing dependencies.

Any “reuse original instruction” optimization must be a later lowering choice validated against the same semantic IR.

## 31. Dependency and license policy

The repository has no current dependencies. The following are candidates or research inputs, not approved imports.

| Component | Intended use | Observed/public license reference | Policy |
| --- | --- | --- | --- |
| LLVM | Decoder support, IR, optimization, object generation | Apache 2.0 with LLVM Exceptions in LLVM's license file | Pin version and ship required notices |
| Capstone | Prototype disassembly | BSD-style license in the project license file; current repository also contains LLVM-related license files | Use only through a reviewed pinned version |
| Remill | Optional lifting research/prototype | Apache 2.0 license in its repository | Do not make foundational until coverage and integration are proven |
| Ghidra | Analyst metadata export | Apache 2.0 | Optional external tool, never required at runtime |
| Unicorn | Differential test reference | GPLv2 in its repository | Keep test-only unless distribution implications are explicitly reviewed |
| SDL | Input/audio/windowing candidate | zlib-style license in the project license file | Pin and include notices if integrated |
| Vulkan-Headers | Vulkan API declarations | Repository documents Apache-2.0/MIT-covered files | Review file-level notices and pin version |
| QEMU | Differential/reference harness candidate | Version- and component-dependent licensing | Keep test-only until exact components are reviewed |
| ARM ABI documents | Format/ABI reference | Document-specific license | Link to the source; do not copy large text into the repository |
| Switchbrew/public research | NSO/MOD0 research reference | Site-specific terms and provenance | Use as a reference; verify independently against fixtures |

Rules:

- Record exact version, commit, license files, and transitive notices for every dependency.
- Do not copy code because it is present in a reference emulator or recompiler.
- Do not mix GPL code into a runtime or distributed tool without an explicit compatibility decision.
- Keep research-only tools outside the release dependency graph when possible.
- Preserve attribution and notices.
- Recheck licenses when vendoring, patching, or upgrading.
- Use public technical facts and clean-room observations where code provenance is uncertain.

## 32. Comparison with reference projects

| Project | Guest CPU | Translation style | Runtime/model | Graphics lesson |
| --- | --- | --- | --- | --- |
| [N64Recomp](https://github.com/N64Recomp/N64Recomp) | MIPS | Literal C | Separate runtime; metadata, direct calls, indirect lookup, relocatable overlays | Keep tool and runtime separable |
| [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp) | MIPS | N64Recomp output plus patches | Game-specific runtime/config/patches; original assets supplied by user | Game integration is a separate engineering layer |
| [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) | Xbox 360 PowerPC | C++ | Explicit CPU state, direct calls, generated indirect-function lookup, analyzer metadata | Fast indirect calls and hybrid function analysis matter |
| [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) | Xbox 360 PowerPC | XenonRecomp/XenosRecomp output | Game runtime plus input, filesystem, shader, and native-port work | Native rendering can replace GPU emulation at the right boundary |
| [PS2Recomp](https://github.com/ran-j/PS2Recomp) | MIPS R5900 | C++ | Analyzer, recompiler, runtime, syscall/stub dispatch, tests | Keep bring-up tools and runtime responsibilities explicit |
| [BanjoRecomp](https://github.com/BanjoRecomp/BanjoRecomp) | MIPS | N64Recomp output | Game-specific patches and RT64-based integration | Exact game metadata and patches drive the port |
| [RecompOne](https://github.com/BlackLabelHQ/RecompOne) | PS1 MIPS | C# | Recompiler and runtime are separate .NET projects | A runtime can replace console services without vendor components |
| [SymphonyRecomp](https://github.com/BlackLabelHQ/SymphonyRecomp) | PS1 MIPS | RecompOne-based | Project-specific wrapper/config/patch workflow | Recompilation, decompilation, and patching are distinct activities |
| [skate3recomp](https://github.com/mchughalex/skate3recomp) | Xbox 360 PowerPC | Recompiled code plus extensive native code | Build manifests, title-update metadata, native D3D12/Vulkan rendering | Native renderer work can dominate game-specific engineering |
| TotkRecomp | AArch64 | Expanded Semantic IR lowered to optional LLVM backend (Milestone 7) | SwitchRecomp plus on-demand Horizon/runtime services (future) | Prefer a verified high-level boundary, Vulkan first |

This table records architectural patterns observed in public repository documentation and layouts. It does not claim that their internal implementations are portable to AArch64 or legally reusable.

## 33. Legal and repository hygiene

The repository and releases must not contain:

- TOTK executables or NSO files.
- Nintendo game assets or extracted resource archives.
- Nintendo firmware.
- `prod.keys`, title keys, or other decryption keys.
- Nintendo SDKs or proprietary libraries.
- Secrets, private dumps, or user-specific credentials.
- A tool whose purpose is to defeat encryption or DRM as part of the distribution workflow.

The intended distribution model is original project code and legally distributable metadata/tools. Users/developers provide their own legally obtained game data through a workflow that does not require the project to redistribute Nintendo content.

The loader should consume an already legally obtained and appropriately prepared/decrypted input supplied by the developer or user when that is required by the target format. It must not distribute title keys, implement key extraction, or add decryption/DRM-circumvention helpers to the project.

The repository may contain:

- Synthetic fixtures.
- Hashes and metadata that are legally appropriate to publish.
- Original source code.
- Open-source dependencies with compatible licenses.
- Documentation and links to public technical references.
- Generated caches only when they contain no protected game content and their provenance is clear.

This section is a repository policy, not a claim about the legal status of any particular jurisdiction or use.

## 34. Required milestone roadmap

The milestones below are gates. Do not skip a gate because the next one appears more exciting.

### Milestone 0 — Repository foundation

**Goal:** Establish layout, build system, dependency policy, CI, logging, coding conventions, and documentation.

**Success:** A clean build and deterministic test command on at least one supported development platform.

### Milestone 1 — Strict NSO0 header inspection

**Goal:** Implement a bounds-safe parser for the fixed `0x100`-byte NSO0
header and expose it through `nso-inspect` for a legally supplied NSO.

**Implemented:** Header fields, three segment descriptors, offsets, virtual
ranges, compression/hash status, build/module ID, BSS, file and memory overlap
checks, and contained RoData-relative embedded/DynStr/DynSym ranges.

**Still not implemented at this layer:** ZBIC decoding. MOD0 discovery,
dynamic symbols, and relocations are implemented by later milestones.

**Success:**

```bash
nso-inspect <user-provided-main>
```

produces a trustworthy, diffable report and rejects malformed input without
attempting decompression or execution.

### Milestone 2A — NSO image materialization and integrity

**Implemented:** Materialize `.text`, `.rodata`, and `.data` with exact-size
copy/decompression checks, verify requested SHA-256 hashes over materialized
bytes, and expose explicit zero-filled BSS under configurable allocation limits.
ZBIC is intentionally unsupported.

### Milestone 2B — Guest memory loader

**Implemented:** Consume `NsoImage` using an explicit module base and create
checked, owned, non-overlapping guest mappings for `.text`, `.rodata`, `.data`,
and zero-filled BSS with R/W/X permission checks.

**Success:** Synthetic NSO bytes pass through parsing, materialization, guest
loading, MOD0/dynamic discovery, and checked address-based reads/writes. The
metadata parser does not apply relocations.

### Milestone 3 — MOD0 and dynamic metadata discovery

**Implemented:** Locate and report validated MOD0 and dynamic metadata from the
guest image without applying relocations. Synthetic fixtures cover malformed
headers, signed offsets, bounded dynamic termination, unknown/duplicate tags,
range validation, RELA decoding, and transactionality.

### Milestone 4 — AArch64 decoding and control-flow analysis

**Implemented:** Decode executable guest bytes through the pinned Capstone
backend behind a SwitchRecomp-owned AArch64 representation. The layer preserves
guest addresses, opcodes, normalized operands/register identities, conditions,
memory operands, PC-relative values, disassembly, and explicit decoder status.
The CFG analyzer performs bounded intraprocedural traversal with deterministic
basic blocks, typed edges, direct call candidates, unresolved indirect flow,
checked target arithmetic, executable-memory validation, range limits, and
block splitting.

**Success:** Synthetic AArch64 fixtures cover representative scalar, memory,
FP/SIMD, atomic, system, branch, trap, malformed, and CFG cases without
silently treating unknown control flow as fallthrough. Semantic lifting and
execution remain separate milestones; relocation application is implemented in
Milestone 5.

### Milestone 5 — Dynamic symbols and relocations

**Implemented:** Bounded dynamic strings/symbols, imports and defined symbols,
generic symbol resolution, and transactional AArch64 `NONE`, `ABS64`,
`RELATIVE`, `GLOB_DAT`, and `JUMP_SLOT` relocation application.

**Success:** Loaded synthetic modules can resolve supported symbols and apply
checked relocations without partially modifying guest memory.

### Milestone 6 — Semantic IR and minimal AArch64 lifting

**Goal:** Lift simple standalone functions such as add/return, loads/stores, and branches.

**Success:** Native results match the AArch64 reference harness.

**Implemented:** An LLVM-independent typed Semantic IR, deterministic printer,
structured verifier, explicit CPU state, initial scalar AArch64 lifter,
reference interpreter, checked guest-memory runtime boundary, and optional LLVM
lowering/JIT execution for synthetic standalone functions.

### Milestone 7 — Expanded AArch64 semantics and coverage

**Goal:** Expand integer, memory, flag, branch, address-formation, and
conditional-select families, then measure normalized opcode coverage over a
locally supplied executable range.

**Success:** The tested subset passes deterministic state and memory fixtures;
the interpreter and optional LLVM backend lower the same typed IR; coverage
reports are deterministic and never include private input paths or game data.

### Milestone 8 — AArch64 FP/SIMD — implemented

**Implemented:** A shared V0–V31 `Vector128` register file, FPCR/FPSR state,
project-owned scalar FP and NEON IR, checked S/D/Q vector memory, reference
interpreter semantics, and LLVM runtime-helper lowering. The support boundary is
documented in `docs/MILESTONE_8.md`.

**Success:** Synthetic scalar and vector fixtures agree across the interpreter
and optional LLVM backend, including raw IEEE edge cases and checked memory.

### Milestone 9 — Threads and atomics — implemented

**Implemented:** Joinable native guest threads with stable project-owned IDs,
per-thread `CpuState`/TLS and exclusive monitors, checked synchronized shared
memory, acquire/release order, deterministic 64-byte exclusive reservations,
barriers, runtime ABI helpers, normalized M9 Semantic IR, reference interpreter
execution, and optional LLVM 18 helper lowering. Pair-exclusive/LSE forms,
WFE/WFI, Horizon services, and function-map dispatch remain explicit future
boundaries.

**Success:** Controlled multi-threaded workloads pass deterministic tests with
structured failures for unsupported synchronization patterns.

### Milestone 10 — Whole-main translation

**Goal:** Analyze and translate the targeted `main` module after function-map,
threading, import, and runtime boundaries are validated.

**Success:** The known synthetic and legally supplied module corpus translates
with auditable coverage and no silent unsupported instructions.

### Milestone 13 — Enter game initialization

**Goal:** Execute the recompiled entry path.

**Success:** Execution reaches initialization and stops at a known unsupported dependency, with a useful diagnostic.

### Milestone 14 — Runtime bring-up

**Goal:** Add required memory, timing, threading, filesystem, handle, and service behavior incrementally.

**Success:** Initialization progresses consistently between runs.

### Milestone 15 — Filesystem and asset loading

**Goal:** Mount user-provided game data and implement the required streaming path.

**Success:** TOTK opens required resources and begins loading without missing-path or handle-semantics failures.

### Milestone 16 — Graphics initialization

**Goal:** Trace and intercept the selected graphics boundary and create logical native resources.

**Success:** Recompiled code creates the required native device/resources through the canonical render interface.

### Milestone 17 — First present

**Goal:** Produce a window, swapchain, render target, and present path.

**Success:** A frame initiated by recompiled code reaches the display. A cleared framebuffer counts.

### Milestone 18 — First visible game output

**Goal:** Render actual TOTK graphics, even if incomplete.

**Success:** Game-generated geometry or UI is visible through the native renderer.

### Milestone 19 — Menu boot

**Goal:** Reach an interactable title screen or early menu.

**Success:** Input works and the menu remains stable across repeated runs.

### Milestone 20 — In-game

**Goal:** Reach a playable scene.

**Success:** Gameplay begins with known limitations documented.

### Milestone 21 — Correctness

**Focus:** Crashes, memory, synchronization, graphics, audio, saves, streaming, input, timing, and gameplay behavior.

**Success:** A repeatable validation suite covers representative flows and regressions.

### Milestone 22 — Performance

**Focus:** Direct-call lowering, dispatcher locality, LLVM optimization, SSA/register improvements, guest-memory fast paths, allocation, renderer batching, shader/pipeline caches, and asynchronous compilation.

**Success:** Performance improvements are measured against a fixed workload without regressions in the correctness suite.

## 35. Risks

| Risk | Severity | Why it matters | Mitigation |
| --- | --- | --- | --- |
| AArch64 semantic error | Critical | Corrupts all downstream behavior | Differential tests, strict unsupported traps, raw opcode mapping |
| Wrong function boundaries | Critical | Invalid CFG and calls | Hybrid analysis, confidence metadata, manual review |
| Guest address misuse | Critical | Memory corruption or invalid host execution | Typed guest addresses, checked translation, no pointer casts |
| Indirect calls/vtables | Critical | C++ code depends on them heavily | Dispatcher, target maps, direct-call specialization only when proven |
| Atomics/orderings | Critical | Rare, nondeterministic thread failures | LLVM memory semantics, exclusive-monitor tests, deterministic schedules |
| FP/NEON mismatch | Critical | Physics, animation, rendering, and math diverge | Reference execution, FP status/rounding tests |
| Static SDK code | High | Imports may not remain identifiable | Pattern/signature analysis, behavior tests, version-specific metadata |
| Horizon API surface | High | Boot may reach many services | On-demand implementation, service traces, fail-loud diagnostics |
| Graphics boundary choice | Critical | Wrong abstraction can require a GPU emulator | Trace candidate boundaries, choose highest viable boundary |
| Shader translation | Critical | No visual output or wrong rendering | Shader IR, validation, golden tests, license review |
| Streaming/filesystem | High | Open-world loading depends on it | Async I/O tests, path traces, cache/streaming instrumentation |
| Exceptions/unwinding | High | Initialization or C++ cleanup may fail | Inventory metadata, staged support, guest-aware crash reports |
| Exact-build drift | High | Addresses and patches become invalid | SHA-256 manifests, original-byte checks, versioned metadata |
| Build/output scale | High | Huge generated code slows iteration | Incremental generation, object grouping, caches, stable metadata |
| Debugging scale | Critical | Failures are otherwise opaque | Guest/native maps, structured logs, replayable traces |
| License incompatibility | High | Prevents clean redistribution | Pin provenance, isolate GPL test tools, review notices |
| Legal content leakage | Critical | Repository/release risk | Ignore rules, CI secret/content checks, user-supplied data only |
| Premature FPS changes | Medium | Breaks physics/timing | Preserve timing first; isolate and test patches |
| Host platform variance | Medium | Different memory/ABI/GPU behavior | Backend abstraction, CI matrix, capability reports |

## 36. Open questions

1. Which exact TOTK version, region/build, and update will be the first supported target?
2. What are the SHA-256 hashes and module identifiers for that build?
3. Which modules are present, and which are actually required to reach initialization?
4. Which NSO compression flags and MOD0 layout variants appear in the selected build?
5. Which dynamic tags and AArch64 relocation types are present?
6. How much code is stripped, and what symbols or unwind metadata remain?
7. How much Nintendo SDK code is statically linked or inlined?
8. Which AArch64 system registers and SVC/service paths are actually reached?
9. Does the game use exceptions during initialization, loading, or gameplay?
10. Which guest address ranges must preserve identity, and can a practical host reservation support a fast mapping?
11. Is a universal `CpuState` ABI sufficient for all callbacks, or are typed ABI shims required earlier?
12. Which dispatcher strategy wins on a real TOTK trace: sorted map, page index, generated table, or another design?
13. Which graphics boundary preserves enough semantic information: high-level `nn::gfx`-like calls, NVN-facing calls, or a lower boundary?
14. Does the target retain recognizable graphics API imports after linking?
15. What shader binary format and Maxwell instruction subset are present?
16. Which shader translation components can be used under compatible licenses?
17. What filesystem layout, archive format, compression, and streaming behavior does the target require?
18. Which input devices and motion sensors should be supported in the first playable milestone?
19. What audio service and latency behavior are required?
20. Which host is the first supported target: Windows x86-64, Linux x86-64, or another platform?
21. Which LLVM release provides the required AArch64 decoder/API stability and acceptable output size?
22. Can Remill cover enough of the required AArch64 semantics to justify a prototype?
23. What code and data must remain in guest memory for pointer identity and renderer interaction?
24. Which parts of the reference projects are research only, and which can be cleanly depended on?
25. What metadata should be manually reviewed before allowing whole-module translation?
26. What is the minimum deterministic boot trace that proves a runtime change is correct?

## 37. Immediate Next Steps

These are the first practical engineering tasks. They intentionally stop before production recompiler implementation.

### 1. Freeze the first exact target build

- **Goal:** Define one supported TOTK executable and module set.
- **Inputs:** Legally obtained developer-provided build; local-only version information.
- **Output:** A private build manifest containing version/region/build ID, module names, and SHA-256 hashes.
- **Success:** A loader can reject a different or incomplete build.
- **Dependencies:** None; this is the project’s first external input decision.

### 2. Establish repository foundation

- **Goal:** Add the initial CMake layout, C++ standard, formatter, test runner, CI, and dependency policy.
- **Inputs:** This RFC and the selected host/toolchain.
- **Output:** A minimal build with logging and one passing test.
- **Success:** Clean configure/build/test from a fresh checkout.
- **Dependencies:** Target host decision.

### 3. Implement NSO header parsing

- **Goal:** Read validated NSO headers without decompressing the full module.
- **Inputs:** Synthetic fixtures and a locally supplied target file.
- **Output:** Parser library and structured report.
- **Success:** Correct fields, bounds checks, unknown flag reporting, and malformed-file tests.
- **Dependencies:** Milestone 0.

### 4. Implement bounded section decompression and hash checks

**Completed in Milestone 2A.**

- **Goal:** Reconstruct decompressed `.text`, `.rodata`, and `.data` when required.
- **Inputs:** Compression flags and synthetic compressed fixtures.
- **Output:** Decompression utility with hash verification.
- **Success:** Decompressed bytes and reported hashes match known fixtures.
- **Dependencies:** NSO parser; reviewed compression dependency.

### 5. Parse MOD0 and dynamic metadata

**Future Milestone 3.**

- **Goal:** Locate and report module/dynamic metadata using a versioned schema.
- **Inputs:** Target module and public format research.
- **Output:** MOD0/dynamic parser and warnings for unsupported fields.
- **Success:** A trustworthy report of parsed ranges, symbols, and relocation records.
- **Dependencies:** NSO loader; exact-build manifest.

### 6. Build `nso-inspect`

- **Goal:** Make the inspection pipeline useful before any lifting exists.
- **Inputs:** Parser libraries and manifest.
- **Output:** Stable text/JSON reports with exit codes.
- **Success:** Two runs on the same input produce identical reports.
- **Dependencies:** Steps 3–5.

### 7. Add synthetic NSO/MOD0/relocation fixtures

- **Goal:** Prevent format work from relying only on a commercial target.
- **Inputs:** Hand-authored or generated fixtures.
- **Output:** Unit tests for valid, compressed, relocated, truncated, overlapping, and unknown-field cases.
- **Success:** Tests cover every implemented parser branch.
- **Dependencies:** Parser implementation.

### 8. Implement guest memory abstraction

**Completed in Milestone 2B.**

- **Goal:** Model guest ranges, protections, BSS, and checked address-based access.
- **Inputs:** Module report and synthetic memory layouts.
- **Output:** `GuestAddress`, `GuestMemory`, mapping/protection tests.
- **Success:** Valid reads/writes work; invalid/unmapped/protected accesses fail with guest diagnostics.
- **Dependencies:** NSO report.

### 9. Load a module and apply initial relocations

**Implemented in Milestone 5.**

- **Goal:** Reconstruct a test module in guest memory.
- **Inputs:** Synthetic module and supported relocation records.
- **Output:** Loader state and relocation log.
- **Success:** Expected guest-visible pointers and instruction fields are present before execution.
- **Dependencies:** Guest memory and relocation parser.

### 10. Integrate an AArch64 decoder — implemented in Milestone 4

- **Goal:** Decode aligned instructions behind the internal decoder interface.
- **Inputs:** Executable `GuestMemory` bytes and synthetic raw opcodes.
- **Output:** SwitchRecomp-owned decoded instructions preserving address,
  opcode, normalized operands, PC-relative values, and explicit flow status.
- **Backend:** Capstone v5.0.3, pinned and isolated behind the public wrapper.
- **Success:** Representative decoder and CFG tests are deterministic; unknown
  control flow is never silently treated as fallthrough.

### 11. Define canonical function metadata

- **Goal:** Version function, block, symbol, jump-table, relocation, and patch schemas.
- **Inputs:** Analyzer requirements and Ghidra export needs.
- **Output:** JSON schema plus validator and sample metadata.
- **Success:** Metadata can be reviewed, diffed, and rejected when hashes do not match.
- **Dependencies:** NSO reports and decoder.

### 12. Build the smallest CFG analyzer — implemented in Milestone 4

- **Goal:** Analyze synthetic functions seeded by known entry points and direct branches.
- **Inputs:** AArch64 decoder and executable synthetic guest mappings.
- **Output:** Basic blocks, typed edges, call candidates, unresolved indirect
  flow, deterministic diagnostics, and bounded traversal results.
- **Success:** Known synthetic CFGs match expected metadata and graph invariants.
- **Dependencies:** Decoder and guest memory; function metadata remains future work.

### 13. Export analyst metadata

- **Goal:** Prototype a minimal Ghidra exporter for functions, symbols, ranges, and references.
- **Inputs:** Synthetic/legally supplied analysis project.
- **Output:** Versioned JSON accepted by the validator.
- **Success:** Import/export round trips preserve addresses and review annotations.
- **Dependencies:** Metadata schema.

### 14. Lift one trivial function — implemented in Milestone 6

- **Inputs:** Functions such as `add x0, x0, x1; ret`.
- **Output:** Verified Semantic IR and optional native JIT execution.
- **Success:** Interpreter and native execution match the reference harness.
- **Dependencies:** Decoder, IR, and runtime context.

### 15. Expand only under differential coverage — Milestones 7, 8, and 9 implemented

- **Goal:** Add each architectural group only after differential coverage is available. FP/SIMD is implemented in Milestone 8, and the controlled native-thread/atomic subset is implemented in Milestone 9; function-map dispatch, pair-exclusive/LSE semantics, and Horizon services remain gated future work.
- **Inputs:** Differential tests and failure corpus.
- **Output:** Increasingly capable semantic IR/code generator and versioned coverage corpus.
- **Success:** Each group passes tests before it is enabled for larger analysis.
- **Dependencies:** All prior test infrastructure.

Only after these steps should whole-module TOTK translation be attempted. The
repository remains intentionally synthetic and does not claim game execution.

## 38. Reference sources

The following sources were inspected for patterns or technical reference while preparing this RFC:

### Recompilation projects

- [N64Recomp](https://github.com/N64Recomp/N64Recomp)
- [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp)
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp)
- [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp)
- [PS2Recomp](https://github.com/ran-j/PS2Recomp)
- [BanjoRecomp](https://github.com/BanjoRecomp/BanjoRecomp)
- [RecompOne](https://github.com/BlackLabelHQ/RecompOne)
- [SymphonyRecomp](https://github.com/BlackLabelHQ/SymphonyRecomp)
- [skate3recomp](https://github.com/mchughalex/skate3recomp)

### Format, ABI, compiler, and graphics references

- [Nintendo Switch Brew: NSO0](https://switchbrew.org/wiki/NSO0)
- [Nintendo Switch Brew: MOD](https://switchbrew.org/wiki/MOD)
- [Arm ELF for the AArch64 Architecture](https://github.com/ARM-software/abi-aa/blob/main/aaelf64/aaelf64.rst)
- [LLVM Language Reference](https://llvm.org/docs/LangRef.html)
- [Armv8 sequential consistency and memory ordering](https://developer.arm.com/community/arm-community-blogs/b/tools-software-ides-blog/posts/armv8-sequential-consistency)
- [Khronos SPIR-V Registry](https://registry.khronos.org/SPIR-V/)
- [LLVM Project license](https://github.com/llvm/llvm-project/blob/main/llvm/LICENSE.TXT)
- [Remill license](https://github.com/lifting-bits/remill/blob/master/LICENSE)
- [Ghidra license](https://github.com/NationalSecurityAgency/ghidra/blob/master/LICENSE)
- [Unicorn license](https://github.com/unicorn-engine/unicorn/blob/master/COPYING)
- [SDL license](https://github.com/libsdl-org/SDL/blob/main/LICENSE.txt)
- [Vulkan-Headers license](https://github.com/KhronosGroup/Vulkan-Headers/blob/main/LICENSE.md)

Reference links are for research and provenance. They do not grant permission to copy code or redistribute protected content.

## 39. Definition of done for this RFC

This RFC is complete when:

- The repository state is accurately described, including the initial bootstrap foundation.
- Existing, proposed, future, and verification-needed items are distinguishable.
- The architecture explicitly avoids becoming a full Switch emulator.
- NSO, MOD0, dynamic data, relocations, guest memory, CPU state, function discovery, indirect calls, jump tables, runtime services, files, threads, atomics, exceptions, graphics, shaders, input, audio, timing, testing, debugging, build, licensing, risks, milestones, open questions, and immediate tasks are covered.
- The next implementation task follows the current milestone roadmap and does not involve attempting to boot TOTK.
- Every target-specific unknown remains marked as **Needs verification**.
- Any future implementation claiming progress points back to a test, report, or exact-build artifact.
