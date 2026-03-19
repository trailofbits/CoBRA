# Mixed Pipeline

The mixed pipeline handles the most challenging class of MBA expressions: those containing products of bitwise subexpressions, such as `(x & y) * (x | y)`. These resist both the linear approach (which assumes no products) and the polynomial approach (which assumes products of plain variables).

**Example:** `(x & y) * (x | y) + (x & ~y) * (~x & y)` simplifies to `x * y`

## When This Pipeline Runs

The classifier routes an expression here when it detects:

- **Mixed products**: Multiplication where at least one operand is a bitwise subexpression (e.g., `(x & y) * (x | y)`)
- **Bitwise over arithmetic**: Bitwise operations applied to arithmetic results (e.g., `(x + y) & z`)

These patterns cannot be handled by direct evaluation at Boolean points because the product structure creates dependencies that the CoB transform cannot resolve in a single pass.

## Pipeline Stages

The mixed pipeline tries multiple decomposition strategies in order of increasing cost. The first strategy to produce a valid, simpler result wins.

```
Expression
    |
[MixedProductRewriter] ── algebraic lowering of products
    |  (success? → re-enter classification)
    |
[HybridDecomposer] ── variable-extraction decomposition
    |  (success? → done)
    |
[TemplateDecomposer] ── bounded template matching
    |  (success? → done)
    |
[BitwiseDecomposer] ── gate enumeration reconstruction
    |  (success? → done)
    |
[ProductIdentityRecoverer] ── MBA product identity collapse
    |  (success? → done)
    |
Unsupported (return best effort or original)
```

## Strategy 1: Mixed Product Rewriting

The **MixedProductRewriter** applies algebraic identities to eliminate mixed products. The primary rewrite is **XOR lowering**:

```
x ^ y  →  x + y - 2*(x & y)
```

This converts XOR (a bitwise operator that blocks polynomial analysis) into pure arithmetic over AND, which the polynomial pipeline can handle. The rewriter walks the expression tree, identifies XOR nodes within product subtrees, and applies this substitution.

After rewriting, the expression is re-classified. If the products are now between plain variables and AND-terms, it can enter the polynomial pipeline. The rewriter limits the number of rounds and monitors node growth to avoid expression explosion.

## Strategy 2: Hybrid Decomposition

The **HybridDecomposer** uses a variable-extraction technique. The idea is to peel off one variable at a time using an invertible operator:

1. For each variable `xᵢ` and each invertible operator `OP` (XOR or ADD):
   - Compute the **residual**: `r(vars) = f(vars) OP⁻¹ xᵢ`
   - If `r` depends on fewer variables or has lower cost than `f`, recursively simplify `r`
   - Compose: `f = xᵢ OP r_simplified`

2. The decomposition is accepted only if the result is strictly simpler than the input (measured by expression cost).

3. Full-width verification ensures the decomposition is correct.

This is effective for expressions where one variable can be "factored out" of a complex combination, reducing the remaining expression to something the linear or polynomial pipelines can handle.

## Strategy 3: Template Decomposition

The **TemplateDecomposer** performs bounded search over small compositions of known atoms. It builds a pool of candidate subexpressions:

- Constants, single variables, and negations
- Unary operations: NOT, NEG
- Pairwise operations: AND, OR, XOR, ADD, SUB, MUL applied to pairs from the pool

Then it searches for compositions that match the target signature:

- **Layer 1**: `target = G(A, B)` — can the target be expressed as a single operation over two atoms from the pool?
- **Layer 2**: `target = G_out(A, G_in(B, C))` — can it be expressed as a nested pair of operations over three atoms?

All candidates are verified by comparing their signature vectors against the target's signature. This is a brute-force approach but with bounded search depth, making it tractable for expressions that resist algebraic decomposition.

## Strategy 4: Bitwise Decomposition

The **BitwiseDecomposer** attempts to reconstruct the expression using only bitwise operations. It enumerates combinations of Boolean gates (AND, OR, XOR, NOT) applied to the input variables and checks whether any combination matches the target signature.

This is useful when the mixed product structure is actually a disguised Boolean function — for example, `(x & y) * (x | y)` over Boolean inputs behaves identically to `x & y`, and the bitwise decomposer can detect this.

## Strategy 5: Product Identity Recovery

The **ProductIdentityRecoverer** recognizes specific MBA product identities and collapses them. The primary identity is:

```
x * y = (x & y) * (x | y) + (x & ~y) * (~x & y)
```

This identity holds for all integer values, not just Boolean inputs. It is often used in obfuscation to replace a simple product with a complex sum of bitwise-product terms.

The recoverer checks whether the expression matches one of 8 role assignments (permutations of the factors) and, if so, collapses it back to the simple product form. It validates the match using Boolean-cube constraints on the factor signatures.

## Recursive Simplification

When any strategy succeeds in decomposing a mixed expression, the resulting subexpressions are fed back through CoBRA's classifier for further simplification. This recursive approach allows CoBRA to handle layered obfuscation where multiple techniques are stacked.

## Known Limitations

Some mixed expressions remain unsupported:

- **Complex shift interactions**: Deeply nested combinations of shifts and bitwise products where no rewrite rule applies
- **High-degree mixed products**: Products of three or more bitwise subexpressions that don't match known identities
- **Incompatible decompositions**: Cases where hybrid extraction produces a residual that is more complex than the original

The QSynth EA dataset contains the most challenging examples — CoBRA simplifies 400 of 500 expressions, with the remaining 100 falling outside current rewrite coverage.
