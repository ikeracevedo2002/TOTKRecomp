# Dynamic symbols and AArch64 relocations

Milestone 5 adds the generic linking layer between a loaded NSO image and the
future decoding/translation stages. It contains no TOTK addresses, symbols, or
Horizon handlers.

## Pipeline

```text
NSO → GuestMemory → MOD0 → .dynamic → dynstr/dynsym/RELA
    → semantic relocations → process guest-provider resolution → relocated guest image
```

`DynamicStringTable` owns a bounded copy of `DT_STRTAB`/`DT_STRSZ`. `get()`
rejects offsets at or beyond the table and requires a NUL terminator before the
table end. Names are byte strings; UTF-8 validation is intentionally not used.

`DynamicSymbolTable` decodes the 24-byte little-endian `Elf64_Sym` layout into
project-owned values. Binding, type, visibility, section index, raw module-
relative `st_value`, and size are retained. A symbol is defined when
`st_shndx != SHN_UNDEF`; otherwise it is exposed as an `ImportSymbol`.

The table is never scanned until arbitrary bytes look invalid. Its count is
derived from `DT_HASH` (`nchain`) or `DT_GNU_HASH` (the terminating chain bit),
with a configurable maximum. If both hashes are present, their counts must
agree. Missing or malformed bounds are explicit errors.

## RELA and supported types

Binary `Elf64_Rela` records are converted into `Relocation` values immediately.
The portable ELF64 `r_info` encoding is:

```text
symbol index = r_info >> 32
type         = r_info & 0xffffffff
```

The processor supports:

| Type | Numeric value | Formula |
| --- | ---: | --- |
| `R_AARCH64_NONE` | 0 | no write |
| `R_AARCH64_ABS64` | 257 | `S + A` |
| `R_AARCH64_GLOB_DAT` | 1025 | `S + A` |
| `R_AARCH64_JUMP_SLOT` | 1026 | `S + A` |
| `R_AARCH64_RELATIVE` | 1027 | `B + A` |

`S` is the resolved guest symbol address, `A` is the signed explicit addend,
`P` is the relocation target, and `B` is the module guest base. All arithmetic
is checked and resolved values are written as little-endian 64-bit guest data.
Unknown types remain inspectable and fail with
`UnsupportedRelocationType`. A supported symbol-backed relocation whose
global/weak symbol is undefined is instead represented by the diagnostic
relocation planner as an unresolved external boundary; it is never written as
zero or another synthetic value.

The numeric assignments and generic dynamic-relocation operations follow Arm's
official [AAELF64 specification](https://github.com/ARM-software/abi-aa/blob/main/aaelf64/aaelf64.rst);
the Switch loader model supplies the module-base/load-bias value used as `B`.

`DT_REL` is not treated as RELA. It is rejected explicitly because REL entries
do not carry the explicit addend used by this implementation.

## Resolution and weak symbols

`SymbolResolver` resolves current-module definitions as `module_base + st_value`,
then checks a deterministic external registry. The registry can later be
populated by other NSO modules or runtime/Horizon layers; no platform symbols
are hardcoded here. Its strict `resolve()` API reports an unresolved strong
symbol as `UndefinedStrongSymbol`, while `resolve_for_relocation()` returns a
typed unresolved result for valid undefined global/weak bindings so the
diagnostic planner can retain them without using an address. Duplicate
external registrations fail explicitly.
Visibility is retained for future inter-module policy; a complete module-scope
visibility/link-order model is outside the earlier module-local layer.

Milestone 13 adds `ProcessSymbolNamespace` above `SymbolResolver`. It indexes
eligible definitions from every supplied guest module with module identity,
symbol index, binding, type, visibility, section, raw value, and provider guest
address. The provider base is always used to compute `S`; the consumer base is
never substituted and host function pointers are never written to guest
memory. A unique strong provider resolves an undefined import, a single weak
provider resolves when no strong provider exists, and multiple plausible
providers remain ambiguous while Nintendo-specific lookup ordering is
unverified. Hidden/local/undefined definitions are not exported providers.

The process planner retains a binding record even after applying a
`R_AARCH64_JUMP_SLOT` or `R_AARCH64_GLOB_DAT`, including consumer module,
relocation source/index/slot, provider module/symbol/address, candidate count,
and resolution basis. Unresolved or ambiguous symbols remain guest import
boundaries and only then reach the runtime/HLE registry. This keeps guest
linker accounting separate from runtime-import accounting.

## Guest-memory write model and atomicity

Normal `GuestMemory::write()` continues to enforce final R/W/X permissions.
Relocation application uses the explicit loader-only `loader_write()` API, which
still validates overflow, mapping, and single-region containment but can write a
region that will be read-only during execution. The processor computes and
validates every relocation before committing bytes, so parse, resolution,
arithmetic, unsupported-type, and invalid-target failures leave the image
unchanged. `plan_relocations()` returns applied entries and unresolved import
boundaries; `apply_relocation_plan()` commits only the validated resolved
writes. The whole-module diagnostic loader uses this split, while strict
`apply_relocations()` retains its blocking behavior.

## CLI and limitations

`nso-dynamic-inspect` reports symbols, imports, and semantic RELA entries. Its
`--json` output uses guest addresses as hex strings and never exposes host
pointers.

REL tables, lazy PLT binding, complete RELRO, multi-module dependency graphs,
symbol versioning, TLS relocation semantics, Horizon emulation, semantic IR,
LLVM, and game-specific patches remain outside this milestone.
