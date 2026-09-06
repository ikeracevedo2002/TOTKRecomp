# Milestone 3 — MOD0 and dynamic metadata

Milestone 3 bridges a materialized NSO image and the metadata needed by the
future relocation stage. It only discovers, parses, and validates metadata. It
does not modify guest memory, resolve imports, execute code, or apply a
relocation.

## Address model

The public APIs use these domains explicitly:

- `GuestAddress`: an address in the loaded guest image; never a host pointer.
- MOD0 offsets: signed 32-bit values relative to the MOD0 header. Official
  Switch toolchains normally emit positive offsets, while signed storage also
  supports custom linker layouts.
- Dynamic pointer values: module-relative offsets in an NSO before relocation.
  `DynamicPointer` retains both the raw `module_offset` and resolved guest
  `address`.
- RELA `r_offset`: retained as a module-relative value and also exposed as a
  checked `target_address`.

The normal discovery path is deliberately narrow:

```text
loaded .text base
  → ModuleStart.magic_offset
  → MOD0 signature
  → MOD0.dynamic_offset
  → ELF64 dynamic entries
```

There is no memory-wide `MOD0` scan in the parser.

## MOD0

`discover_mod0` reads the two 32-bit words at the loaded text base and derives
the MOD0 address with checked unsigned arithmetic. A zero slot is reported as
optional metadata absence. A non-zero slot that is unmapped, overflows, or does
not contain the little-endian `MOD0` signature returns a structured error.

`parse_mod0` reads the base 0x1c-byte header with `GuestMemory` and explicit
little-endian decoding. It resolves and validates:

- dynamic table address;
- BSS start/end range;
- exception-info start/end range;
- runtime-generated module-object address.

All signed additions use `checked_add_signed_u64`, including negative offsets
and `INT32_MIN`. Optional post-header ranges for newer system versions can be
enabled through `Mod0ParseOptions`; they are disabled by default because the
same post-header area is version-dependent.

## Dynamic table

`parse_dynamic` reads `Elf64_Dyn` as two independent little-endian 64-bit
values. It requires `DT_NULL` before `max_dynamic_entries` and never follows an
unchecked host pointer. All raw entries are retained in order.

Known metadata includes string/symbol tables, RELA/REL tables, JMPREL/PLT
metadata, init/fini arrays, flags, dependencies, and common GNU/version tags.
Unknown tags are preserved and do not fail parsing. `DT_NEEDED` is the only
known repeatable tag. Repeated singleton tags such as `DT_STRTAB`, `DT_SYMTAB`,
or `DT_RELA` are rejected as ambiguous.

The parser validates:

- `DT_STRTAB`/`DT_STRSZ` and string-offset bounds;
- `DT_SYMTAB`/`DT_SYMENT`, with the ELF64 size of 24 bytes;
- `DT_RELA`/`DT_RELASZ`/`DT_RELAENT`, with the ELF64 size of 24 bytes;
- `DT_REL`/`DT_RELSZ`/`DT_RELENT`, with the ELF64 size of 16 bytes;
- `DT_JMPREL`/`DT_PLTRELSZ`/`DT_PLTREL`, currently requiring `DT_RELA`;
- guest readability, range containment, configured string and relocation
  limits, and divisibility of table sizes by entry sizes.

## RELA records

`parse_rela_table` and `parse_jmprel_table` read `Elf64_Rela` records without
casting guest bytes to a packed host struct. `RelaEntry` preserves `r_offset`,
`r_info`, and signed `r_addend`; `symbol_index()` and `relocation_type()` use
the ELF64 split (`r_info >> 32` and the low 32 bits).

The returned records are metadata only. No write path exists in this milestone.

## Limits and errors

Defaults are bounded to 65,536 dynamic entries, 1,048,576 relocations, and a
256 MiB dynamic string table. Callers can provide smaller limits. Errors retain
the existing `Result<T, Error>` model and distinguish unmapped memory,
permission failures, arithmetic overflow/underflow, malformed metadata,
unsupported PLT encodings, and resource limits.

## CLI

After normal NSO materialization and guest loading, `nso-inspect` reports MOD0
and dynamic addresses/counts. A valid image without a MOD0 slot reports:

```text
MOD0:
  found: no
  status: optional metadata absent
```

The CLI never claims relocation or execution support.

## Validation and next stage

Tests use only small synthetic guest mappings. They cover valid and malformed
MOD0, positive and negative offsets, overflow/underflow, cross-region reads,
dynamic termination limits, unknown and duplicate tags, string/symbol/RELA/
JMPREL validation, signed RELA addends, and GuestMemory immutability.

The next stage is Milestone 4: relocation application over `ModuleMetadata`,
`DynamicInfo`, `RelaEntry`, and `GuestMemory`. Symbol resolution, import
resolution, AArch64 decoding, XCI/NCA handling, keys, and runtime/HLE remain
out of scope.

## Technical references

- [Nintendo Switch Brew — MOD](https://switchbrew.org/wiki/MOD)
- [Arm — ELF for the Arm 64-bit Architecture](https://github.com/ARM-software/abi-aa/blob/main/aaelf64/aaelf64.rst)
- [System V ABI — Dynamic Linking](https://refspecs.linuxbase.org/elf/gabi4%2B/ch5.dynamic.html)
- [Atmosphère `rocrt` declarations](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libstratosphere/include/stratosphere/rocrt/rocrt.hpp)
