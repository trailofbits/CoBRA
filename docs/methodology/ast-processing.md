# AST Processing Passes

These passes operate on work items in the `kFoldedAst` state -- the expression AST after constant folding. They perform structural analysis, prerequisite lowering, and algebraic rewrites that prepare the expression for technique-specific passes (signature, semilinear, decomposition).

`kLowerNotOverArith` and `kClassifyAst` run during seeding before the scheduler loop. Once a `kFoldedAst` item is in the scheduler, the `kFoldedAstPasses` table orders signature-state construction, direct-remainder prep, decomposition, lifting, and the structural rewrite passes. The structural rewrites (operand simplification, product identity collapse, XOR lowering) only run on expressions flagged as exploration candidates -- those with `kSfHasMixedProduct` or `kSfHasBitwiseOverArith` structure.


## NOT-over-Arithmetic Lowering

**Pass:** `kLowerNotOverArith`
**Source:** [Simplifier.cpp](../../lib/core/Simplifier.cpp) (implementation), [OrchestratorPasses.cpp](../../lib/core/OrchestratorPasses.cpp) (pass wrapper)

A prerequisite pass that normalizes NOT applied to purely arithmetic subexpressions. The identity:

```
~e  =  -e + (2^w - 1)
```

where `w` is the bit width, rewrites NOT-over-addition into pure arithmetic. For example, `~(a + b)` becomes `-(a + b) + 0xFFFFFFFFFFFFFFFF` at 64-bit width. This runs before classification because the NOT wrapping can mask the expression's true structure, causing misclassification.

The pass only fires on items with `Provenance::kOriginal`. It emits a new work item with `Provenance::kLowered` and recomputes the Boolean signature for the lowered expression.


## Classification

**Pass:** `kClassifyAst`
**Source:** [Classifier.cpp](../../lib/core/Classifier.cpp)

Walks the AST bottom-up and computes a `Classification` consisting of a `SemanticClass` and a set of `StructuralFlag` bits. These determine which technique families and structural rewrites are applicable.

### Structural flags

| Flag | Meaning |
|------|---------|
| `kSfHasBitwise` | Any bitwise operator present |
| `kSfHasArithmetic` | Any arithmetic operator present |
| `kSfHasMul` | Variable-variable multiplication |
| `kSfHasMultilinearProduct` | Product of two variables with distinct variable sets |
| `kSfHasSingletonPower` | Single variable raised to degree >= 2 |
| `kSfHasSingletonPowerGt2` | Single variable raised to degree > 2 |
| `kSfHasMultivarHighPower` | Multiple variables with max degree >= 2 |
| `kSfHasMixedProduct` | Product where at least one operand contains non-leaf bitwise ops |
| `kSfHasBitwiseOverArith` | Bitwise op wrapping variable-dependent arithmetic |
| `kSfHasArithOverBitwise` | Arithmetic op wrapping non-leaf bitwise children |
| `kSfHasUnknownShape` | Unrecognized structure |

### Semantic class

The flags combine into a `SemanticClass`:

- **kLinear** -- No variable-variable products and no constants in bitwise ops.
- **kSemilinear** -- Constants inside bitwise operations but no variable-variable products. Routes to semilinear technique passes.
- **kPolynomial** -- Variable-variable products without mixed products or bitwise-over-arithmetic.
- **kNonPolynomial** -- Products combined with mixed structure or bitwise-over-arithmetic.

The classifier also folds constant bitwise subtrees (via `FoldConstantBitwise`) as a side effect, simplifying expressions like `AND(NOT(Constant(C)), x)` down to `AND(Constant(~C), x)` before flag computation.


## Operand Simplification

**Pass:** `kOperandSimplify`
**Source:** [OrchestratorPasses.cpp](../../lib/core/OrchestratorPasses.cpp)

Targets binary multiplication nodes where at least one operand contains non-leaf bitwise operations -- the mixed-product pattern. The pass finds the first such site in the AST, then emits each bitwise operand as an independent child work item that enters the signature-based passes for simplification.

