# CoBRA Methodology

This document describes the architecture and techniques behind CoBRA's Mixed Boolean-Arithmetic (MBA) expression simplifier. For the mathematical details behind each technique, see the referenced papers.

## Background

MBA expressions interleave arithmetic operators (`+`, `-`, `*`) with bitwise operators (`&`, `|`, `^`, `~`) and shifts (`<<`, `>>`). This combination defeats both algebraic simplification (which expects pure arithmetic) and Boolean minimization (which expects pure logic). MBA obfuscation exploits this gap — a simple expression like `x + y` can be rewritten as `(x & y) + (x | y)`, and layered rewrites make it arbitrarily complex.

CoBRA reverses this process. Given an obfuscated MBA expression, it recovers a simplified equivalent using a pipeline of algebraic techniques rooted in the observation that every MBA expression over *n* variables is fully determined by its values on the 2^n Boolean inputs {0, 1}^n.

**Foundational reference:** Zhou et al., [Information Hiding in Software with Mixed Boolean-Arithmetic Transforms](https://www.researchgate.net/publication/221239701_Information_Hiding_in_Software_with_Mixed_Boolean-Arithmetic_Transforms) (WISA 2007) — the original formalization of MBA obfuscation.

## Architecture Overview

CoBRA classifies each input expression and routes it through one of four specialized pipelines:

```
Input Expression
       |
  [Classifier] ── structural analysis, constant folding
       |
  [AuxVarEliminator] ── detect variable cancellations, reduce variable count
       |
  [Route Dispatch]
       |
       +-- Linear ──────────> BitwiseOnly pipeline
       |                      (signature vector, CoB transform, pattern matching, ANF)
       |
       +-- Semilinear ──────> BitwiseOnly pipeline (with bit-partitioning)
       |                      (constant-masked atoms, per-partition reconstruction)
       |
       +-- Polynomial ──────> Multilinear / PowerRecovery pipeline
       |                      (coefficient splitting, finite differences, interpolation)
       |
       +-- Mixed ───────────> MixedRewrite pipeline
       |                      (multi-step rewriting, decomposition engine, ghost residual solving)
       |
  [Verification] ── spot-check (random inputs) or Z3 equivalence proof
       |
  Simplified Expression
```

## Classification

The **Classifier** performs structural analysis of the expression AST to determine which pipeline can handle it. It sets structural flags based on the operators and operand patterns found:

| Flag | Meaning | Example |
|------|---------|---------|
| `HasMultilinearProduct` | Variable-variable multiplication | `x * y` |
| `HasSingletonPower` | Single variable raised to a power | `x * x` (i.e., x^2) |
| `HasMixedProduct` | Product of bitwise subexpressions | `(x & y) * (x \| y)` |
| `HasBitwiseOverArith` | Bitwise op wrapping arithmetic | `(x + y) & z` |

These flags determine the **route**:

| Route | Condition | Pipeline |
|-------|-----------|----------|
| BitwiseOnly | No products, no mixed ops | [Linear](linear-pipeline.md) / [Semilinear](semilinear-pipeline.md) |
| Multilinear | Has `x * y` products | [Polynomial](polynomial-pipeline.md) |
| PowerRecovery | Has `x^k` terms | [Polynomial](polynomial-pipeline.md) |
| MixedRewrite | Has mixed/bitwise-over-arithmetic products | [Mixed](mixed-pipeline.md) (multi-step + decomposition engine) |

## Auxiliary Variable Elimination

Before entering a pipeline, CoBRA checks whether any variables cancel out of the expression. If the signature vector is invariant under toggling a particular variable's input bit, that variable contributes nothing and is eliminated. This reduces the problem dimension and can shift an expression to a simpler pipeline.

## Verification

After simplification, CoBRA validates correctness:

- **Spot-check** (default): Evaluates original and simplified expressions on random inputs and confirms they match at full bitwidth.
- **Z3 equivalence proof** (optional, `--verify`): Constructs a formal equivalence query and proves the result is correct for all inputs.

## Pipeline Documentation

Each pipeline is documented in detail:

1. **[Linear Pipeline](linear-pipeline.md)** — Weighted sums of bitwise atoms
2. **[Semilinear Pipeline](semilinear-pipeline.md)** — Bitwise atoms with constant masks
3. **[Polynomial Pipeline](polynomial-pipeline.md)** — Variable products and powers
4. **[Mixed Pipeline](mixed-pipeline.md)** — Products of bitwise subexpressions

## Technique Index

See the **[Technique Index](techniques.md)** for an alphabetical reference of all techniques with cross-links to the pipelines where they are used.

## References

The full list of academic references that influenced CoBRA's design:

### MBA Foundations

| Paper | Authors | Venue | Link |
|-------|---------|-------|------|
| Information Hiding in Software with Mixed Boolean-Arithmetic Transforms | Zhou, Main, Gu, Johnson | WISA 2007 | [ResearchGate](https://www.researchgate.net/publication/221239701_Information_Hiding_in_Software_with_Mixed_Boolean-Arithmetic_Transforms) |
| MBA-Blast: Unveiling and Simplifying Mixed Boolean-Arithmetic Obfuscation | Liu, Shen, Ming, Zheng, Li, Xu | USENIX Security 2021 | [Paper](https://www.usenix.org/conference/usenixsecurity21/presentation/liu-binbin) |

### Linear & Semilinear MBA Simplification

| Paper | Authors | Venue | Link |
|-------|---------|-------|------|
| Efficient Deobfuscation of Linear Mixed Boolean-Arithmetic Expressions (SiMBA) | Reichenwallner, Meerwald-Stadler | 2022 | [arXiv:2209.06335](https://arxiv.org/abs/2209.06335) |
| Deobfuscation of Semi-Linear Mixed Boolean-Arithmetic Expressions (MSiMBA) | Skees | 2024 | [arXiv:2406.10016](https://arxiv.org/abs/2406.10016) |

### General & Polynomial MBA

| Paper | Authors | Venue | Link |
|-------|---------|-------|------|
| Simplification of General Mixed Boolean-Arithmetic Expressions (GAMBA) | Reichenwallner, Meerwald-Stadler | IEEE EuroS&PW 2023 | [arXiv:2305.06763](https://arxiv.org/abs/2305.06763) |
| Efficient Normalized Reduction and Generation of Equivalent Multivariate Binary Polynomials | Gamez-Montolio, Florit, Brain, Howe | BAR / NDSS 2024 | [NDSS](https://www.ndss-symposium.org/ndss-paper/auto-draft-436/) · [Open Access PDF](https://openaccess.city.ac.uk/id/eprint/32695/) |
| Simplifying Mixed Boolean-Arithmetic Obfuscation by Program Synthesis and Term Rewriting (ProMBA) | Lee, Lee | ACM CCS 2023 | [ACM DL](https://dl.acm.org/doi/10.1145/3576915.3623186) |

### Classical Techniques

| Technique | Reference |
|-----------|-----------|
| Möbius transform / ANF | Barbier, Cheballah, Le Bars — [On the computation of the Möbius transform](https://www.sciencedirect.com/science/article/pii/S0304397519307674) (TCS 2019) |
| Shannon decomposition | Boole, *Laws of Thought* (1854); Shannon (1949) — [Wikipedia](https://en.wikipedia.org/wiki/Boole%27s_expansion_theorem) |
| Hensel lifting (modular inverse) | Dumas — [On Newton-Raphson iteration for multiplicative inverses modulo prime powers](https://arxiv.org/abs/1209.6626) (2012) |
| Finite differences / falling factorial | Newton, *Principia Mathematica* (1687) — [Wikipedia](https://en.wikipedia.org/wiki/Newton_polynomial) |

### Dataset Sources

| Tool | Authors | Venue | Link |
|------|---------|-------|------|
| NeuReduce | Feng, Liu, Xu, Zheng, Xu | EMNLP 2020 | [ACL Anthology](https://aclanthology.org/2020.findings-emnlp.56/) |
| Syntia | Blazytko, Contag, Aschermann, Holz | USENIX Security 2017 | [USENIX](https://www.usenix.org/conference/usenixsecurity17/technical-sessions/presentation/blazytko) |
| QSynth | David, Coniglio, Ceccato | BAR 2020 | [PDF](https://archive.bar/pdfs/bar2020-preprint9.pdf) |
| LOKI | Schloegel, Blazytko, Contag, Aschermann, Basler, Holz, Abbasi | USENIX Security 2022 | [USENIX](https://www.usenix.org/conference/usenixsecurity22/presentation/schloegel) |
