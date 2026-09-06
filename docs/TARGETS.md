# Exact-build targets

TOTKRecomp is target-specific. Addresses, module relationships, patches, and
hashes must never be mixed between game builds. A public manifest describes the
exact build identity; local configuration points tools at privately held input
files.

## Manifest schema v1

The committed template is at
`targets/totk/template/manifest.json`. It is intentionally not a supported
target.

| Field | Required | Format and meaning | Placeholder policy |
| --- | --- | --- | --- |
| `schema_version` | Yes | Integer schema version. v1 is the only supported version. | Never `TBD`. |
| `game` | Yes | Logical game identifier; v1 expects `totk`. | Never `TBD`. |
| `support_status` | Yes | `template`, `experimental`, `supported`, or `unsupported`. | Never `TBD`. |
| `version` | Yes | Exact game version string. | `TBD` only for `template`. |
| `region` | Yes | Region or region/build qualifier. | `TBD` only for `template`. |
| `title_update` | Yes | Exact title/update identifier. | `TBD` only for `template`. |
| `minimum_tool_version` | No | Tool version required to consume the manifest. | Must be a non-empty string. |
| `notes` | No | Human-readable context. | Any UTF-8 string. |
| `modules` | Yes | Non-empty array of logical module records. | Never empty. |
| `modules[].name` | Yes | Unique logical module name, such as `main`. | Must not be empty. |
| `modules[].build_id` | Yes | Verified hexadecimal module Build ID when known. | `TBD` only for `template`. |
| `modules[].sha256` | Yes | Lowercase/uppercase 64-character SHA-256 string. | `TBD` only for `template`. |
| `modules[].expected_size` | No | Exact input file size in bytes. | Non-negative integer. |
| `modules[].expected_decompressed_size` | No | Optional decompressed size in bytes. | Non-negative integer. |
| `modules[].notes` | No | Module-specific context. | Any UTF-8 string. |

Schema v1 rejects unknown fields and unsupported future schema versions. This is
deliberate: a build manifest that is silently reinterpreted could select the
wrong executable.

`template` is the only status that permits `TBD`. `experimental` requires real
metadata but may still represent incomplete support. `supported` requires
complete verified metadata. `unsupported` is an explicit rejection state.

## Public versus local data

Public manifests contain logical identity and cryptographic metadata only. They
must not contain machine-local paths, extracted RomFS paths, cache locations, or
private analysis project locations.

Copy `config/local.example.json` to `config/local.json` and replace the example
paths locally. `config/local.json` is ignored by Git. Local configuration may
point to `main`, `subsdk*`, RomFS, cache, and analysis directories, but it is not
part of target identity.

## File matching

The bootstrap validator compares a local module against its manifest using file
existence, optional exact size, and SHA-256. Build ID extraction is intentionally
not claimed until NSO parsing exists. A template or unsupported manifest cannot
validate a file as a supported target.

Mixed modules are rejected by requiring every module to belong to the same
manifest and by checking each module's exact digest. A future loader will add
cross-module relationship checks after NSO metadata is available.

## Next milestone

Milestone 1 implements strict `NSO0` header parsing, segment metadata, compression
flags, hashes, BSS, and later MOD0 discovery. It must continue to use checked
arithmetic and user-provided local inputs.
