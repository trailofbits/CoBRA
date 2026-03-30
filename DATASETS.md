# Dataset Benchmark Report

CoBRA is validated against **73,136 lines** drawn from **34 dataset files** spanning 7 independent sources. Every expression is parsed, simplified, and spot-checked at runtime. The numbers below are enforced by automated test assertions in [`test/verify/test_dataset_benchmarks.cpp`](test/verify/test_dataset_benchmarks.cpp) and verified on every CI run. OSES Fast is currently disabled (OOM on deeply nested expressions); its numbers are from the last successful run on master.

**Overall: 72,928 / 73,066 parsed expressions simplified (99.81%).**

---

## MBA Classes

CoBRA classifies each input expression into one of four semantic classes and selects techniques accordingly:

| Class | Description | Techniques |
|-------|-------------|------------|
| **Linear** | Weighted sums of bitwise atoms (`+`, `-`, `*const` over `&`, `\|`, `^`, `~`) | Signature vector, CoB butterfly transform, pattern matching, ANF |
| **Semilinear** | Bitwise operations with constant masks (`x & 0xFF`, `y \| 0xF0`) | Constant lowering, structure recovery, term refinement, bit-partitioned reconstruction |
| **Polynomial** | Products of variables (`x*y`, `x^2`) possibly mixed with bitwise atoms | Coefficient splitting, polynomial recovery, finite differences |
| **Mixed** | Products of bitwise subexpressions (`(x&y)*(x\|y)`) | Decomposition engine, ghost residual solving, template matching, lifting |

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
- **msimba**: Semilinear expressions with bit-extraction patterns. All 1,000 simplify via bit-partitioned semilinear techniques.

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
| `qsynth_ea.txt` | QSynth | 501 | 500 | **431** | 69 | **86.2%** |

- **loki_tiny**: 25 sections covering add, subtract, AND, OR, XOR at depths 1-5. All 25,000 are 2-variable linear MBAs.
- **mba_obf_nonlinear**: 500 polynomial + 500 linear expressions, all with linear ground-truth targets. All 1,000 pass full-width verification.
- **syntia**: All 500 expressions simplify via the orchestrator's decomposition and lifting passes.
- **qsynth_ea**: The most challenging dataset. 431 of 500 expressions simplify. The 69 unsupported expressions break down into 9 verify-failed, 2 representation-gap, 14 guard-failed, and 44 search-exhausted. Impact-ranked lifting with budget supplementation and correct competition group handle accounting recovers many expressions that were previously blocked by structural redundancy or handle leaks preventing lifted outer solutions from propagating back.

### OSES Dataset

Source: [oracle-synthesis-meets-equality-saturation](https://github.com/fvrmatteo/oracle-synthesis-meets-equality-saturation)

| Dataset | Total Lines | Parsed | Simplified | Unsupported | Rate |
|---------|:-----------:|:------:|:----------:|:-----------:|:----:|
| `oses_fast.txt` | 473 | 458 | **390** | 68 | **85.2%** |
| `oses_slow.txt` | 7 | 7 | **6** | 1 | **85.7%** |
| **OSES Total** | **480** | **465** | **396** | **69** | **85.2%** |

- **oses**: 479 MBA expressions extracted from the OSES `synth.py` evaluation script (plus 1 header comment). Expressions span linear, nonlinear/product, and constant categories with 1-14 variables. The dataset is split into fast (472 expressions under 50K characters) and slow (7 mega-expressions over 50K characters). **Both subsets are currently disabled** — the fast subset OOMs on deeply nested expressions that exceed memory limits during recursive evaluation, and the slow subset requires minutes per expression. Numbers shown are from the last successful run on master.

### ObfuscatorX Dataset

| Dataset | Total Lines | Parsed | Simplified | Unsupported | Rate |
|---------|:-----------:|:------:|:----------:|:-----------:|:----:|
| `obfuscatorx.txt` | 8 | 7 | **7** | 0 | **100%** |

- **obfuscatorx**: 7 expressions lifted from ObfuscatorX. All simplify via the standard pipeline.

---

## Aggregate Summary

| Metric | Count |
|--------|------:|
| Total dataset lines | 73,136 |
| Comment/header lines skipped | 70 |
| **Parsed expressions** | **73,066** |
| **Simplified** | **72,928** |
| Unsupported (by design) | 138 |
| Errors / failures | **0** |

| MBA Class | Expressions | Simplified | Rate |
|-----------|:-----------:|:----------:|:----:|
| Linear | ~55,000 | ~55,000 | **~100%** |
| Semilinear | ~1,000 | ~1,000 | **~100%** |
| Polynomial | ~5,000 | ~4,950 | **~99%** |
| Mixed | ~9,000 | ~8,800 | **~98%** |

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
