# CoBRA build system recipes
# Usage: just <recipe>

set dotenv-load := false

llvm_prefix := `brew --prefix llvm 2>/dev/null || echo /usr/local`
z3_prefix := `brew --prefix z3 2>/dev/null || echo /usr/local`
clang_format := `command -v clang-format 2>/dev/null || echo "$(brew --prefix llvm 2>/dev/null || echo /usr/local)/bin/clang-format"`
clang_tidy := `command -v clang-tidy 2>/dev/null || echo "$(brew --prefix llvm 2>/dev/null || echo /usr/local)/bin/clang-tidy"`
nproc := `sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 4`

# Default recipe: build and test
default: build test

# Build dependencies (GoogleTest, LLVM forwarding, Z3 forwarding)
deps:
    cmake -S dependencies -B build-deps \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="{{llvm_prefix}};{{z3_prefix}}" \
        -DCOBRA_ENABLE_Z3=ON \
        -DUSE_EXTERNAL_Z3=ON
    cmake --build build-deps -j {{nproc}}

# Configure the main project
configure: deps
    cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$(pwd)/build-deps/install;{{llvm_prefix}};{{z3_prefix}}"

# Build the project
build: configure
    cmake --build build -j {{nproc}}

# Build without LLVM pass plugin
build-no-llvm: deps
    cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$(pwd)/build-deps/install" \
        -DCOBRA_BUILD_LLVM_PASS=OFF
    cmake --build build -j {{nproc}}

# Run all tests
test:
    ctest --test-dir build --output-on-failure -j {{nproc}}

# Run a specific test by name pattern
test-filter pattern:
    ctest --test-dir build --output-on-failure -R "{{pattern}}"

# Install to a prefix (default: build/_install)
install prefix="build/_install": build
    cmake --install build --prefix "{{prefix}}"

# Clean build artifacts
clean:
    cmake --build build --target clean 2>/dev/null || true

# Remove all build directories
distclean:
    rm -rf build build-deps

# Run clang-format on all source files
format:
    fd -e cpp -e h --exclude build --exclude build-deps . | xargs {{clang_format}} -i

# Check formatting without modifying files
format-check:
    fd -e cpp -e h --exclude build --exclude build-deps . | xargs {{clang_format}} --dry-run --Werror

# Run codespell
spell:
    codespell

# Run all lint checks
lint: format-check spell

# Run prek pre-commit hooks
hooks:
    prek run

# Install prek git hooks
hooks-install:
    prek install
