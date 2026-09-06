# Dynamic symbols and AArch64 relocations

Milestone 5 adds the generic linking layer between a loaded NSO image and the
future decoding/translation stages. It contains no TOTK addresses, symbols, or
Horizon handlers.

## Pipeline

```text
NSO → GuestMemory → MOD0 → .dynamic → dynstr/dynsym/RELA
    → semantic relocations → symbol resolution → relocated guest image
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
is checked and values are written as little-endian 64-bit guest data. Unknown
types remain inspectable and fail with `UnsupportedRelocationType`.

The numeric assignments and generic dynamic-relocation operations follow Arm's
official [AAELF64 specification](https://github.com/ARM-software/abi-aa/blob/main/aaelf64/aaelf64.rst);
the Switch loader model supplies the module-base/load-bias value used as `B`.

`DT_REL` is not treated as RELA. It is rejected explicitly because REL entries
do not carry the explicit addend used by this implementation.

## Resolution and weak symbols

`SymbolResolver` resolves current-module definitions as `module_base + st_value`,
then checks a deterministic external registry. The registry can later be
populated by other NSO modules or runtime/Horizon layers; no platform symbols
are hardcoded here.

Missing strong undefined symbols fail with `UndefinedStrongSymbol`. Missing weak
undefined symbols resolve to guest address zero while remaining visible in
`unresolved_imports()`. Duplicate external registrations fail explicitly.
Visibility is retained for future inter-module policy; a complete module-scope
visibility/link-order model is outside this milestone.

## Guest-memory write model and atomicity

Normal `GuestMemory::write()` continues to enforce final R/W/X permissions.
Relocation application uses the explicit loader-only `loader_write()` API, which
still validates overflow, mapping, and single-region containment but can write a
region that will be read-only during execution. The processor computes and
validates every relocation before committing bytes, so parse, resolution,
arithmetic, unsupported-type, and invalid-target failures leave the image
unchanged.

## CLI and limitations

`nso-dynamic-inspect` reports symbols, imports, and semantic RELA entries. Its
`--json` output uses guest addresses as hex strings and never exposes host
pointers.

REL tables, lazy PLT binding, complete RELRO, multi-module dependency graphs,
symbol versioning, TLS relocation semantics, Horizon emulation, semantic IR,
LLVM, and game-specific patches remain outside this milestone.
