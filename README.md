# CoBRA

**Co**efficient-**B**ased **R**econstruction of **A**rithmetic — a Mixed Boolean-Arithmetic expression simplifier.

[![License: Apache-2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Tests](https://img.shields.io/badge/tests-858-brightgreen.svg)](#testing)

CoBRA deobfuscates expressions that interleave arithmetic (`+`, `-`, `*`) with bitwise (`&`, `|`, `^`, `~`) and shift (`<<`, `>>`) operators — a technique commonly used in software obfuscation.

```
$ cobra-cli --mba "(x&y)+(x|y)"
x + y

$ cobra-cli --mba "((a^b)|(a^c)) + 65469 * ~((a&(b&c))) + 65470 * (a&(b&c))" --bitwidth 16
67 + (a | b | c)

$ cobra-cli --mba "((a^b)&c) | ((a&b)^c)"
c ^ a & b

$ cobra-cli --mba "(x&0xFF)+(x&0xFF00)" --bitwidth 16
x & 255 | x & -256

$ cobra-cli --mba "x << 3"
8 * x
```

<details>
<summary>More examples</summary>

```
$ cobra-cli --mba "~x"
~x

$ cobra-cli --mba "(x^y)*(x&y) + 3*(x|y)"
(x & y) * (x ^ y) + ~2 * -(x | y)

$ cobra-cli --mba '-357*(x&~y)*(x&y)+102*(x&~y)*(x&~y)+374*(x&~y)*~(x^y)
  -306*(x&~y)*~(x|y)-17*(x&~y)*~(x|~y)-105*~(x|~y)*(x&y)+30*~(x|~y)*(x&~y)
  +110*~(x|~y)*~(x^y)-90*~(x|~y)*~(x|y)-5*~(x|~y)*~(x|~y)+34*(x&~y)*~x
  -85*(x&~y)*~y+10*~(x|~y)*~x-25*~(x|~y)*~y'
22 * (x & y) + -17 * x + -5 * y
```

</details>

## How It Works

CoBRA classifies each expression and routes it through the appropriate pipeline:

| Pipeline | Input | Technique |
|----------|-------|-----------|
| **Linear** | Weighted sums of bitwise atoms | Signature vector → CoB butterfly transform → pattern matching / ANF cleanup |
| **Semilinear** | Constant-masked atoms (`x & 0xFF`) | Bit-partitioned decomposition per mask partition |
| **Polynomial** | Variable products (`x*y`, `x^2`) | Coefficient splitting + singleton power recovery |
| **Mixed** | Products of bitwise subexpressions | Hybrid/template decomposition, product identity recovery, algebraic rewriting |

The core insight is the **Change of Basis (CoB) transform**: evaluate the expression on all 2^n Boolean inputs to produce a signature vector, then apply a butterfly recurrence to recover the coefficients of each AND-product basis term. Pattern matching recognizes all 2- and 3-variable Boolean functions, including scaled forms like `67*(a|b|c)`. For 4- and 5-variable Boolean expressions, **Shannon decomposition** recursively splits on a variable to reduce to smaller cases covered by the lookup tables. Complex Boolean results fall back to ANF (Algebraic Normal Form) with cleanup passes for absorption, common-cube factoring, and OR recognition.

## Features

- **Linear MBA simplification** — arbitrary combinations of `&`, `|`, `^`, `~` with `+`, `-`, constant multipliers
- **Scaled pattern matching** — expressions like `k * f(vars) + c` where `f` is a bitwise function; Shannon decomposition extends coverage to 4- and 5-variable Boolean expressions
- **Semilinear support** — constant-masked atoms like `x & 0xFF`, `y | 0xF0` via bit-partitioned reconstruction
- **Polynomial recovery** — multilinear terms (`x*y`) and singleton powers (`x^2`) via coefficient splitting
- **Mixed product handling** — hybrid/template decomposition and product identity recovery for bitwise-product expressions
- **Constant shifts** — `<<` desugars to multiplication, `>>` on bitwise subtrees simplifies via the semilinear pipeline
- **ANF cleanup** — absorption, common-cube factoring, and OR recognition for compact Boolean output
- **Configurable bitwidth** — 1-bit to 64-bit modular arithmetic
- **Auxiliary variable elimination** — reduces variable count when terms cancel
- **Z3 verification** — optional equivalence checking of simplified output
- **Spot-check self-test** — lightweight random-input validation when Z3 is unavailable
- **LLVM pass plugin** — integrate directly into compiler pipelines (requires LLVM 19-22)

## Building

See [BUILD.md](BUILD.md) for full details including optional dependencies (LLVM, Z3).

```bash
# Build dependencies (GoogleTest, optionally LLVM/Z3)
cmake -S dependencies -B build-deps -DCMAKE_BUILD_TYPE=Release
cmake --build build-deps

# Build CoBRA
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH=$(pwd)/build-deps/install \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run tests
ctest --test-dir build --output-on-failure
```

### CLI-Only (No LLVM)

```bash
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH=$(pwd)/build-deps/install \
  -DCOBRA_BUILD_LLVM_PASS=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Usage

```bash
# Basic simplification
cobra-cli --mba "(x&y)+(x|y)"

# Specify bitwidth
cobra-cli --mba "(x&0xFF)+(x&0xFF00)" --bitwidth 16

# Enable Z3 equivalence verification
cobra-cli --mba "(a^b)+(a&b)+(a&b)" --verify

# Verbose output (show intermediate pipeline steps)
cobra-cli --mba "(x&y)+(x|y)" --verbose
```

### Options

| Flag | Default | Description |
|------|---------|-------------|
| `--mba <expr>` | | Expression to simplify |
| `--bitwidth <n>` | 64 | Modular arithmetic width (1-64) |
| `--max-vars <n>` | 12 | Maximum variable count |
| `--verify` | off | Z3 equivalence check |
| `--strict` | off | Require Z3 for semilinear results |
| `--verbose` | off | Print pipeline internals |

## Project Structure

```
lib/core/                Core simplification pipeline (36 source files)
  Simplifier               Top-level orchestration and route dispatch
  Classifier               Route: Linear / Semilinear / Polynomial / Mixed
  SignatureVector           Evaluate expression on {0,1}^n inputs
  AuxVarEliminator         Reduce variable count by detecting cancellations
  PatternMatcher           Recognize bitwise patterns (2-var/3-var tables, scaled)
  CoeffInterpolator        Butterfly interpolation for coefficient recovery
  CoBExprBuilder           Reconstruct expressions from CoB coefficients
  AnfTransform             Algebraic Normal Form conversion
  AnfCleanup               Absorption, factoring, OR recognition
  CoefficientSplitter      Separate bitwise vs. arithmetic contributions
  ArithmeticLowering       Lower arithmetic fragment to polynomial IR
  PolyNormalizer           Canonical form for polynomial expressions
  SingletonPowerRecovery   Detect x^k terms via finite differences
  MixedProductRewriter     Expand bitwise products into linear sums
  BitwiseDecomposer        Bitwise-only decomposition for non-polynomial products
  HybridDecomposer         Combined bitwise-polynomial decomposition
  TemplateDecomposer       Bounded template matching for mixed expressions
  ProductIdentityRecoverer Recover product-of-sums identities
  SemilinearNormalizer     Decompose into weighted bitwise atoms
  BitPartitioner           Split constant-masked atoms by bit position

lib/llvm/                LLVM pass plugin (CobraPass, MBADetector, IRReconstructor)
lib/verify/              Z3-based equivalence verification
include/cobra/           Public headers
tools/cobra-cli/         CLI frontend and expression parser
test/                    858 tests across 47 test files (unit + integration + dataset benchmarks)
```

## Testing

CoBRA has 858 tests covering unit, integration, and dataset benchmarks:

```bash
# Run all tests
ctest --test-dir build --output-on-failure

# Run a specific test suite
ctest --test-dir build -R test_simplifier --output-on-failure

# Run with verbose output
ctest --test-dir build -V
```

Dataset benchmarks validate against real-world obfuscated expressions from QSynth and PLDI datasets. See [DATASETS.md](DATASETS.md) for the full benchmark report — 69,477 expressions simplified across 31 dataset files from 6 independent sources, with zero failures.

## Known Limitations

- **Some mixed products unsupported** — complex combinations of bitwise-product subexpressions (e.g., `2*(x&y)*(x|y) + x*y`) may not simplify when no rewrite rule applies
- **No general logic minimization** — CoBRA uses greedy algebraic rewrites, not Quine-McCluskey/Espresso/BDD

## License

[Apache-2.0](LICENSE)
