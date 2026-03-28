# CoBRA Methodology

This document describes the architecture and techniques behind CoBRA's Mixed Boolean-Arithmetic (MBA) expression simplifier. For the individual methods, see the [Technique Index](techniques.md). For the mathematical details behind each technique, see the referenced papers.

## Background

MBA expressions interleave arithmetic operators (`+`, `-`, `*`) with bitwise operators (`&`, `|`, `^`, `~`) and shifts (`<<`, `>>`). This combination defeats both algebraic simplification (which expects pure arithmetic) and Boolean minimization (which expects pure logic). MBA obfuscation exploits this gap — a simple expression like `x + y` can be rewritten as `(x & y) + (x | y)`, and layered rewrites make it arbitrarily complex.

CoBRA reverses this process. Given an obfuscated MBA expression, it recovers a simplified equivalent by combining [signature-based techniques](signature-techniques.md), [semilinear techniques](semilinear-techniques.md), [decomposition](decomposition.md), and [lifting](lifting.md) under a worklist-based orchestrator. Many of the signature-side techniques are rooted in the observation that linear MBA subproblems over *n* variables are fully determined by their values on the 2^n Boolean inputs {0, 1}^n. The rest of the pipeline adds full-width verification and residual handling for cases where Boolean data alone is not enough.

