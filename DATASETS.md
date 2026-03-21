# Dataset Benchmark Report

CoBRA is validated against **73,119 expressions** drawn from **32 dataset files** spanning 7 independent sources. Every expression is parsed, simplified, and spot-checked at runtime. The numbers below are enforced by automated test assertions in [`test/verify/test_dataset_benchmarks.cpp`](test/verify/test_dataset_benchmarks.cpp) and verified on every CI run.

**Overall: 69,942 / 70,060 parsed expressions simplified (99.83%), zero failures.**

---

## MBA Classes

CoBRA classifies each input expression into one of four semantic classes, then routes it through the appropriate pipeline:

| Class | Route | Description | Technique |
|-------|-------|-------------|-----------|
| **Linear** | BitwiseOnly | Weighted sums of bitwise atoms (`+`, `-`, `*const` over `&`, `\|`, `^`, `~`) | Signature vector, CoB butterfly transform, pattern matching, ANF cleanup |
| **Semilinear** | BitwiseOnly | Bitwise operations with constant masks (`x & 0xFF`, `y \| 0xF0`) | Bit-partitioned decomposition per mask partition |
| **Polynomial** | Multilinear / PowerRecovery | Products of variables (`x*y`, `x^2`) possibly mixed with bitwise atoms | Coefficient splitting, singleton power recovery, multilinear interpolation |
| **Mixed** | MixedRewrite | Products of bitwise subexpressions (`(x&y)*(x\|y)`) | Hybrid/template decomposition, product identity recovery, algebraic rewriting |

---

## Results by Dataset

### Proprietary Datasets

| Dataset | Expressions | Parsed | Simplified | Unsupported | Rate |
|---------|:-----------:|:------:|:----------:|:-----------:|:----:|
| `univariate64.txt` | 1,000 | 1,000 | **1,000** | 0 | **100%** |
| `multivariate64.txt` | 1,000 | 1,000 | **1,000** | 0 | **100%** |
| `permutation64.txt` | 13 | 13 | **13** | 0 | **100%** |
| `msimba.txt` | 1,000 | 1,000 | **1,000** | 0 | **100%** |

- **univariate64** / **multivariate64**: Polynomial expressions (`x0*x0`, `x0*x1`) that simplify to linear targets. All 2,000 pass full-width verification.
- **permutation64**: High-degree polynomial expressions with `**` (exponentiation). All 13 expressions parse and simplify correctly.
- **msimba**: Semilinear expressions with bit-extraction patterns. All 1,000 simplified via the bit-partitioned pipeline.

### SiMBA Datasets

