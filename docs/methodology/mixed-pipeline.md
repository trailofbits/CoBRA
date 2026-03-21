# Mixed Pipeline

The mixed pipeline handles the most challenging class of MBA expressions: those containing products of bitwise subexpressions, such as `(x & y) * (x | y)`. These resist both the linear approach (which assumes no products) and the polynomial approach (which assumes products of plain variables).

**Example:** `(x & y) * (x | y) + (x & ~y) * (~x & y)` simplifies to `x * y`

## When This Pipeline Runs

The classifier routes an expression here when it detects:

- **Mixed products**: Multiplication where at least one operand is a bitwise subexpression (e.g., `(x & y) * (x | y)`)
- **Bitwise over arithmetic**: Bitwise operations applied to arithmetic results (e.g., `(x + y) & z`)

These patterns cannot be handled by direct evaluation at Boolean points because the product structure creates dependencies that the CoB transform cannot resolve in a single pass.

## Pipeline Stages

The mixed pipeline uses a multi-step MixedRewrite pipeline followed by a decomposition engine. Each step either produces a simplified result (verified at full width) or passes control to the next step.

```
Expression
    |
[Step 1: Opportunistic] ── try standard pipeline directly
    |  (success? → done)
    |
[Step 1.5: Early Decomposition] ── decompose original AST before preconditioning
    |  (success? → done)
    |
[Step 2: Operand Simplification] ── simplify operands, retry standard pipeline
    |  (success? → done)
    |
[Step 2.5: Product Identity Collapse] ── collapse MBA identities, retry
    |  (success? → done)
    |
[Phase 2: Decomposition Engine] ── extract-solve loop on preconditioned AST
    |  Extract polynomial core → solve residual
    |  (success? → done)
    |
[Step 3: XOR Lowering] ── algebraic rewriting, re-enter classification
    |  (success? → done)
    |
Unsupported (return best effort or original)
```

## Step 1: Opportunistic Standard Pipeline

Before applying mixed-specific techniques, CoBRA attempts to run the standard pipeline (linear or polynomial) directly on the expression. Some structurally mixed expressions can be simplified without specialized handling — for example, when the mixed products cancel out at full width. If the standard pipeline produces a verified result, it is accepted immediately.

## Step 1.5: Early Decomposition

The decomposition engine is run on the **original folded AST** before any preconditioning rewrites. Product cores that directly equal f(x) exist in the obfuscated form (as sums of `Mul(non-const, non-const)` terms) but are destroyed by operand simplification, which restructures the AST and introduces non-product terms. By trying decomposition first, these direct-match product cores are captured before they are lost.

## Step 2: Operand Simplification

The **OperandSimplifier** recursively simplifies the operands of mixed product nodes. If a product like `(complex_bitwise_expr) * (another_expr)` has operands that can be individually simplified, the overall expression may become tractable for the standard pipeline.

Each candidate replacement is verified at full width by probing the original and simplified operands on random inputs. This prevents a class of semantic corruption where an operand simplifies to a boolean-equivalent but full-width-incorrect expression — a replacement that is invisible on {0,1} inputs but alters the product's behavior at arbitrary values.

After operand simplification, CoBRA re-attempts the standard pipeline. If the simplified operands reveal a linear or polynomial structure, the expression is resolved here.

## Step 2.5: Product Identity Collapse

The **ProductIdentityRecoverer** recognizes specific MBA product identities and collapses them. The primary identity is:

```
x * y = (x & y) * (x | y) + (x & ~y) * (~x & y)
```

This identity holds for all integer values, not just Boolean inputs. It is commonly used in obfuscation to replace a simple product with a complex sum of bitwise-product terms.

The recoverer checks whether the expression matches one of 8 role assignments (permutations of the factors) and, if so, collapses it back to the simple product form. It validates the match using Boolean-cube constraints on the factor signatures. After collapsing, the standard pipeline is re-attempted.

## Phase 2: Decomposition Engine

The **DecompositionEngine** uses an extract-solve architecture to decompose mixed expressions into a polynomial core plus a residual that can be solved independently.

### Core Extraction

Three extractors produce candidate polynomial cores:

1. **ExtractProductCore**: Identifies variable-variable products in the add-tree of the AST — including `Mul(a,b)`, `-(Mul(a,b))`, and `~(Mul(a,b))` forms — and builds a polynomial core from these terms.
2. **ExtractPolyCore**: Recovers a multivariate polynomial approximation via `RecoverMultivarPoly` at degree 2 or 3.
3. **ExtractTemplateCore**: Uses bounded template matching to find a polynomial expression that partially explains the target function.

