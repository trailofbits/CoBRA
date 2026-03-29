# Signature-Based Techniques

These passes operate on work items in the `kSignatureState` and `kSignatureCoeffState` states within CoBRA's worklist-based orchestrator. The Boolean signature vector is the common starting point for this family. Some passes solve directly from that Boolean data, while others add coefficient models or recurse into grouped child solves.

---

## Signature Vector

**Pass:** `kBuildSignatureState` (transitions `kFoldedAst` to `kSignatureState`)
**Source:** [SignatureVector.cpp](../../lib/core/SignatureVector.cpp), [OrchestratorPasses.cpp](../../lib/core/OrchestratorPasses.cpp)

For an expression over *n* variables, evaluate on all 2^n Boolean input combinations. The expression `(x & y) + (x | y)` with inputs `(0,0), (0,1), (1,0), (1,1)` produces `[0, 1, 1, 2]`.

Any linear MBA expression over *n* variables is fully determined by its values on {0,1}^n. Two expressions are equivalent (mod 2^bitwidth) if and only if their signature vectors match.

The pass also performs auxiliary variable elimination: variables that never affect the output are dropped, reducing the problem dimension before downstream techniques run.

**Reference:** [SiMBA](https://arxiv.org/abs/2209.06335) (Reichenwallner & Meerwald-Stadler, 2022)

---

## Pattern Matching

**Pass:** `kSignaturePatternMatch` (operates on `kSignatureState`)
**Source:** [PatternMatcher.cpp](../../lib/core/PatternMatcher.cpp)

Fast-path recognition of known Boolean functions by comparing signature vectors against precomputed tables.

### Lookup tables (1-3 variables)

- **1-variable**: Direct linear model `a*x + c`.
- **2-variable**: All 16 Boolean functions (AND, OR, XOR, NAND, etc.) keyed by packed truth table bits.
- **3-variable**: All 254 non-constant Boolean functions, generated via BFS over {AND, OR, XOR, NOT} from variables.

If the signature matches, the simplified expression is returned immediately.

### Shannon decomposition (4-5 variables)

For 4-5 variable expressions, enumeration is impractical. CoBRA uses Shannon decomposition:

```
f(x1, ..., xn) = x_k * f(..., x_k=1, ...) XOR ~x_k * (f(..., x_k=0, ...) XOR f(..., x_k=1, ...))
```

Implemented as a mux: `f = f0 XOR (x_k AND (f0 XOR f1))`, where f0 and f1 are the cofactors with the split variable set to 0 and 1 respectively. Each cofactor has one fewer variable and is matched by the lower-dimension tables. Special cases (constant cofactor, complementary cofactors, independent of split variable) are handled directly without the general mux.

**Reference:** Boole, *Laws of Thought* (1854); Shannon (1949) -- [Wikipedia](https://en.wikipedia.org/wiki/Boole%27s_expansion_theorem)

### Scaled patterns

For non-Boolean signatures, the matcher tests whether the signature has the form `c + k * f(vars)` where f is a recognized Boolean function with entries in {0, 1}. It factors out the constant offset and scale, matches the Boolean quotient, and reconstructs `k * f(vars) + c`.

---

## CoB Butterfly Interpolation

**Pass:** `kSignatureCobCandidate` (operates on `kSignatureCoeffState`)
**Source:** [CoeffInterpolator.cpp](../../lib/core/CoeffInterpolator.cpp), [CoBExprBuilder.cpp](../../lib/core/CoBExprBuilder.cpp)

The Change of Basis transform re-expresses the signature vector in the AND-product basis: `{1, x, y, x&y, z, x&z, y&z, x&y&z, ...}`. Each basis entry corresponds to an AND-product of the variables whose bits are set in its index.

### Butterfly recurrence

The transform is an in-place butterfly (structurally similar to the FFT butterfly). For each variable, subtract the "without" entry from the "with" entry. All arithmetic is modulo 2^bitwidth:

```
for var in 0..n:
    for each index i where bit var is set:
        coeffs[i] = coeffs[i] - coeffs[i with bit var cleared]
```

The result is a vector of 2^n coefficients. Zero coefficients mean those AND-product terms are absent.

### Expression reconstruction

[CoBExprBuilder.cpp](../../lib/core/CoBExprBuilder.cpp) reconstructs an expression from the nonzero coefficients. Before emitting the raw sum-of-AND-products, it applies greedy pairwise rewriting to recover OR and XOR operations:

- **OR match**: `c*A + c*B - c*(A|B)` collapses to `c*(A | B)` (the union term has coefficient `-c`).
- **XOR match**: `c*A + c*B - 2c*(A|B)` collapses to `c*(A XOR B)` (the union term has coefficient `-2c`).
- **NOT recognition**: A constant term of -1 plus a single term with coefficient -1 collapses to `~(term)`.

Terms are processed in popcount order (simpler AND-products first) so that lower-order structures are recognized before higher-order ones consume them.

**Reference:** [SiMBA](https://arxiv.org/abs/2209.06335) (Reichenwallner & Meerwald-Stadler, 2022)

---

## Coefficient Model Preparation

**Pass:** `kPrepareCoeffModel` (transitions `kSignatureState` to `kSignatureCoeffState`)
**Source:** [SignaturePasses.cpp](../../lib/core/SignaturePasses.cpp)

Runs the CoB butterfly interpolation on the signature vector to produce the coefficient vector. This intermediate state augments the signature with coefficient data needed by the CoB candidate builder and singleton power recovery passes.

---

## ANF Transform

**Pass:** `kSignatureAnf` (operates on `kSignatureState`)
**Source:** [AnfTransform.cpp](../../lib/core/AnfTransform.cpp), [AnfCleanup.cpp](../../lib/core/AnfCleanup.cpp)

When CoB produces a correct but verbose result, CoBRA tries Algebraic Normal Form -- the representation of a Boolean function as a polynomial over GF(2), using only XOR and AND:

```
f = 1 XOR x XOR y XOR (x AND y)
```

### Packed Mobius transform

Computed via the Mobius transform on the low bit of the signature (the GF(2) projection). CoBRA uses a packed implementation with word-level parallelism: intra-word stages handle the first 6 dimensions using precomputed bit masks (alternating bit patterns at each stride), while inter-word stages XOR whole 64-bit words for dimensions 7 and above.

**Reference:** Barbier, Cheballah, Le Bars -- [On the computation of the Mobius transform](https://www.sciencedirect.com/science/article/pii/S0304397519307674) (TCS 2019)

### ANF cleanup

Four cost-driven rules compete to improve the raw XOR-of-AND-products:

1. **OR recognition**: Detects complete OR families. If all 2^k - 1 nonempty submasks of a variable set appear as monomials, the set rewrites to an OR chain. `x XOR y XOR (x & y)` becomes `x | y`.

2. **Partial OR recognition**: Finds OR families embedded in a larger expression. The OR chain replaces the family monomials, and the remainder is cleaned up recursively.

3. **Common-cube factoring**: Extracts shared AND-factors. `(x & y) XOR (x & z)` becomes `x & (y XOR z)`. Candidates are single variables and variable pairs drawn from monomial intersections. Each candidate is evaluated for cost savings before committing.

4. **Absorption**: For exactly two monomials where one is a subset of the other (M and M&N), rewrites `M XOR (M & N)` as `M & ~N`.

Each rule computes cost before and after rewriting. The lowest-cost result across all applicable rules wins.

---

## Singleton Power Recovery

**Pass:** `kSignatureSingletonPolyRecovery` (operates on `kSignatureCoeffState`)
**Source:** [SingletonPowerRecovery.cpp](../../lib/core/SingletonPowerRecovery.cpp)

Detects univariate polynomial terms `x^k` via the method of finite differences.

### Algorithm

For each variable *i*, evaluate a univariate slice `g(t) = f(0, ..., t, ..., 0)` at consecutive integers `t = 0, 1, ..., d_max`. Apply forward differences repeatedly in-place:

```
for k = 1 to d_max:
    for t = d_max down to k:
        table[t] = table[t] - table[t-1]
```

After *d* passes, `table[d]` is the *d*-th forward difference, which equals `d!` times the degree-*d* falling factorial coefficient.

### Coefficient extraction

The *k*-th forward difference `delta_k` must be divisible by 2^v(k!) where v is the 2-adic valuation (number of trailing zeros in k!). After shifting out the factors of 2, the coefficient is recovered by multiplying by the modular inverse of the odd part of k!, computed via Hensel lifting.

The degree cap is derived from the bitwidth: degrees high enough that k! is divisible by 2^bitwidth produce vanishing contributions and are skipped.

**Reference:** Newton, *Principia Mathematica* (1687) -- [Gregory-Newton interpolation](https://en.wikipedia.org/wiki/Newton_polynomial); Dumas -- [Hensel lifting](https://arxiv.org/abs/1209.6626) (2012)

---

## Multivariate Polynomial Recovery

**Pass:** `kSignatureMultivarPolyRecovery` (operates on `kSignatureState`)
**Source:** [MultivarPolyRecovery.cpp](../../lib/core/MultivarPolyRecovery.cpp)

For expressions with variable-variable products, the AND-product basis conflates `x & y` with `x * y` (they are identical on Boolean inputs). The coefficient splitter resolves this ambiguity.

### Coefficient splitting

[CoefficientSplitter.cpp](../../lib/core/CoefficientSplitter.cpp) evaluates the expression at structured non-Boolean points where all active variables equal 2. At these points, `x & y = 2` but `x * y = 4`, revealing the MUL contribution. The splitter works bottom-up by popcount:

```
eval_diff = f(P_m) - g(P_m)       // actual minus predicted from known AND+MUL terms
mul_coeff = (eval_diff / 2) * (2^{deg-1} - 1)^{-1}   mod 2^{bitwidth-1}
and_coeff = cob_coeff - mul_coeff
```

The modular inverse is computed via Hensel lifting.

**Reference:** [GAMBA](https://arxiv.org/abs/2305.06763) (Reichenwallner & Meerwald-Stadler, 2023)

### Tensor-product interpolation

After splitting, the polynomial part is recovered via falling-factorial interpolation on the {0, 1, ..., d}^k grid (where k is the number of support variables). Forward differences are applied as a tensor product: *d* passes per dimension. Degree escalation iteratively increases the degree from a minimum bound, accepting the first degree that passes full-width verification.

**Reference:** Gamez-Montolio et al. -- [Efficient Normalized Reduction](https://www.ndss-symposium.org/ndss-paper/auto-draft-436/) (BAR/NDSS 2024)

---

## Bitwise Decomposition

**Pass:** `kSignatureBitwiseDecompose` (operates on `kSignatureState`)
**Source:** [BitwiseDecomposer.cpp](../../lib/core/BitwiseDecomposer.cpp)

Shannon-style decomposition applied at the signature level. For each variable *k*, the signature is split into two cofactors (variable=0 and variable=1). The decomposer enumerates multiple gate operations to find structured relationships:

| Gate | Condition | Reconstruction |
|------|-----------|---------------|
| AND | cofactor-0 is all zeros | `x_k & g(rest)` |
| MUL | cofactor-0 is all zeros | `x_k * g(rest)` |
| OR | cofactor-1 = cofactor-0 \| 1 | `x_k \| g(rest)` |
| XOR | cofactor-1 = cofactor-0 ^ 1 | `x_k ^ g(rest)` |
| ADD | constant difference between cofactors | `c*x_k + g(rest)` |

Each candidate emits the residual signature `g` as a child work item in a competition group. The child has one fewer active variable and is solved independently by the full signature technique stack. Results are recombined via the `BitwiseComposeCont` continuation. Candidates are sorted by active variable count so simpler residuals are tried first.

---

## Hybrid Decomposition

**Pass:** `kSignatureHybridDecompose` (operates on `kSignatureState`)
**Source:** [HybridDecomposer.cpp](../../lib/core/HybridDecomposer.cpp)

An extract-and-recurse approach. For each variable *k* and extraction operation (XOR, ADD), the decomposer builds a residual signature by subtracting the variable's contribution:

| Operation | Residual |
|-----------|----------|
| XOR | `r[i] = sig[i] XOR bit_k(i)` |
| ADD | `r[i] = sig[i] - bit_k(i)` |

If the residual differs from the original signature, it is emitted as a child work item. The solver simplifies the residual, and the result is recombined as `x_k XOR r_expr` or `x_k + r_expr` via the `HybridComposeCont` continuation. Candidates are sorted by active variable count.

---

## Output Selection

Within a `kSignatureState` solve, `kBuildSignatureState` creates a local competition group. Direct candidates from pattern matching, ANF, CoB reconstruction, singleton-power recovery, multivariate polynomial recovery, and the winners of bitwise or hybrid child decompositions submit into that group. When the group resolves, its lowest-cost accepted candidate is emitted as a single `kCandidateExpr`.

That group-local winner can still lose at the top level to a semilinear, decomposition, or lifting result that reaches the main loop first. Competition groups coordinate one signature solve at a time; they are not a single global winner-selection mechanism for the whole orchestrator.
