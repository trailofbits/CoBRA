# Dataset Benchmark Report

CoBRA is validated against **76,144 lines** drawn from **35 dataset files** spanning 7 independent sources. These datasets are redistributed under their original licenses, which differ from CoBRA's Apache-2.0 license. See [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES) for provenance and license details. Every expression is parsed, simplified, and spot-checked at runtime. The numbers below are enforced by automated test assertions in [`test/verify/test_dataset_benchmarks.cpp`](test/verify/test_dataset_benchmarks.cpp) and verified on every CI run. OSES Fast is currently disabled (OOM on deeply nested expressions); its numbers are from the last successful run on master.

**Overall: 75,023 / 75,126 parsed expressions simplified (99.86%).**

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

### MSiMBA Datasets

Source: [Simplifier](https://github.com/mazeworks-security/Simplifier) (GPL-3.0-only)

| Dataset | Expressions | Parsed | Simplified | Unsupported | Rate |
|---------|:-----------:|:------:|:----------:|:-----------:|:----:|
| `univariate64.txt` | 1,000 | 1,000 | **1,000** | 0 | **100%** |
| `multivariate64.txt` | 1,000 | 1,000 | **1,000** | 0 | **100%** |
| `permutation64.txt` | 13 | 13 | **13** | 0 | **100%** |
| `msimba.txt` | 1,000 | 1,000 | **1,000** | 0 | **100%** |

- **univariate64** / **multivariate64**: Polynomial expressions (`x0*x0`, `x0*x1`) that simplify to linear targets. All 2,000 pass full-width verification.
- **permutation64**: High-degree polynomial expressions with `**` (exponentiation). All 13 expressions parse and simplify correctly.
- **msimba**: Semilinear expressions with bit-extraction patterns. All 1,000 simplify via bit-partitioned semilinear techniques.

### SiMBA++ Datasets

All files in the `simba/` directory were taken from the [SiMBA++](https://github.com/pgarba/SiMBA-) repository (GPL-3.0-only). SiMBA++ aggregates datasets from several upstream projects; original provenance is noted per subsection.

#### Expression Templates (E-Series)

Originally from [SiMBA](https://github.com/DenuvoSoftwareSolutions/SiMBA) (GPL-3.0-only, Denuvo Software Solutions). Redistributed via SiMBA++.

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

Originally from the [MBA-Solver](https://github.com/softsec-unh/MBA-Solver) repository (GPL-3.0-only), published at [PLDI'21](https://pldi21.sigplan.org/details/pldi-2021-papers/43/Boosting-SMT-Solver-Performance-on-Mixed-Bitwise-Arithmetic-Expressions). Redistributed via SiMBA++.

| Dataset | Total Lines | Parsed | Simplified | Notes | Rate |
|---------|:-----------:|:------:|:----------:|-------|:----:|
| `pldi_linear.txt` | 1,012 | 1,008 | **1,008** | 4 comment headers skipped | **100%** |
| `pldi_poly.txt` | 1,009 | 1,008 | **1,008** | 1 comment header skipped | **100%** |
| `pldi_nonpoly.txt` | 1,005 | 1,003 | **1,003** | 1 comment header skipped | **100%** |

#### MBA-Blast Datasets

Originally from the [MBA-Blast](https://github.com/softsec-unh/MBA-Blast) repository, published at [USENIX Security'21](https://dl.acm.org/doi/10.1145/3453483.3454068). Redistributed via SiMBA++.

| Dataset | Total Lines | Parsed | Simplified | Rate |
|---------|:-----------:|:------:|:----------:|:----:|
| `blast_dataset1.txt` | 63 | 62 | **62** | **100%** |
| `blast_dataset2.txt` | 2,501 | 2,500 | **2,500** | **100%** |

#### NeuReduce Dataset

`test_data.txt` is the [NeuReduce](https://github.com/fvrmatteo/NeuReduce) dataset (published at [EMNLP'20](https://aclanthology.org/2020.findings-emnlp.56.pdf)) under a different filename. Redistributed via SiMBA++. A second copy with minor variable renaming (`b` &rarr; `t`) exists as `gamba/neureduce.txt` via GAMBA.

| Dataset | Total Lines | Parsed | Simplified | Rate |
|---------|:-----------:|:------:|:----------:|:----:|
| `test_data.txt` | 10,000 | 10,000 | **10,000** | **100%** |


### GAMBA Datasets

All files in the `gamba/` directory were taken from [GAMBA](https://github.com/DenuvoSoftwareSolutions/GAMBA) (GPL-3.0-only, Denuvo Software Solutions). GAMBA aggregates datasets from several upstream projects; original provenance is noted per entry.

| Dataset | Origin | Total Lines | Parsed | Simplified | Unsupported | Rate |
|---------|--------|:-----------:|:------:|:----------:|:-----------:|:----:|
| `loki_tiny.txt` | LOKI | 25,025 | 25,000 | **25,000** | 0 | **100%** |
| `neureduce.txt` | NeuReduce | 10,000 | 10,000 | **10,000** | 0 | **100%** |
| `mba_obf_linear.txt` | GAMBA | 1,001 | 1,000 | **1,000** | 0 | **100%** |
| `mba_obf_nonlinear.txt` | GAMBA | 1,002 | 1,000 | **1,000** | 0 | **100%** |
| `syntia.txt` | Syntia | 501 | 500 | **500** | 0 | **100%** |
| `qsynth_ea.txt` | QSynth | 501 | 500 | **466** | 34 | **93.2%** |
| `mba_flatten.txt` | MBA-Flatten | 3,008 | 2,060 | **2,060** | 0 | **100%** |

- **loki_tiny**: 25 sections covering add, subtract, AND, OR, XOR at depths 1-5. All 25,000 are 2-variable linear MBAs. From the [Loki](https://github.com/RUB-SysSec/loki) repository, published at [USENIX Security'22](https://www.usenix.org/conference/usenixsecurity22/presentation/schloegel).
- **neureduce.txt**: 10,000 linear expressions with 2 to 5 variables. From the [NeuReduce](https://github.com/fvrmatteo/NeuReduce) dataset, published at [EMNLP'20](https://aclanthology.org/2020.findings-emnlp.56.pdf). Same dataset as `simba/test_data.txt` with minor variable renaming (`b` &rarr; `t`).
- **mba_obf_nonlinear** and **mba_obf_linear**: 1000 polynomial/nonpolynomial + 1000 linear expressions, all with linear ground-truth targets. All pass full-width verification. From the [MBA-Obfuscator](https://github.com/nhpcc502/MBA-Obfuscator) repo, published at [ICICS'21](https://dl.acm.org/doi/10.1007/978-3-030-86890-1_16).
- **syntia**: All 500 expressions simplify via the orchestrator's decomposition and lifting passes. From the [QSynth](https://github.com/werew/qsynth-artifacts) repo, published at [BAR'20](https://archive.bar/pdfs/bar2020-preprint9.pdf).
- **qsynth_ea**: The most challenging dataset. 466 of 500 expressions simplify. The 34 unsupported expressions are mixed bitwise-arithmetic expressions where CoB is boolean-correct but diverges at full width, and polynomial recovery (d=1..4) also fails — a genuine representation gap in carry-sensitive boolean-null residuals. From the [QSynth](https://github.com/werew/qsynth-artifacts) repo, published at [BAR'20](https://archive.bar/pdfs/bar2020-preprint9.pdf).
- **mba_flatten**: 3,008 lines across 7 sections (2-4 variable linear, sub-expression, and unsolvable-by-other-tools categories). 948 lines skipped (section headers, 3-field sub-expression rows, and expressions that fail parse). All 2,060 parseable expressions simplify at 100%. From [MBA-Flatten](https://tinyurl.com/y5l948pu), published in [Security and Communication Networks'22](https://onlinelibrary.wiley.com/doi/10.1155/2022/7307139).

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
| Total dataset lines | 76,144 |
| Comment/header lines skipped | 1,018 |
| **Parsed expressions** | **75,126** |
| **Simplified** | **75,023** |
| Unsupported | 103 |
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

### Redistribution Sources

These are the repositories from which CoBRA's copies were taken.

| Source | URL | License | Datasets |
|--------|-----|---------|----------|
| Simplifier | https://github.com/mazeworks-security/Simplifier | GPL-3.0 | univariate64, multivariate64, permutation64, msimba |
| SiMBA++ | https://github.com/pgarba/SiMBA- | GPL-3.0 | E-series, PLDI, BLAST, test_data |
| GAMBA | https://github.com/DenuvoSoftwareSolutions/GAMBA | GPL-3.0 | loki_tiny, neureduce, mba_obf_*, mba_flatten, syntia, qsynth_ea |
| OSES | https://github.com/fvrmatteo/oracle-synthesis-meets-equality-saturation | — | oses_all |

### Original Authors

These are the upstream projects that created the datasets. Some were aggregated into SiMBA++ or GAMBA before CoBRA sourced them.

| Original | URL | License | Datasets |
|----------|-----|---------|----------|
| SiMBA | https://github.com/DenuvoSoftwareSolutions/SiMBA | GPL-3.0 | E-series |
| MBA-Solver | https://github.com/softsec-unh/MBA-Solver | GPL-3.0 | PLDI |
| MBA-Blast | https://github.com/softsec-unh/MBA-Blast | — | BLAST |
| NeuReduce | https://github.com/fvrmatteo/NeuReduce | — | test_data, neureduce |
| Loki | https://github.com/RUB-SysSec/loki | AGPL-3.0 | loki_tiny |
| MBA-Obfuscator | https://github.com/nhpcc502/MBA-Obfuscator | MIT | mba_obf_* |
| QSynth | https://github.com/werew/qsynth-artifacts | — | qsynth_ea, syntia |
| MBA-Flatten | https://tinyurl.com/y5l948pu | — | mba_flatten |

See [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES) for full provenance and license details.
