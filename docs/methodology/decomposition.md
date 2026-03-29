# Decomposition

These passes handle the most challenging MBA expressions -- those containing products of bitwise subexpressions like `(x & y) * (x | y)`. The decomposition engine uses an extract-solve architecture: extract a polynomial core, compute the residual, classify it, and solve it with specialized solvers.

Reference: Reichenwallner et al. -- [GAMBA](https://arxiv.org/abs/2305.06763) (2023), whose core-residual split inspired this design.


## Overview

The decomposition family spans two state kinds:

- **kCoreCandidate** -- a polynomial core has been extracted from the expression.
- **kRemainderState** -- the residual after subtracting the core, ready for solving.

The scheduler tries multiple core extractors as alternative passes on the same `kFoldedAst` item. A successful extractor emits a `kCoreCandidate`, which leads to a `kRemainderState`. Residual solving may in turn create a nested competition group when a child signature solve or continuation needs to recombine results, but top-level decomposition is not wrapped in its own global extractor race.


## Core Extraction

Three extractors produce candidate polynomial cores from `kFoldedAst` items. Each uses a different strategy and runs at a different cost tier. The original AST item is retained so later extractors can still run if earlier ones do not finish the simplification.

### Product Core Extraction

Pass: `kExtractProductCore`
Source: [DecompositionPasses.cpp](../../lib/core/DecompositionPasses.cpp), [DecompositionEngine.cpp](../../lib/core/DecompositionEngine.cpp)

Walks the add-tree of the AST and identifies variable-variable products -- `Mul(a, b)`, `Neg(Mul(a, b))`, and `Not(Mul(a, b))` forms where neither operand is a constant. The extracted products become the polynomial core; everything else goes to the residual. This is the cheapest extractor since it works purely from AST structure with no evaluation.

### Polynomial Core Extraction

Passes: `kExtractPolyCoreD2`, `kExtractPolyCoreD3`, `kExtractPolyCoreD4`
Source: [DecompositionPasses.cpp](../../lib/core/DecompositionPasses.cpp), [MultivarPolyRecovery.cpp](../../lib/core/MultivarPolyRecovery.cpp)

Recovers a multivariate polynomial approximation via `RecoverMultivarPoly` at degree 2, 3, or 4. Each degree level is a separate pass, letting the scheduler try lower degrees first (cheaper) before escalating.

The recovery uses tensor-product forward differences on a `{0, ..., d}^k` evaluation grid, where `d` is the degree and `k` is the number of support variables. Forward differences are converted to falling-factorial basis coefficients `h` via:

```
h = (alpha >> q) * ModInverseOdd(odd_part_factorial, prec)   mod 2^prec
```

where `q` counts the factors of 2 contributed by the factorial denominators and `prec = bitwidth - q` is the remaining precision. This is the standard 2-adic normalization from polynomial interpolation over Z/(2^n).

Reference: Gamez-Montolio et al. -- [Efficient Normalized Reduction](https://www.ndss-symposium.org/ndss-paper/auto-draft-436/) (BAR/NDSS 2024)

### Template Core Extraction

Pass: `kExtractTemplateCore`
Source: [DecompositionPasses.cpp](../../lib/core/DecompositionPasses.cpp), [TemplateDecomposer.cpp](../../lib/core/TemplateDecomposer.cpp)

Bounded template matching using a pool of candidate subexpressions. The pool contains constants, variables, unary operations (negation, bitwise NOT), and pairwise combinations (AND, OR, XOR, ADD, MUL) plus their negations and complements.

Search proceeds in layers:

1. **Direct match** -- target matches an atom in the pool.
2. **Layer 1** -- `target = G(A, B)` for atoms A, B and gate G. Invertible gates (XOR, ADD) use hash lookup; AND/OR/MUL use filtered brute-force.
3. **Layer 2** -- `target = G_out(A, G_in(B, C))`. Precomputes all inner compositions `G_in(B, C)` into an indexed cache, then searches outer gates.
4. **Unary wrapping** -- checks `Neg(target)` and `Not(target)` against layers 1-2.
5. **Layer 3** -- `target = G1(A, G2(B, R))` with both G1, G2 invertible. O(pool^2) hash lookups.

Each candidate is verified with full-width random probes before acceptance. This is the most expensive extractor and runs last.


## Remainder Preparation

Two passes prepare the residual for downstream solvers:

**kPrepareDirectRemainder** -- Used when the expression itself is a boolean-null candidate (zero on all Boolean inputs but nonzero at full width). Computes a remainder without core subtraction.

**kPrepareRemainderFromCore** -- Given a core candidate, builds a compiled evaluator for `r(x) = f(x) - core(x)` via [DecompositionEngine.cpp](../../lib/core/DecompositionEngine.cpp). The remainder carries metadata about its origin (product core, polynomial core, template core) via `RemainderOrigin`.

For polynomial cores, `AcceptCore` validates the core with 5 deterministic probes before a remainder is emitted: the core must be non-trivial (residual differs from the original) and non-exhaustive (residual is not identically zero). Product and template cores do not use that acceptance gate.


## Residual Classification

The residual falls into one of three categories:

1. **Zero** -- the core alone explains the expression. No further solving needed.
2. **Polynomial** -- the residual is a polynomial function recoverable via interpolation.
3. **Boolean-null** -- the residual is zero on all {0,1}^n Boolean inputs but nonzero at some full-width point. This is a "ghost" -- invisible to Boolean-domain analysis.

Classification is performed by `IsBooleanNullResidual` in [GhostResidualSolver.cpp](../../lib/core/GhostResidualSolver.cpp): first check that all Boolean-signature entries are zero, then probe 8 deterministic mixed-parity points to confirm nonzero behavior at full width.


## Residual Solvers

Six solver passes operate on `kRemainderState`, tried in scheduler priority order.

### Supported Pipeline

Pass: `kResidualSupported`
Source: [DecompositionPasses.cpp](../../lib/core/DecompositionPasses.cpp), [SignaturePasses.cpp](../../lib/core/SignaturePasses.cpp)

Runs the standard signature-based family on the residual by emitting a residual `kSignatureState` child with an evaluator override. The recombination continuation then applies a hardened 64-probe recheck rather than the default 8 to reject boolean-correct but full-width-incorrect false positives. This is the strongest verification gate in the system.

### Polynomial Recovery

Pass: `kResidualPolyRecovery`
Source: [MultivarPolyRecovery.cpp](../../lib/core/MultivarPolyRecovery.cpp)

Attempts falling-factorial polynomial recovery on the residual via `RecoverAndVerifyPoly` with degree escalation. Iteratively increases degree from a minimum bound to a cap, accepting the first degree that passes full-width verification.

### Ghost Residual Solving

Pass: `kResidualGhost`
Source: [GhostResidualSolver.cpp](../../lib/core/GhostResidualSolver.cpp)

For boolean-null residuals, attempts to express the residual as `c * g(tuple)` where `c` is a constant and `g` is a ghost primitive from the [ghost basis library](../../lib/core/GhostBasis.cpp).

The coefficient `c` is inferred via 2-adic arithmetic: find the probe point where the ghost value `g` has the fewest trailing zeros (lowest 2-adic valuation `t`), shift both the residual value and ghost value right by `t`, compute the modular inverse of the odd part at precision `bitwidth - t`, and multiply. All other probe points are cross-checked against `c * g`.

### Factored Ghost Solving

Pass: `kResidualFactoredGhost`
Source: [GhostResidualSolver.cpp](../../lib/core/GhostResidualSolver.cpp)

Extends ghost solving to `residual = q(x) * g(tuple)` where `q` is a polynomial quotient. For each ghost primitive and variable tuple, constructs a weight function `w(x) = g(tuple(x))` and calls [WeightedPolyFit](../../lib/core/WeightedPolyFit.cpp) to recover `q`.

WeightedPolyFit solves the system `w(x_i) * q(x_i) = r(x_i)` in 2-adic arithmetic. The key challenge is precision loss: when the weight `w` has high 2-adic valuation (many trailing zeros), the effective precision drops. The solver handles this via forward elimination at reduced precision and back-substitution on the pivot system. Specifically, for each pivot column it selects the row with the lowest 2-adic valuation, eliminates at precision `bitwidth - t`, and back-substitutes to recover full-precision coefficients.

### Escalated Factored Ghost

Pass: `kResidualFactoredGhostEscalated`

Same as factored ghost but at a higher polynomial degree. Separate pass so the scheduler tries the cheaper version first.

### Template Fallback

Pass: `kResidualTemplate`
Source: [TemplateDecomposer.cpp](../../lib/core/TemplateDecomposer.cpp)

Last resort: bounded template matching on the residual. Most expensive solver, used when all structured approaches fail.


## Ghost Primitives

Source: [GhostBasis.cpp](../../lib/core/GhostBasis.cpp)

Functions that are identically zero on Boolean inputs but nonzero at full width:

| Primitive | Definition | Arity |
|-----------|-----------|:-----:|
| `mul_sub_and(x, y)` | `x*y - (x & y)` | 2 |
| `mul3_sub_and3(x, y, z)` | `x*y*z - (x & y & z)` | 3 |

These arise naturally in MBA obfuscation: they can be added to any expression without changing Boolean-domain behavior, but they alter full-width semantics. On {0,1} inputs, `x*y` equals `x & y` (since multiplication and conjunction are identical on single bits), so their difference vanishes. At full width -- for example, x=3, y=5 -- `3*5 = 15` but `3 & 5 = 1`, giving `mul_sub_and(3, 5) = 14`.

Both primitives are symmetric (operand order does not matter). The solver enumerates all strictly increasing combinations of support variables for each primitive's arity.


## Recombination

When a residual solver succeeds, the result is recombined with its core as `core + solved_residual`. Most residual solvers do this immediately and emit a `kCandidateExpr` directly. The `kResidualSupported` path instead resolves through a child competition group and `RemainderRecombineCont`, because the residual itself is solved by a nested signature-style search.

If decomposition is already running under a parent group, the recombined result is submitted there. Otherwise the verified `kCandidateExpr` returns directly to the worklist, and top-level acceptance still follows worklist order rather than a dedicated decomposition-wide competition group.


## Verification Gate

All decomposition results are gated by full-width verification before acceptance. The supported-pass residual path uses a hardened 64-probe recheck to catch boolean-correct but full-width-incorrect false positives. This is a particular risk with decomposition because core subtraction can create residuals where Boolean-domain equivalence does not imply full-width equivalence.

Reference: Blazytko et al. -- [SiMBA](https://arxiv.org/abs/2209.06335) (2022), Section 4.2 discusses the boolean-null phenomenon and why full-width verification is necessary.
