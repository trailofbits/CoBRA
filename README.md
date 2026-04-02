# CoBRA

**Co**efficient-**B**ased **R**econstruction of **A**rithmetic — a Mixed Boolean-Arithmetic expression simplifier.

[![License: Apache-2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Tests](https://img.shields.io/badge/tests-1193-brightgreen.svg)](#testing)

CoBRA deobfuscates expressions that interleave arithmetic (`+`, `-`, `*`) with bitwise (`&`, `|`, `^`, `~`) and shift (`<<`, `>>`) operators — a technique commonly used in software obfuscation.

```
$ cobra-cli --mba "(x&y)+(x|y)"
x + y

$ cobra-cli --mba "((a^b)|(a^c)) + 65469 * ~((a&(b&c))) + 65470 * (a&(b&c))" --bitwidth 16
67 + (a | b | c)

$ cobra-cli --mba "((a^b)&c) | ((a&b)^c)"
c ^ a & b

$ cobra-cli --mba "(x&0xFF)+(x&0xFF00)" --bitwidth 16
x

$ cobra-cli --mba "(x ^ 0x10) + 2 * (x & 0x10)"
16 + x

$ cobra-cli --mba "x << 3"
8 * x
```

<details>
<summary>More examples</summary>

```
$ cobra-cli --mba "~x"
~x

$ cobra-cli --mba "(x^y)*(x&y) + 3*(x|y)"
(x ^ y) * (x & y) + 3 * (x | y)

$ cobra-cli --mba '-357*(x&~y)*(x&y)+102*(x&~y)*(x&~y)+374*(x&~y)*~(x^y)
  -306*(x&~y)*~(x|y)-17*(x&~y)*~(x|~y)-105*~(x|~y)*(x&y)+30*~(x|~y)*(x&~y)
  +110*~(x|~y)*~(x^y)-90*~(x|~y)*~(x|y)-5*~(x|~y)*~(x|~y)+34*(x&~y)*~x
  -85*(x&~y)*~y+10*~(x|~y)*~x-25*~(x|~y)*~y'
22 * (x & y) + -17 * x + -5 * y
```

</details>

## How It Works

CoBRA uses a worklist-based orchestrator to simplify expressions. Each input enters the worklist as a work item tagged with a state kind. A scheduler selects the next pass to run based on the item's state, prerequisite dependencies, and an attempt cache that prevents redundant work.

36 discrete passes are organized into families: AST processing, signature-based techniques, semilinear techniques, decomposition, and lifting. Some passes fork local alternatives or child solves that are resolved by competition groups; outside those groups, the worklist returns the first fully verified top-level candidate. All results are verified by spot-checking random inputs (default) or Z3 equivalence proof (`--verify`).

```
Input Expression
       |
  [Worklist Scheduler]
       |
  Work items flow through state kinds:
       |
  kFoldedAst ──> AST processing passes
       |         (classify, lower, rewrite)
       |
       +──> kSignatureState ──> Signature techniques
       |    (pattern match, CoB, ANF, polynomial recovery)
       |
       +──> kSemilinearNormalizedIr ──> Semilinear techniques
       |    (normalize, recover structure, refine, reconstruct)
       |
       +──> kCoreCandidate / kRemainderState ──> Decomposition
       |    (extract core, classify residual, solve)
       |
       +──> kLiftedSkeleton ──> Lifting
       |    (virtual variable substitution, outer solve)
       |
       +──> kCandidateExpr ──> Verification
            (spot-check or Z3 proof)
       |
  Simplified Expression
```

**Signature-based techniques** evaluate the expression on all Boolean inputs to get a signature vector. A CoB butterfly transform recovers AND-product basis coefficients. Pattern matching, ANF, and polynomial recovery handle different complexity levels.

**Semilinear techniques** handle expressions with constant masks (e.g., `x & 0xFF`). The expression is decomposed into weighted bitwise atoms, then structure recovery and term refinement simplify the intermediate representation, and bit-partitioned OR-assembly reconstructs the final result.

**Decomposition** targets mixed expressions with products of bitwise subexpressions. A polynomial core is extracted, then residuals are classified and solved (polynomial, boolean-null/ghost, or template fallback).

**Lifting** replaces complex subexpressions with virtual variables, solves the simplified outer skeleton, then substitutes back.

## Features

- **Linear MBA simplification** — weighted sums of bitwise atoms via signature vector and CoB transform
- **Scaled pattern matching** — `k * f(vars) + c` with Shannon decomposition for 4-5 variable Boolean expressions
- **Semilinear support** — constant-masked atoms with XOR/OR/NOT-AND constant lowering, structure recovery, term refinement, bit-partitioned reconstruction
- **Polynomial recovery** — multilinear terms and singleton powers via coefficient splitting and finite differences
- **Mixed product handling** — decomposition engine with core extraction, residual solving, and ghost residual classification
- **Subexpression lifting** — replace complex subtrees with virtual variables to reduce problem dimension
- **Worklist orchestrator** — DAG-aware pass scheduling with deduplication and bounded search
- **Competition groups** — local alternative branches and child solves use cost-based winner selection with continuations
- **Constant shifts** — `<<` desugars to multiplication, `>>` simplifies via semilinear techniques
- **ANF cleanup** — absorption, common-cube factoring, and OR recognition
- **Configurable bitwidth** — 1-bit to 64-bit modular arithmetic
- **Auxiliary variable elimination** — reduces variable count when terms cancel
- **Z3 verification** — optional equivalence checking of simplified output
- **Spot-check self-test** — lightweight random-input validation when Z3 is unavailable
- **LLVM pass plugin** — integrate directly into compiler pipelines (requires LLVM 19-22)

## Building

See [BUILD.md](BUILD.md) for full details including optional dependencies (LLVM, Z3).

```bash
# Build dependencies (Abseil, Highway; optionally GoogleTest, LLVM, Z3)
cmake -S dependencies -B build-deps -DCMAKE_BUILD_TYPE=Release
cmake --build build-deps

# Build CoBRA
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH=$(pwd)/build-deps/install \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build

# (Optional) Build and run tests
cmake -S dependencies -B build-deps -DCMAKE_BUILD_TYPE=Release -DCOBRA_BUILD_TESTS=ON
cmake --build build-deps
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH=$(pwd)/build-deps/install \
  -DCMAKE_BUILD_TYPE=Release \
  -DCOBRA_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

### With LLVM Pass Plugin

```bash
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH=$(pwd)/build-deps/install \
  -DCOBRA_BUILD_LLVM_PASS=ON \
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
| `--max-vars <n>` | 16 | Maximum variable count |
| `--verify` | off | Z3 equivalence check |
| `--verbose` | off | Print pipeline internals |

## Project Structure

```
lib/core/                Core simplification engine (~50 source files)
  Orchestrator             Worklist scheduler, state machine, main simplification loop
  OrchestratorPasses       39-pass registry with DAG-aware scheduling
  CompetitionGroup         Multi-technique racing and winner selection
  ContinuationTypes        Deferred recombination data for pass composition
  JoinState                Multi-operand join tracking for structural rewrites
  SignatureSimplifier      Signature-based techniques (CoB, pattern matching, ANF)
  SignatureVector          Evaluate expression on {0,1}^n inputs
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
  DecompositionEngine      Extract-solve loop: polynomial core + residual solving
  GhostBasis               Ghost primitive library (mul_sub_and, mul3_sub_and3)
  GhostResidualSolver      Boolean-null classification and ghost residual solving
  WeightedPolyFit          2-adic weighted linear solve for polynomial quotients
  MixedProductRewriter     Expand bitwise products into linear sums
  TemplateDecomposer       Bounded template matching for mixed expressions
  ProductIdentityRecoverer Recover product-of-sums identities
  SemilinearNormalizer     Decompose into weighted bitwise atoms
  SemilinearSignature      Per-bit signature evaluation and linear shortcut
  StructureRecovery        XOR recovery, mask elimination, term coalescing
  TermRefiner              Dead-bit mask reduction, same-coefficient merge
  BitPartitioner           Group bit positions by semantic profile
  MaskedAtomReconstructor  Reassemble with OR-rewrite for disjoint masks
  Evaluator                Compiled expression evaluator

lib/llvm/                LLVM pass plugin (CobraPass, MBADetector, IRReconstructor)
lib/verify/              Z3-based equivalence verification
include/cobra/           Public headers
tools/cobra-cli/         CLI frontend and expression parser
test/                    1193 tests across ~63 test files
```

## Testing

CoBRA has 1193 tests covering unit, integration, and dataset benchmarks:

```bash
# Run all tests
ctest --test-dir build --output-on-failure

# Run a specific test suite
ctest --test-dir build -R test_simplifier --output-on-failure

# Run with verbose output
ctest --test-dir build -V
```

Dataset benchmarks validate against real-world obfuscated expressions from multiple independent sources. See [DATASETS.md](DATASETS.md) for the full benchmark report — 70,059 expressions across 33 dataset files from 7 independent sources.

## Known Limitations

- **Deeply interleaved mixed-polynomial MBAs** — the remaining unsupported expressions are predominantly large, heavily duplicated ASTs with interleaved arithmetic and bitwise operators. Impact-ranked subexpression lifting recovers many of these, but expressions that exhaust the worklist budget after lifting remain unsupported
- **Boolean-domain reconstruction divergence** — a small number of expressions produce CoB candidates that are correct on `{0,1}` inputs but incorrect at full width (AND-product basis vs. arithmetic multiplication). These are detected and correctly reported as verify-failed
- **No general logic minimization** — CoBRA uses greedy algebraic rewrites, not Quine-McCluskey/Espresso/BDD

## Acknowledgments

Thanks to [Bas Zweers](https://github.com/AnalogCyberNuke) and the [Back Engineering](https://github.com/backengineering) team for the inspiration and guidance that helped shape this project. Recommended viewing: their re//verse 2026 talk [Deobfuscation of a Real World Binary Obfuscator](https://www.youtube.com/watch?v=3LtwqJM3Qjg).

Additional thanks to [Matteo Favaro](https://github.com/fvrmatteo) and other anonymous contributors for the continual review and testing

## License

[Apache-2.0](LICENSE)
