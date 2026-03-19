# Polynomial Pipeline

The polynomial pipeline handles MBA expressions that contain variable-variable products (`x * y`) or singleton powers (`x^2`, `x^3`). These go beyond the linear pipeline because the AND-product basis conflates `x & y` with `x * y` on Boolean inputs — they produce identical results when inputs are restricted to {0, 1}.

**Example:** `(x^y)*(x&y) + 3*(x|y)` involves both bitwise functions and variable products.

## When This Pipeline Runs

The classifier routes expressions here under two conditions:

- **Multilinear route**: The expression contains products of distinct variables (`x * y`, `a * b * c`) but no singleton powers. Handled via coefficient splitting and multilinear interpolation.
- **PowerRecovery route**: The expression contains singleton powers (`x * x`, `x^3`) or multivariate high-degree terms. Handled via finite differences and falling-factorial interpolation.

## The Core Problem: AND vs. MUL Ambiguity

On Boolean inputs {0, 1}, the operations `x & y` and `x * y` are identical — both return 1 only when both operands are 1. The CoB butterfly transform (used in the linear pipeline) cannot distinguish them. An expression like `2*(x & y) + 3*(x * y)` would appear as a single coefficient of 5 for the `x&y` / `x*y` basis term.

The polynomial pipeline resolves this ambiguity by evaluating the expression at **non-Boolean points** (e.g., inputs of 2) where `x & y` and `x * y` produce different results.

**Reference:** Reichenwallner & Meerwald-Stadler, [Simplification of General Mixed Boolean-Arithmetic Expressions](https://arxiv.org/abs/2305.06763) (GAMBA, 2023)

## Multilinear Recovery

### Stage 1: CoB Coefficients

The pipeline starts with the same CoB butterfly transform as the linear pipeline, producing coefficients for each AND-product basis term. These coefficients are correct but conflate AND and MUL contributions.

### Stage 2: Coefficient Splitting

The **CoefficientSplitter** separates AND from MUL by evaluating the original expression at structured non-binary points. For each basis mask (representing a subset of variables):

1. Set the variables in the mask to 2 and all others to 0
2. Evaluate the original expression at this point
3. Compare with what the AND-product alone would produce at these inputs
4. The difference reveals the MUL contribution

At input 2, `x & y` evaluates to `2 & 2 = 2`, but `x * y` evaluates to `2 * 2 = 4` — the gap reveals the arithmetic (MUL) component.

For higher-order corrections, the splitter uses **Hensel lifting** to compute modular inverses of odd numbers modulo 2^bitwidth. This is needed because the correction factors involve dividing by factorials, which may not be directly invertible in modular arithmetic. Hensel lifting iteratively doubles the precision of the inverse using a Newton-like recurrence.

**Reference:** Dumas, [On Newton-Raphson iteration for multiplicative inverses modulo prime powers](https://arxiv.org/abs/1209.6626) (2012)

### Stage 3: Multivariate Polynomial Recovery

Once AND and MUL contributions are separated, the MUL terms are recovered via **falling-factorial interpolation** on a small grid of evaluation points. For multilinear terms (per-variable degree at most 1), the grid is {0, 1}^n and the recovery is exact from the split coefficients.

For expressions where per-variable degree may reach 2, CoBRA evaluates on the {0, 1, 2}^n grid and recovers the falling-factorial coefficients, then converts to the standard monomial basis.

**Reference:** Gamez-Montolio, Florit, Brain, Howe — [Efficient Normalized Reduction and Generation of Equivalent Multivariate Binary Polynomials](https://www.ndss-symposium.org/ndss-paper/auto-draft-436/) (BAR/NDSS 2024)

## Singleton Power Recovery

When the expression contains powers of a single variable (`x^2`, `x^3`, etc.), CoBRA uses a dedicated recovery path based on the **method of finite differences**.

### How It Works

1. **Univariate slicing**: For each variable, CoBRA evaluates a univariate slice `g(t) = f(0, ..., 0, t, 0, ..., 0)` at consecutive integer points `t = 0, 1, 2, ...` up to the maximum expected degree.

2. **Forward differences**: The sequence of values is differenced repeatedly. A polynomial of degree *d* has its *d*-th forward difference equal to a constant (the leading coefficient times *d*!). This is the discrete analog of how the *d*-th derivative of a degree-*d* polynomial is constant.

3. **Factorial-basis coefficients**: The forward differences directly give the coefficients in the **falling factorial basis**: `f(t) = c₀ + c₁·t + c₂·t·(t-1) + c₃·t·(t-1)·(t-2) + ...`

4. **Basis conversion**: The falling-factorial coefficients are converted to the standard monomial basis (`1, t, t², t³, ...`) using Stirling numbers of the second kind.

5. **Modular divisibility**: Since all arithmetic is modulo 2^bitwidth, recovering the leading coefficient requires dividing by *d*!. When *d*! has factors of 2, Hensel lifting computes the inverse of the odd part, and divisibility of the even part is checked explicitly.

**Reference:** Newton, *Principia Mathematica* (1687) — Gregory-Newton interpolation formula. [Wikipedia: Newton polynomial](https://en.wikipedia.org/wiki/Newton_polynomial)

### Polynomial Normalization

Recovered polynomial terms are canonicalized by `PolyNormalizer` into a standard form: terms are sorted by total degree, then lexicographically by variable indices. This ensures a unique representation and enables direct comparison of polynomial expressions.

**Reference:** Gamez-Montolio et al., [Efficient Normalized Reduction and Generation of Equivalent Multivariate Binary Polynomials](https://openaccess.city.ac.uk/id/eprint/32695/) (BAR/NDSS 2024) — null polynomial reduction ensures that distinct coefficient vectors correspond to distinct polynomial functions.

## Combining Bitwise and Polynomial Parts

After splitting, CoBRA separately simplifies:

- The **bitwise part** (AND coefficients) through the linear pipeline
- The **polynomial part** (MUL coefficients + singleton powers) through polynomial normalization

The final result combines both parts. If a variable appears in both the bitwise and polynomial components, CoBRA's arithmetic lowering pass converts the polynomial fragment to a normalized IR and merges the two representations.
