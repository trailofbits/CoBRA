# Building CoBRA

## Prerequisites

- CMake 3.20+
- C++17 compiler (GCC 9+, Clang 10+, Apple Clang 12+)
- LLVM 19-22 (for the LLVM pass plugin; optional)

## Quick Start

```bash
# Step 1: Build dependencies
cmake -S dependencies -B build-deps -DCMAKE_BUILD_TYPE=Release
cmake --build build-deps

# Step 2: Build CoBRA
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH=$(pwd)/build-deps/install \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run tests
ctest --test-dir build --output-on-failure
```

## Superbuild Options

Pass these to the `cmake -S dependencies` step:

| Option | Default | Description |
|--------|---------|-------------|
| `USE_EXTERNAL_LLVM` | ON | Use system LLVM (OFF = build from source) |
| `USE_EXTERNAL_GOOGLETEST` | OFF | Use system GoogleTest (OFF = build from source) |
| `COBRA_ENABLE_Z3` | OFF | Enable Z3 support (requires lib/verify implementation) |
| `USE_EXTERNAL_Z3` | OFF | Use system Z3 (OFF = build from source) |

## Main Project Options

Pass these to the `cmake -S .` step:

| Option | Default | Description |
|--------|---------|-------------|
| `COBRA_BUILD_LLVM_PASS` | ON | Build the LLVM pass plugin (requires LLVM) |
| `COBRA_BUILD_TESTS` | ON | Build tests (requires GoogleTest in prefix) |

## CLI-Only Build (No LLVM)

```bash
cmake -S dependencies -B build-deps -DCMAKE_BUILD_TYPE=Release
cmake --build build-deps

cmake -S . -B build \
  -DCMAKE_PREFIX_PATH=$(pwd)/build-deps/install \
  -DCOBRA_BUILD_LLVM_PASS=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build
```