### Residual Solving

For each candidate core, the engine computes a **residual evaluator**: `r(x) = f(x) - core(x)`. The residual is what remains after subtracting the core from the target expression. The engine then classifies and solves the residual:

1. **Zero residual**: If the residual is identically zero, the core alone is the answer.

2. **Polynomial residual**: The engine attempts falling-factorial polynomial recovery on the residual via `RecoverAndVerifyPoly` with degree escalation. This iteratively increases the polynomial degree from a minimum bound up to a cap, accepting the first degree that passes full-width verification.

3. **Boolean-null residual**: If the residual is zero on all {0,1}^n Boolean inputs but nonzero at some full-width point, it is classified as **boolean-null** by the `IsBooleanNullResidual` classifier. This indicates a "ghost" — a function invisible to Boolean-domain analysis. The engine then applies two ghost solvers in sequence:

   - **SolveGhostResidual**: Attempts to express the residual as `c * g(tuple)`, where `c` is a constant and `g` is a ghost primitive from the `GhostBasis` library (e.g., `mul_sub_and(x,y) = x*y - (x&y)`). Uses 2-adic coefficient inference with mixed-parity probe points.

   - **SolveFactoredGhostResidual**: Attempts to express the residual as `q(x) * g(tuple)`, where `q` is a polynomial quotient recovered via `WeightedPolyFit` (a 2-adic weighted linear solver). Enumerates ghost primitives in priority order with the ghost function as the weight.

4. **Template fallback**: If no polynomial or ghost solver succeeds, the engine falls back to template decomposition on the residual.

All solver results are gated by `FullWidthCheckEval` before acceptance. The supported-pipeline residual path uses a hardened 64-probe recheck (rather than the default 8) to reject boolean-correct but full-width-incorrect false positives.

### Ghost Primitives

The `GhostBasis` library provides functions that are identically zero on Boolean inputs but nonzero at full width:

| Primitive | Definition | Property |
|-----------|-----------|----------|
| `mul_sub_and(x,y)` | `x*y - (x&y)` | Zero when both inputs are 0 or 1 |
| `mul3_sub_and3(x,y,z)` | `x*y*z - (x&y&z)` | Zero when all three inputs are 0 or 1 |

These "ghost" functions arise naturally in MBA obfuscation: they can be added to any expression without changing its Boolean-domain behavior, but they alter the full-width semantics.

## Step 3: XOR Lowering

The **MixedProductRewriter** applies algebraic identities to eliminate XOR from product subtrees. The primary rewrite is:

```
x ^ y  →  x + y - 2*(x & y)
```

This converts XOR (a bitwise operator that blocks polynomial analysis) into pure arithmetic over AND, which the polynomial pipeline can handle. The rewriter walks the expression tree, identifies XOR nodes within product subtrees, and applies this substitution.

After rewriting, the expression is re-classified. If the products are now between plain variables and AND-terms, it can enter the polynomial pipeline. The rewriter limits the number of rounds and monitors node growth to avoid expression explosion.

## Recursive Simplification

When any step succeeds in decomposing a mixed expression, the resulting subexpressions are fed back through CoBRA's classifier for further simplification. This recursive approach allows CoBRA to handle layered obfuscation where multiple techniques are stacked.

## Known Limitations

Some mixed expressions remain unsupported:

- **Product-inside-bitwise** — expressions where products are nested inside bitwise operators (`(a*b) & c`, `(a*b) ^ (c*d)`) rather than at the top-level add-tree. These account for ~48% of remaining unsupported cases. The current pipeline can only extract products from add-tree leaves; bitwise-wrapped products require a new representation family.
- **Boolean-null residuals** — residuals that are zero on all Boolean inputs but nonzero at full width, where neither ghost primitives nor polynomial quotients produce a match. These account for ~39% of remaining cases.
- **Non-boolean-null mixed residuals** — product cores with residuals containing nested MBA-encoded max/min chains that resist both polynomial recovery and structural simplification. ~13% of remaining cases.
- **Complex shift interactions**: Deeply nested combinations of shifts and bitwise products where no rewrite rule applies.

The QSynth EA dataset contains the most challenging examples — CoBRA simplifies 288 of 500 expressions, with the remaining 212 falling outside current decomposition coverage (including arithmetic-under-bitwise expressions that fail full-width verification).
