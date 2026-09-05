# TOTKRecomp

TOTKRecomp is an experimental static-recompilation research project for one exact,
verified build of *The Legend of Zelda: Tears of the Kingdom*. The repository is
split conceptually between reusable `SwitchRecomp` infrastructure and
TOTK-specific target metadata.

## Current status

The repository bootstrap is implemented: C++20/CMake build targets, common
bounds-checked binary utilities, SHA-256 file validation, logging, versioned
target-manifest validation, tests, CI, and the `nso-inspect` CLI skeleton are
present.

No supported TOTK build is committed. The repository contains no game binaries,
keys, firmware, SDKs, or extracted game assets. The committed TOTK manifest is an
explicit `template` and contains no real hashes or Build IDs.

There is not yet a complete NSO parser, AArch64 decoder, semantic IR, LLVM
lowering, runtime, Horizon compatibility layer, renderer, or playable game.

## Build and test

Prerequisites:

- CMake 3.20 or newer;
- a C++20 compiler;
- Git for the pinned CMake `FetchContent` dependencies;
- OpenSSL development headers/libraries on non-Windows hosts.

Configure, build, and test with Ninja:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

The default generator also works:

```bash
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Run the current CLI skeleton:

```bash
./build/nso-inspect --help
./build/nso-inspect --version
```

On a multi-config generator, use `build/Debug/nso-inspect`.

## Documentation

- [Architecture RFC](docs/ARCHITECTURE.md)
- [Build notes](docs/BUILD.md)
- [Dependency policy](docs/DEPENDENCIES.md)
- [Target manifests and local configuration](docs/TARGETS.md)

## Legal/content boundary

The project does not distribute or require committed Nintendo copyrighted content.
Use only files you are legally entitled to inspect locally. Do not commit game
binaries, title keys, firmware, proprietary SDKs, Nintendo libraries, or extracted
game assets.

## Next milestone

Milestone 1 is strict `NSO0` header parsing and an expanded `nso-inspect` report,
using synthetic fixtures and user-provided local inputs only.