The child items are linked through an `OperandJoinState`. When both operands resolve (either simplified or left unchanged), the join recombines them into the product and substitutes the result back into the full AST. The recombined expression re-enters the worklist for further processing.

Non-bitwise operands are pre-resolved in the join state immediately, so the join does not wait for children that were never emitted.

This pass is a structural transform: it increments the rewrite generation counter and requires `kExtractProductCore` to have been attempted first (prerequisite mask).


## Product Identity Collapse

**Pass:** `kProductIdentityCollapse`
**Source:** [OrchestratorPasses.cpp](../../lib/core/OrchestratorPasses.cpp)

Recognizes the MBA product identity:

```
x * y  =  (x & y) * (x | y)  +  (x & ~y) * (~x & y)
```

Obfuscators use this identity to replace a simple product with a sum of two bitwise-product terms. The pass searches for `Add(Mul(f0, f1), Mul(f2, f3))` subtrees and checks whether the four factors can be assigned to the roles intersection, union, left-only, and right-only.

Validation uses Boolean signatures evaluated at {0,1}^n inputs. For each of the 8 possible role assignments, the pass checks two constraints:

1. **Pairwise disjoint:** The intersection, left-only, and right-only factors are mutually disjoint at every Boolean input point.
2. **Union coverage:** The union factor equals the bitwise OR of the other three at every Boolean input point.

When a valid assignment is found, the pass recovers `x = intersection | left` and `y = intersection | right`, then emits both as child work items through a `ProductJoinState`. Each child enters the signature passes independently, and the join reconstructs `x * y` when both resolve.

Up to 4 valid role assignments are explored in parallel as competing alternatives.

This pass is a structural transform with a prerequisite on `kExtractProductCore`.


## XOR Lowering

**Pass:** `kXorLowering`
**Source:** [MixedProductRewriter.cpp](../../lib/core/MixedProductRewriter.cpp) (rewriter), [OrchestratorPasses.cpp](../../lib/core/OrchestratorPasses.cpp) (pass wrapper)

Converts XOR nodes into pure arithmetic using the identity:

```
x ^ y  =  x + y - 2 * (x & y)
```

XOR blocks polynomial analysis because it is a bitwise operator that does not distribute over addition. After rewriting, the expression contains only addition, multiplication, negation, and AND -- a form where polynomial recovery passes become applicable.

The rewriter is context-sensitive: it only rewrites XOR nodes that appear inside a mixed-product or bitwise-over-arithmetic context. XOR nodes in purely bitwise subtrees are left alone.

### Bounds

The rewriter enforces two limits to prevent expression explosion:

- **Max rounds** (default 2): The rewriter iterates, re-classifying after each round. It stops when no further XOR sites remain or no progress is made.
- **Max node growth** (default 3x): If the rewritten AST exceeds 3x the original node count, the round is aborted.

Each round also checks for regressions: if new unsupported flags appear that were not present before the rewrite, the round is rejected.

After rewriting, the pass re-classifies the expression. If the result still has unrecovered mixed structure (`NeedsStructuralRecovery` returns true), the pass reports `kBlocked` with a `kRepresentationGap` reason rather than emitting the unhelpful intermediate form.


## Rewrite Generation Budget

The three structural rewrite passes -- operand simplification, product identity collapse, and XOR lowering -- share a rewrite generation counter on each work item. The scheduler enforces a maximum (default 3, set in `OrchestratorPolicy::max_rewrite_gen`):

| Generation | State |
|:----------:|-------|
| 0 | Original or lowered expression |
| 1 | After first structural rewrite |
| 2 | After second rewrite on the result |
| 3 | Maximum -- no further structural rewrites allowed |

Once the budget is exhausted, only non-structural passes (technique solvers, decomposition, lifting) remain eligible. This forces the orchestrator to commit to solving the expression in its current form rather than endlessly reshaping it.

The scheduler also deprioritizes structural transforms relative to technique passes. In the `kFoldedAstPasses` priority table, signature state construction and decomposition prep run at priorities 0-8, while the three structural rewrites run at priorities 9-11. This means the orchestrator always tries direct solving before falling back to structural rewrites.
