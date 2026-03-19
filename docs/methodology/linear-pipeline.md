# Linear Pipeline

The linear pipeline handles the most common class of MBA expressions: weighted sums of bitwise atoms. These are expressions built from `+`, `-`, and constant multiplication over bitwise subexpressions (`&`, `|`, `^`, `~`).

**Example:** `(x & y) + (x | y)` simplifies to `x + y`

## When This Pipeline Runs

The classifier routes an expression here when it contains no variable-variable products (`x * y`), no singleton powers (`x^2`), and no products of bitwise subexpressions. The expression is purely a linear combination of bitwise functions of the input variables.

## Pipeline Stages

```
Expression
    |
[Signature Vector] ── evaluate on all {0,1}^n inputs
    |
[Pattern Matcher] ── fast-path: recognize common functions (2-5 vars)
    |  (match found? → done)
    |
[CoB Butterfly Interpolation] ── recover AND-product basis coefficients
    |
[CoBExprBuilder] ── reconstruct expression from coefficients
    |  (clean result? → done)
    |
[ANF Transform] ── Möbius transform to Algebraic Normal Form
    |
[ANF Cleanup] ── absorption, factoring, OR recognition
    |
Simplified Expression
```

## Stage 1: Signature Vector

The signature vector is the foundation of CoBRA's approach. For an expression over *n* variables, CoBRA evaluates the expression on all 2^n Boolean input combinations — every assignment of 0 or 1 to each variable.

For example, with variables `x` and `y`, the inputs are `(0,0)`, `(0,1)`, `(1,0)`, `(1,1)`. The expression `(x & y) + (x | y)` produces the signature `[0, 1, 1, 2]`.

This works because any linear MBA expression over *n* variables is fully determined by its values on {0, 1}^n. Two expressions are equivalent if and only if their signature vectors are identical (modulo 2^bitwidth). This insight — that evaluation at Boolean points suffices — is the key idea behind SiMBA's approach.

**Reference:** Reichenwallner & Meerwald-Stadler, [Efficient Deobfuscation of Linear Mixed Boolean-Arithmetic Expressions](https://arxiv.org/abs/2209.06335) (SiMBA, 2022)

## Stage 2: Pattern Matching

Before running heavier algebra, CoBRA attempts to recognize the signature vector directly. It maintains lookup tables of known Boolean functions:

- **2-variable functions**: All 16 Boolean functions of two variables (AND, OR, XOR, NAND, etc.)
- **3-variable functions**: All 254 non-constant Boolean functions of three variables, generated via BFS over {AND, OR, XOR, NOT}
- **Scaled patterns**: Expressions of the form `k * f(vars) + c`, where `f` is a recognized Boolean function, `k` is a constant multiplier, and `c` is a constant offset

If the signature matches a known pattern (possibly after factoring out a scale and offset), the simplified expression is returned immediately.

### Shannon Decomposition (4-5 variables)

For expressions with 4 or 5 variables, the lookup tables would be prohibitively large. Instead, CoBRA uses **Shannon decomposition** — a classical technique that splits a Boolean function by fixing one variable:

```
f(x₁, x₂, ..., xₙ) = x₁ · f(1, x₂, ..., xₙ)  +  (1-x₁) · f(0, x₂, ..., xₙ)
                     = x₁ · f₁  +  ~x₁ · f₀
```

Each cofactor `f₁` and `f₀` depends on one fewer variable, reducing the problem to smaller cases covered by the 2-var and 3-var tables. CoBRA selects which variable to split on and recursively simplifies the cofactors.

**Reference:** Boole, *Laws of Thought* (1854); Shannon (1949) — [Wikipedia](https://en.wikipedia.org/wiki/Boole%27s_expansion_theorem)

## Stage 3: Change of Basis (CoB) Butterfly Interpolation

When pattern matching doesn't produce a result, CoBRA performs the **Change of Basis transform**. The idea is to re-express the signature vector in the AND-product basis: `{1, x, y, x&y, z, x&z, y&z, x&y&z, ...}`.

Each entry in the basis corresponds to an AND-product of some subset of the variables (identified by the bits set in its index). The CoB transform recovers the coefficient of each basis term, so the expression becomes:

```
f = c₀ + c₁·x + c₂·y + c₃·(x&y) + c₄·z + c₅·(x&z) + ...
```

The transform uses an in-place **butterfly recurrence** (similar in structure to the FFT butterfly). For each variable, it walks the signature vector and subtracts the "without this variable" entry from the "with this variable" entry. All arithmetic is modulo 2^bitwidth.

After the transform, the coefficients directly give a simplified sum of AND-product terms. Zero coefficients mean those terms are absent from the result.

**Reference:** Reichenwallner & Meerwald-Stadler, [Efficient Deobfuscation of Linear Mixed Boolean-Arithmetic Expressions](https://arxiv.org/abs/2209.06335) (SiMBA, 2022)

## Stage 4: ANF Transform (Fallback)

When the CoB coefficients produce a correct but verbose result (e.g., many nonzero terms), CoBRA falls back to **Algebraic Normal Form (ANF)** — the representation of a Boolean function as a polynomial over GF(2), using only XOR and AND:

```
f = 1 ⊕ x ⊕ y ⊕ (x ∧ y)    (example: XNOR)
```

The ANF is computed via the **Möbius transform**, a classical technique from combinatorics. CoBRA uses a packed implementation that operates on entire machine words at once: intra-word stages handle the first 6 dimensions using bit masks, and inter-word stages process whole 64-bit words for higher dimensions. This makes the transform very fast in practice.

**Reference:** Barbier, Cheballah, Le Bars — [On the computation of the Möbius transform](https://www.sciencedirect.com/science/article/pii/S0304397519307674) (Theoretical Computer Science, 2019)

## Stage 5: ANF Cleanup

Raw ANF output can be redundant. CoBRA applies three cleanup passes:

1. **Absorption**: If a term is a subset of another (e.g., `x` and `x & y`), the superset is absorbed. This follows from the Boolean identity `x ⊕ (x & y) = x & ~y`.

2. **Common-cube factoring**: When multiple terms share a common AND-factor, they can be factored out. For example, `(x & y) ⊕ (x & z)` becomes `x & (y ⊕ z)`.

3. **OR recognition**: The combination `x ⊕ y ⊕ (x & y)` is recognized as `x | y`. CoBRA detects these patterns and rewrites to more readable OR-based forms.

These passes are applied iteratively until no further simplification is possible.

## Output Selection

CoBRA compares the results from pattern matching, CoB reconstruction, and ANF (when attempted) using an expression cost metric that accounts for operator count and depth. The cheapest correct result is returned.
