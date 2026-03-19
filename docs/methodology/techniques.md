# Technique Index

Alphabetical reference of all techniques used in CoBRA. Each entry includes a brief description and links to the pipeline documentation where the technique is used in context.

---

### Absorption (ANF Cleanup)

Removes redundant terms from ANF output. If term A is a subset of term B (e.g., `x` and `x & y`), the superset term is absorbed. Based on the Boolean identity `x XOR (x & y) = x & ~y`.

**Used in:** [Linear Pipeline](linear-pipeline.md#stage-5-anf-cleanup)

---

### Algebraic Normal Form (ANF)

Representation of a Boolean function as a multilinear polynomial over GF(2), using only XOR and AND. Every Boolean function has a unique ANF. CoBRA computes it via the Möbius transform when the CoB result is verbose.

**Used in:** [Linear Pipeline](linear-pipeline.md#stage-4-anf-transform-fallback)

**Reference:** Barbier et al., [On the computation of the Möbius transform](https://www.sciencedirect.com/science/article/pii/S0304397519307674) (TCS 2019)

---

### Auxiliary Variable Elimination

Pre-processing step that detects variables which cancel out of an expression. If toggling a variable's input bit never changes the output (the signature is invariant), that variable is removed, reducing the problem dimension.

**Used in:** [Architecture Overview](README.md#auxiliary-variable-elimination)

---

### Bit Partitioning

Decomposes constant masks into minimal groups of bit positions where all masks have uniform behavior. Within each partition, constants reduce to 0 or 1, enabling standard linear simplification.

**Used in:** [Semilinear Pipeline](semilinear-pipeline.md#stage-2-bit-partitioning)

**Reference:** Skees, [Deobfuscation of Semi-Linear Mixed Boolean-Arithmetic Expressions](https://arxiv.org/abs/2406.10016) (MSiMBA, 2024)

---

### Bitwise Decomposition

Reconstructs an expression using only Boolean gates (AND, OR, XOR, NOT) by enumerating gate combinations and matching against the target signature vector.

**Used in:** [Mixed Pipeline](mixed-pipeline.md#strategy-4-bitwise-decomposition)

---

### Change of Basis (CoB) Butterfly Transform

In-place recurrence that converts a signature vector into AND-product basis coefficients. For each variable, subtracts the "without" entry from the "with" entry, analogous to an FFT butterfly. Recovers the weight of each AND-combination of variables.

**Used in:** [Linear Pipeline](linear-pipeline.md#stage-3-change-of-basis-cob-butterfly-interpolation)

**Reference:** Reichenwallner & Meerwald-Stadler, [Efficient Deobfuscation of Linear Mixed Boolean-Arithmetic Expressions](https://arxiv.org/abs/2209.06335) (SiMBA, 2022)

---

### Coefficient Splitting

Separates AND and MUL contributions in CoB coefficients by evaluating the expression at non-Boolean points (e.g., input values of 2). At these points, `x & y` and `x * y` produce different results, revealing which portion of each coefficient comes from bitwise vs. arithmetic operations.

**Used in:** [Polynomial Pipeline](polynomial-pipeline.md#stage-2-coefficient-splitting)

**Reference:** Reichenwallner & Meerwald-Stadler, [Simplification of General Mixed Boolean-Arithmetic Expressions](https://arxiv.org/abs/2305.06763) (GAMBA, 2023)

---

### Common-Cube Factoring

ANF cleanup pass that extracts shared AND-factors from multiple terms. For example, `(x & y) XOR (x & z)` becomes `x & (y XOR z)`.

**Used in:** [Linear Pipeline](linear-pipeline.md#stage-5-anf-cleanup)

---

### Falling-Factorial Interpolation

Recovers polynomial coefficients using the falling factorial basis `t·(t-1)·(t-2)·...`. The forward differences of a polynomial's values at consecutive integers directly give the falling-factorial coefficients. These are then converted to the standard monomial basis via Stirling numbers.

**Used in:** [Polynomial Pipeline](polynomial-pipeline.md#singleton-power-recovery)

**Reference:** Newton, *Principia Mathematica* (1687) — [Wikipedia: Newton polynomial](https://en.wikipedia.org/wiki/Newton_polynomial)

---

### Finite Differences (Forward)

Repeated differencing of a polynomial's values at consecutive integer inputs. The *d*-th forward difference of a degree-*d* polynomial is constant and equals `d!` times the leading coefficient. This is the discrete analog of repeated differentiation.

**Used in:** [Polynomial Pipeline](polynomial-pipeline.md#how-it-works)

---

### Hensel Lifting

Iterative algorithm for computing modular inverses of odd numbers modulo 2^w. Starting from the trivial inverse mod 2, each iteration doubles the precision using a Newton-like recurrence. Used in coefficient splitting to divide by factorials in modular arithmetic.

**Used in:** [Polynomial Pipeline](polynomial-pipeline.md#stage-2-coefficient-splitting)

**Reference:** Dumas, [On Newton-Raphson iteration for multiplicative inverses modulo prime powers](https://arxiv.org/abs/1209.6626) (2012)

---

### Hybrid Decomposition

Variable-extraction technique for mixed expressions. For each variable and invertible operator (XOR, ADD), computes the residual after "removing" that variable. If the residual is simpler, accepts the decomposition `f = x OP residual`.

**Used in:** [Mixed Pipeline](mixed-pipeline.md#strategy-2-hybrid-decomposition)

---

### Mixed Product Rewriting

Algebraic lowering of bitwise operators within products. The primary rewrite is XOR lowering: `x ^ y → x + y - 2*(x & y)`, which converts XOR to pure arithmetic, enabling the polynomial pipeline to handle the result.

**Used in:** [Mixed Pipeline](mixed-pipeline.md#strategy-1-mixed-product-rewriting)

---

### Möbius Transform

Classical combinatorial transform that converts a Boolean function's truth table to its Algebraic Normal Form (ANF) coefficients over GF(2). CoBRA uses a packed implementation with word-level parallelism.

**Used in:** [Linear Pipeline](linear-pipeline.md#stage-4-anf-transform-fallback)

**Reference:** Barbier et al., [On the computation of the Möbius transform](https://www.sciencedirect.com/science/article/pii/S0304397519307674) (TCS 2019)

---

### OR Recognition

ANF cleanup pass that detects the pattern `x XOR y XOR (x & y)` and rewrites it as `x | y`. Produces more readable output for expressions that are naturally OR-based.

**Used in:** [Linear Pipeline](linear-pipeline.md#stage-5-anf-cleanup)

---

### Pattern Matching

Fast-path recognition of known Boolean functions by comparing signature vectors against precomputed lookup tables. Covers all 16 two-variable and 254 three-variable Boolean functions, including scaled forms `k * f(vars) + c`.

**Used in:** [Linear Pipeline](linear-pipeline.md#stage-2-pattern-matching)

---

### Polynomial Normalization

Canonicalizes polynomial expressions by sorting terms by total degree, then lexicographically by variable indices. Uses null polynomial reduction to ensure distinct coefficient vectors correspond to distinct polynomial functions over fixed-width integers.

**Used in:** [Polynomial Pipeline](polynomial-pipeline.md#polynomial-normalization)

**Reference:** Gamez-Montolio et al., [Efficient Normalized Reduction and Generation of Equivalent Multivariate Binary Polynomials](https://www.ndss-symposium.org/ndss-paper/auto-draft-436/) (BAR/NDSS 2024)

---

### Product Identity Recovery

Recognizes the MBA identity `x * y = (x & y) * (x | y) + (x & ~y) * (~x & y)` and collapses structured products back to simple form. Checks 8 role assignments for factor arrangements and validates via Boolean-cube constraints.

**Used in:** [Mixed Pipeline](mixed-pipeline.md#strategy-5-product-identity-recovery)

---

### Shannon Decomposition

Classical cofactor expansion that splits a Boolean function by fixing one variable: `f(x, ...) = x · f(1, ...) + ~x · f(0, ...)`. Each cofactor depends on one fewer variable. CoBRA uses this recursively to reduce 4-5 variable problems to sizes covered by lookup tables.

**Used in:** [Linear Pipeline](linear-pipeline.md#shannon-decomposition-4-5-variables)

**Reference:** Boole, *Laws of Thought* (1854); Shannon (1949) — [Wikipedia](https://en.wikipedia.org/wiki/Boole%27s_expansion_theorem)

---

### Signature Vector

The foundation of CoBRA's approach. Evaluates an expression on all 2^n Boolean input combinations to produce a vector of 2^n output values. Two expressions are equivalent (mod 2^bitwidth) if and only if their signature vectors match.

**Used in:** [Linear Pipeline](linear-pipeline.md#stage-1-signature-vector), [Architecture Overview](README.md#architecture-overview)

**Reference:** Reichenwallner & Meerwald-Stadler, [Efficient Deobfuscation of Linear Mixed Boolean-Arithmetic Expressions](https://arxiv.org/abs/2209.06335) (SiMBA, 2022)

---

### Singleton Power Recovery

Detects `x^k` terms via finite differences. Evaluates a univariate slice at consecutive integers, computes forward differences to find the degree and leading coefficient, then uses Hensel lifting for modular inverse computation.

**Used in:** [Polynomial Pipeline](polynomial-pipeline.md#singleton-power-recovery)

---

### Template Decomposition

Bounded brute-force search over small compositions of known atoms. Builds a pool of candidate subexpressions (constants, variables, unary/pairwise ops) and searches for 1- or 2-layer compositions that match the target signature.

**Used in:** [Mixed Pipeline](mixed-pipeline.md#strategy-3-template-decomposition)

---

### XOR Lowering

Algebraic identity `x ^ y → x + y - 2*(x & y)` that converts XOR to pure arithmetic. Enables the polynomial pipeline to analyze expressions containing XOR within product subtrees.

**Used in:** [Mixed Pipeline](mixed-pipeline.md#strategy-1-mixed-product-rewriting)