**Foundational reference:** Zhou et al., [Information Hiding in Software with Mixed Boolean-Arithmetic Transforms](https://www.researchgate.net/publication/221239701_Information_Hiding_in_Software_with_Mixed_Boolean-Arithmetic_Transforms) (WISA 2007) — the original formalization of MBA obfuscation.

## Architecture Overview

CoBRA uses a **worklist-based orchestrator** that treats simplification as a graph exploration problem. An input expression enters the worklist as a **work item** carrying a **state kind** that describes its current form. A **scheduler** selects passes to transform items through successive state kinds until a verified simplified expression is found.

```
Input Expression
       |
 [Seed: lower / classify]
       |
   kFoldedAst
    |  |  |  |
    |  |  |  +--> kLiftedSkeleton --> kFoldedAst
    |  |  +-----> kCoreCandidate --> kRemainderState --> kCandidateExpr
    |  +--------> kSemilinearNormalizedIr --> kSemilinearCheckedIr
    |                                           |
    |                                           v
    |                                   kSemilinearRewrittenIr --> kCandidateExpr
    |
   +-----------> kSignatureState --> kSignatureCoeffState --> kCandidateExpr
                                   \--> kCompetitionResolved --> kCandidateExpr

   kCandidateExpr --> Verification or Immediate Acceptance --> Simplified Expression
```

The decomposition family can enter `kRemainderState` either directly via `kPrepareDirectRemainder` (boolean-null path) or by first producing a `kCoreCandidate`; the diagram shows the core-first branch.

Key architectural properties:

- **Pass registry**: 36 discrete passes, each consuming a specific state kind and producing 0+ child items with new state kinds.
- **DAG-aware scheduling**: Passes declare prerequisite dependencies. The scheduler respects these and consults an attempt cache to avoid redundant work.
- **Competition groups**: Specific passes that fork alternatives or child solves use local cost-based winner selection and continuations to recombine the winner. Outside those groups, the worklist returns the first fully verified top-level candidate.
- **Deduplication**: A fingerprint-based cache prevents re-running the same pass on equivalent states, bounding the search space.
- **Policy bounds**: Configurable limits on total work item expansions and structural rewrite generations prevent runaway exploration.

See [orchestrator.md](orchestrator.md) for the full architectural details.

## Classification

The **ClassifyAst** pass performs structural analysis of the expression AST to determine which technique families are applicable. It sets structural flags:

| Flag | Meaning | Example |
|------|---------|---------|
| `kSfHasMultilinearProduct` | Variable-variable multiplication with distinct variable sets | `x * y` |
| `kSfHasSingletonPower` | Single variable raised to a power | `x * x` |
| `kSfHasMixedProduct` | Product of bitwise subexpressions | `(x & y) * (x \| y)` |
| `kSfHasBitwiseOverArith` | Bitwise op wrapping arithmetic | `(x + y) & z` |
| `kSfHasArithOverBitwise` | Arithmetic op combining non-leaf bitwise children | `(x & y) + (x \| y)` |

These flags determine which passes the scheduler will consider for the item. Semilinear eligibility is tracked separately as `SemanticClass::kSemilinear` when constants appear inside bitwise operators; there is no standalone `HasConstantMask` scheduler flag.

## Auxiliary Variable Elimination

Before entering technique-specific passes, CoBRA checks whether any variables cancel out of the expression. If the signature vector is invariant under toggling a particular variable's input bit, that variable contributes nothing and is eliminated. This reduces the problem dimension.

## Verification

After simplification, CoBRA validates correctness:

- **Spot-check** (default): Evaluates original and simplified expressions on random inputs and confirms they match at full bitwidth.
- **Z3 equivalence proof** (optional, `--verify`): Constructs a formal equivalence query and proves the result is correct for all inputs.

## Technique Documentation

1. [Orchestrator Architecture](orchestrator.md) — Worklist model, pass registry, scheduling, competition groups
2. [AST Processing](ast-processing.md) — Classification, structural rewrites, operand simplification
3. [Signature Techniques](signature-techniques.md) — [Signature Vector](techniques.md#signature-vector), [Pattern Matching](techniques.md#pattern-matching), [Change of Basis (CoB) Butterfly Transform](techniques.md#change-of-basis-cob-butterfly-transform), [Algebraic Normal Form (ANF)](techniques.md#algebraic-normal-form-anf), [Shannon Decomposition](techniques.md#shannon-decomposition), [Coefficient Splitting](techniques.md#coefficient-splitting), [Singleton Power Recovery](techniques.md#singleton-power-recovery)
4. [Semilinear Techniques](semilinear-techniques.md) — [Linear Shortcut Detection](techniques.md#linear-shortcut-detection), [Constant Lowering (Semilinear)](techniques.md#constant-lowering-semilinear), [Structure Recovery (XOR Recovery)](techniques.md#structure-recovery-xor-recovery), [Bit Partitioning](techniques.md#bit-partitioning)
5. [Decomposition](decomposition.md) — [Decomposition Engine (Extract-Solve)](techniques.md#decomposition-engine-extract-solve), [Boolean-Null Residual Classification](techniques.md#boolean-null-residual-classification), [Ghost Residual Solving](techniques.md#ghost-residual-solving), [WeightedPolyFit](techniques.md#weightedpolyfit)
6. [Lifting](lifting.md) — [Arithmetic Atom Lifting](techniques.md#arithmetic-atom-lifting), [Repeated Subexpression Lifting](techniques.md#repeated-subexpression-lifting)

See the **[Technique Index](techniques.md)** for an alphabetical reference of all techniques with cross-links.

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

| Technique | Where Used | Reference |
|-----------|------------|-----------|
| Mobius transform / ANF | [Mobius Transform](techniques.md#mobius-transform) · [Algebraic Normal Form (ANF)](techniques.md#algebraic-normal-form-anf) · [Packed Mobius Transform](signature-techniques.md#packed-mobius-transform) | Barbier, Cheballah, Le Bars — On the computation of the Mobius transform (TCS 2019) |
| Shannon decomposition | [Shannon Decomposition](techniques.md#shannon-decomposition) · [Signature Techniques](signature-techniques.md#shannon-decomposition-4-5-variables) | Boole, Laws of Thought (1854); Shannon (1949) |
| Hensel lifting (modular inverse) | [Hensel Lifting](techniques.md#hensel-lifting) · [Coefficient Extraction](signature-techniques.md#coefficient-extraction) | Dumas — On Newton-Raphson iteration for multiplicative inverses modulo prime powers (2012) |
| Finite differences / falling factorial | [Finite Differences (Forward)](techniques.md#finite-differences-forward) · [Falling-Factorial Interpolation](techniques.md#falling-factorial-interpolation) · [Algorithm](signature-techniques.md#algorithm) · [Coefficient Extraction](signature-techniques.md#coefficient-extraction) | Newton, Principia Mathematica (1687) |

### Dataset Sources

| Tool | Authors | Venue | Link |
|------|---------|-------|------|
| NeuReduce | Feng, Liu, Xu, Zheng, Xu | EMNLP 2020 | [ACL Anthology](https://aclanthology.org/2020.findings-emnlp.56/) |
| Syntia | Blazytko, Contag, Aschermann, Holz | USENIX Security 2017 | [USENIX](https://www.usenix.org/conference/usenixsecurity17/technical-sessions/presentation/blazytko) |
| QSynth | David, Coniglio, Ceccato | BAR 2020 | [PDF](https://archive.bar/pdfs/bar2020-preprint9.pdf) |
| LOKI | Schloegel, Blazytko, Contag, Aschermann, Basler, Holz, Abbasi | USENIX Security 2022 | [USENIX](https://www.usenix.org/conference/usenixsecurity22/presentation/schloegel) |
| OSES | Matteo | 2024 | [GitHub](https://github.com/fvrmatteo/oracle-synthesis-meets-equality-saturation) |
