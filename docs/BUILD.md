# Build instructions

TOTKRecomp uses an out-of-source CMake build and requires C++20.

## Configure, build, and test

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Ninja is preferred but not mandatory. With a multi-config generator such as
Visual Studio, select the configuration explicitly:

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

The default build enables tests. Disable them only when needed with
`-DTOTKRECOMP_BUILD_TESTS=OFF`.

For development diagnostics, sanitizers can be enabled on GCC/Clang builds:

```bash
cmake -S . -B build -G Ninja \
  -DTOTKRECOMP_ENABLE_SANITIZERS=ON
```

The normal build never accesses game files and never requires Nintendo keys or
other proprietary content.