Source: [SiMBA](https://github.com/pgarba/SiMBA-)

#### Expression Templates (E-Series)

Each file contains 1,000 obfuscated linear MBA expressions plus a header comment. All 16,000 expressions across 16 files simplify at 100%.

| Dataset | Variables | Expressions | Simplified | Rate |
|---------|:---------:|:-----------:|:----------:|:----:|
| `e1_2vars.txt` | 2 | 1,000 | **1,000** | **100%** |
| `e1_3vars.txt` | 3 | 1,000 | **1,000** | **100%** |
| `e1_4vars.txt` | 4 | 1,000 | **1,000** | **100%** |
| `e1_5vars.txt` | 5 | 1,000 | **1,000** | **100%** |
| `e2_2vars.txt` | 2 | 1,000 | **1,000** | **100%** |
| `e2_3vars.txt` | 3 | 1,000 | **1,000** | **100%** |
| `e2_4vars.txt` | 4 | 1,000 | **1,000** | **100%** |
| `e3_2vars.txt` | 2 | 1,000 | **1,000** | **100%** |
| `e3_3vars.txt` | 3 | 1,000 | **1,000** | **100%** |
| `e3_4vars.txt` | 4 | 1,000 | **1,000** | **100%** |
| `e4_2vars.txt` | 2 | 1,000 | **1,000** | **100%** |
| `e4_3vars.txt` | 3 | 1,000 | **1,000** | **100%** |
| `e4_4vars.txt` | 4 | 1,000 | **1,000** | **100%** |
| `e5_2vars.txt` | 2 | 1,000 | **1,000** | **100%** |
| `e5_3vars.txt` | 3 | 1,000 | **1,000** | **100%** |
| `e5_4vars.txt` | 4 | 1,000 | **1,000** | **100%** |
| **E-Series Total** | | **16,000** | **16,000** | **100%** |

#### PLDI Research Datasets

| Dataset | Total Lines | Parsed | Simplified | Notes | Rate |
|---------|:-----------:|:------:|:----------:|-------|:----:|
| `pldi_linear.txt` | 1,012 | 1,008 | **1,008** | 4 comment headers skipped | **100%** |
| `pldi_poly.txt` | 1,009 | 1,008 | **1,008** | 1 comment header skipped | **100%** |
| `pldi_nonpoly.txt` | 1,004 | 1,003 | **1,003** | 1 comment header skipped | **100%** |

- **pldi_nonpoly**: Of the 1,003 parseable expressions, 844 are linear, 55 are polynomial, 92 are mixed expressions (marked unsolvable by the original PLDI/SiMBA tooling) that CoBRA handles via hybrid/template decomposition, and 12 contain wrapped product identities that CoBRA resolves via full-variable verification and template decomposition fallback.

#### Other SiMBA Datasets

| Dataset | Expressions | Parsed | Simplified | Rate |
|---------|:-----------:|:------:|:----------:|:----:|
| `test_data.txt` | 10,000 | 10,000 | **10,000** | **100%** |
| `blast_dataset1.txt` | 63 | 62 | **62** | **100%** |
| `blast_dataset2.txt` | 2,501 | 2,500 | **2,500** | **100%** |

### GAMBA Datasets

Source: [GAMBA](https://github.com/DenuvoSoftwareSolutions/GAMBA)

| Dataset | Origin | Total Lines | Parsed | Simplified | Unsupported | Rate |
|---------|--------|:-----------:|:------:|:----------:|:-----------:|:----:|
| `loki_tiny.txt` | LOKI | 25,025 | 25,000 | **25,000** | 0 | **100%** |
| `neureduce.txt` | NeuReduce | 10,000 | 10,000 | **10,000** | 0 | **100%** |
| `mba_obf_linear.txt` | GAMBA | 1,001 | 1,000 | **1,000** | 0 | **100%** |
| `mba_obf_nonlinear.txt` | GAMBA | 1,002 | 1,000 | **1,000** | 0 | **100%** |
| `syntia.txt` | Syntia | 501 | 500 | **500** | 0 | **100%** |
| `qsynth_ea.txt` | QSynth | 501 | 500 | **407** | 93 | **81.4%** |

- **loki_tiny**: 25 sections covering add, subtract, AND, OR, XOR at depths 1-5. All 25,000 are 2-variable linear MBAs.
- **mba_obf_nonlinear**: 500 polynomial + 500 linear expressions, all with linear ground-truth targets. All 1,000 pass full-width verification.
- **qsynth_ea**: The most challenging dataset. 407 of 500 expressions simplify via the multi-step MixedRewrite pipeline and decomposition engine. The 93 unsupported expressions are dominated by product-inside-bitwise patterns (products nested inside `&`, `|`, `^`) and boolean-null residuals that fall outside current representation families.

### OSES Dataset

Source: [oracle-synthesis-meets-equality-saturation](https://github.com/fvrmatteo/oracle-synthesis-meets-equality-saturation)

| Dataset | Total Lines | Parsed | Simplified | Unsupported | Rate |
|---------|:-----------:|:------:|:----------:|:-----------:|:----:|
| `oses_all.txt` | 473 | 466 | **441** | 25 | **94.6%** |

- **oses_all**: 472 MBA expressions extracted from the OSES `synth.py` evaluation script (plus 1 header comment). Expressions span linear (205), nonlinear/product (131), linear-with-constants (129), and constant (7) categories with 1-14 variables. The 25 unsupported expressions are dominated by product-inside-bitwise patterns and complex multi-variable mixed products that fall outside current representation families.

---

## Aggregate Summary

| Metric | Count |
|--------|------:|
| Total dataset lines | 73,119 |
| Comment/header lines skipped | 2,069 |
| Non-expression lines (headers, no ground truth) | 990 |
| **Parsed expressions** | **70,060** |
| **Simplified** | **69,942** |
| Unsupported (by design) | 118 |
| Errors / failures | **0** |

| MBA Class | Expressions | Simplified | Rate |
|-----------|:-----------:|:----------:|:----:|
| Linear | ~55,000 | ~55,000 | **100%** |
| Semilinear | 1,000 | 1,000 | **100%** |
| Polynomial | ~5,000 | ~5,000 | **100%** |
| Mixed / Hybrid | ~8,584 | ~8,467 | **~99%** |

All simplified results are validated via spot-check (random-input evaluation) at 64-bit width. When Z3 is available, full equivalence proofs are performed.

---

## Dataset Sources

| Source | URL | Datasets |
|--------|-----|----------|
| SiMBA | https://github.com/pgarba/SiMBA- | E-series, PLDI, BLAST, test_data |
| GAMBA | https://github.com/DenuvoSoftwareSolutions/GAMBA | loki_tiny, neureduce, mba_obf_*, syntia, qsynth_ea |
| QSynth | (via GAMBA) | qsynth_ea |
| NeuReduce | (via GAMBA) | neureduce |
| Syntia | (via GAMBA) | syntia |
| LOKI | (via GAMBA) | loki_tiny |
| OSES | https://github.com/fvrmatteo/oracle-synthesis-meets-equality-saturation | oses_all |
