# Technique Index

Alphabetical reference of all techniques used in CoBRA with cross-links to detailed documentation.

---

### Absorption (ANF Cleanup)

Removes redundant terms from ANF output. If term A is a subset of term B (e.g., `x` and `x & y`), the superset term is absorbed. Based on the Boolean identity `x XOR (x & y) = x & ~y`.

**Source:** [AnfCleanup.cpp](../../lib/core/AnfCleanup.cpp)
**Used in:** [Signature Techniques](signature-techniques.md#anf-cleanup)
**Reference:** Barbier et al., [On the computation of the Mobius transform](https://www.sciencedirect.com/science/article/pii/S0304397519307674) (TCS 2019)

---

### Algebraic Normal Form (ANF)

Representation of a Boolean function as a multilinear polynomial over GF(2), using only XOR and AND. Every Boolean function has a unique ANF. CoBRA computes it via the Mobius transform when the CoB result is verbose.

**Source:** [AnfTransform.cpp](../../lib/core/AnfTransform.cpp)
**Used in:** [Signature Techniques](signature-techniques.md#anf-transform)
**Reference:** Barbier et al., [On the computation of the Mobius transform](https://www.sciencedirect.com/science/article/pii/S0304397519307674) (TCS 2019)

---

### Arithmetic Atom Lifting

Replaces pure arithmetic subexpressions that appear under bitwise parents with virtual variables to reduce the problem dimension. The simplified skeleton is solved at lower arity, then virtual variables are substituted back to recover the full expression.

**Source:** [LiftingPasses.cpp](../../lib/core/LiftingPasses.cpp)
**Used in:** [Lifting](lifting.md)

---

### Atom Table Compaction

Removes unreferenced atoms from the semilinear IR atom table after rewrite passes and before bit partitioning. Remaps term atom IDs to maintain referential integrity, avoiding partitioning cost for dead intermediate atoms.

**Source:** [SemilinearPasses.cpp](../../lib/core/SemilinearPasses.cpp)
**Used in:** [Semilinear Techniques](semilinear-techniques.md#atom-table-compaction)

---

### Auxiliary Variable Elimination

Pre-processing step that detects variables which cancel out of an expression. If toggling a variable's input bit never changes the output (the signature is invariant), that variable is removed, reducing the problem dimension.

**Source:** [AuxVarEliminator.cpp](../../lib/core/AuxVarEliminator.cpp)
**Used in:** [Architecture Overview](README.md#auxiliary-variable-elimination)

---

### Bit Partitioning

Groups bit positions by semantic profile (constant mask behavior). Within each partition, constants reduce to 0 or 1, enabling standard linear simplification per partition.

**Source:** [BitPartitioner.cpp](../../lib/core/BitPartitioner.cpp)
**Used in:** [Semilinear Techniques](semilinear-techniques.md#bit-partitioning)
**Reference:** Skees, [Deobfuscation of Semi-Linear Mixed Boolean-Arithmetic Expressions](https://arxiv.org/abs/2406.10016) (MSiMBA, 2024)

---

### Bitwise Decomposition

Shannon-style decomposition at signature level. Splits a Boolean function into cofactors by fixing one variable, reducing the problem to smaller subproblems solvable by pattern matching or CoB.

**Source:** [BitwiseDecomposer.cpp](../../lib/core/BitwiseDecomposer.cpp)
**Used in:** [Signature Techniques](signature-techniques.md#bitwise-decomposition)

---

### Boolean-Null Residual Classification

Identifies residuals that are zero on all {0,1}^n Boolean inputs but nonzero at some full-width point. These "ghost" residuals arise from functions like `x*y - (x&y)` that are invisible to Boolean-domain analysis. Classification uses a deterministic mixed-parity probe bank.

**Source:** [GhostResidualSolver.cpp](../../lib/core/GhostResidualSolver.cpp)
**Used in:** [Decomposition](decomposition.md#residual-classification)

---

### Change of Basis (CoB) Butterfly Transform

In-place recurrence that converts a signature vector into AND-product basis coefficients. For each variable, subtracts the "without" entry from the "with" entry, analogous to an FFT butterfly. Recovers the weight of each AND-combination of variables.

**Source:** [CoeffInterpolator.cpp](../../lib/core/CoeffInterpolator.cpp)
**Used in:** [Signature Techniques](signature-techniques.md#cob-butterfly-interpolation)
**Reference:** Reichenwallner & Meerwald-Stadler, [Efficient Deobfuscation of Linear Mixed Boolean-Arithmetic Expressions](https://arxiv.org/abs/2209.06335) (SiMBA, 2022)

---

### Coefficient Splitting

Separates AND and MUL contributions in CoB coefficients by evaluating the expression at non-Boolean points (e.g., input value 2). At these points, `x & y = 2` but `x * y = 4`, revealing which portion of each coefficient comes from bitwise vs. arithmetic operations.

**Source:** [CoefficientSplitter.cpp](../../lib/core/CoefficientSplitter.cpp)
**Used in:** [Signature Techniques](signature-techniques.md#coefficient-splitting)
**Reference:** Reichenwallner & Meerwald-Stadler, [Simplification of General Mixed Boolean-Arithmetic Expressions](https://arxiv.org/abs/2305.06763) (GAMBA, 2023)

---

### Common-Cube Factoring

ANF cleanup pass that extracts shared AND-factors from multiple terms. For example, `(x & y) XOR (x & z)` becomes `x & (y XOR z)`.

**Source:** [AnfCleanup.cpp](../../lib/core/AnfCleanup.cpp)
**Used in:** [Signature Techniques](signature-techniques.md#anf-cleanup)

---

### Competition Groups

Local coordination mechanism used when a pass forks alternatives or child solves. Within a group, the lowest-cost accepted candidate that beats the baseline becomes the winner, and a continuation or default resolver turns that winner back into a work item. Top-level success is still determined by worklist order once a group resolves.

**Source:** [CompetitionGroup.cpp](../../lib/core/CompetitionGroup.cpp)
**Used in:** [Orchestrator](orchestrator.md#competition-groups)

---

### Constant Lowering (Semilinear)

Algebraic decomposition of XOR, OR, and NOT-AND patterns with constant operands into AND-basis form during semilinear normalization. `a ^ c` becomes `a + c - 2*(a & c)`, `a | c` becomes `a + c - (a & c)`, `(~a) & c` becomes `c - (a & c)`.

**Source:** [SemilinearNormalizer.cpp](../../lib/core/SemilinearNormalizer.cpp)
**Used in:** [Semilinear Techniques](semilinear-techniques.md#constant-lowering)

---

### Decomposition Engine (Extract-Solve)

Decomposes mixed expressions by extracting a polynomial core and solving the residual independently. Three core extractors (product-AST, polynomial, template) produce candidates; the residual is classified and solved by six specialized solvers. All results are gated by full-width verification.

**Source:** [DecompositionEngine.cpp](../../lib/core/DecompositionEngine.cpp)
**Used in:** [Decomposition](decomposition.md#overview)

---

### Deduplication (Fingerprint Cache)

StateFingerprint and PassAttemptCache prevent the worklist from re-running passes on states it has already seen. Fingerprints are computed from expression structure and state kind.

**Source:** [Orchestrator.cpp](../../lib/core/Orchestrator.cpp)
**Used in:** [Orchestrator](orchestrator.md#deduplication)

---

### Degree Escalation

Iteratively increases polynomial degree from a minimum bound up to a configurable cap, accepting the first degree that passes full-width verification. Avoids committing to a fixed degree upfront for expressions where the true polynomial degree is unknown.

**Source:** [MultivarPolyRecovery.cpp](../../lib/core/MultivarPolyRecovery.cpp)
**Used in:** [Signature Techniques](signature-techniques.md#multivariate-polynomial-recovery), [Decomposition](decomposition.md#polynomial-recovery)

---

### Evaluator-Based Polynomial Recovery

Recovers polynomial coefficients from an evaluator function (rather than an explicit expression) by evaluating on the {0, 1, ..., d}^n grid and solving the falling-factorial interpolation system. Used by the decomposition engine for residuals and by WeightedPolyFit for weighted quotient systems.

**Source:** [MultivarPolyRecovery.cpp](../../lib/core/MultivarPolyRecovery.cpp)
**Used in:** [Decomposition](decomposition.md#polynomial-recovery)

---

### Falling-Factorial Interpolation

Recovers polynomial coefficients using the falling factorial basis `t*(t-1)*(t-2)*...`. Forward differences of polynomial values at consecutive integers directly give falling-factorial coefficients. These are converted to the standard monomial basis via Stirling numbers.

**Source:** [SingletonPowerRecovery.cpp](../../lib/core/SingletonPowerRecovery.cpp)
**Used in:** [Signature Techniques](signature-techniques.md#coefficient-extraction)
**Reference:** Newton -- [Gregory-Newton interpolation](https://en.wikipedia.org/wiki/Newton_polynomial)

---

### Finite Differences (Forward)

Repeated differencing of a polynomial's values at consecutive integer inputs. The d-th forward difference of a degree-d polynomial is constant and equals `d!` times the leading coefficient. This is the discrete analog of repeated differentiation.

**Source:** [SingletonPowerRecovery.cpp](../../lib/core/SingletonPowerRecovery.cpp)
**Used in:** [Signature Techniques](signature-techniques.md#algorithm)

---

### Ghost Residual Solving

Solves boolean-null residuals (zero on Boolean inputs, nonzero at full width) by expressing them as a constant or polynomial times a ghost primitive. Two solvers: constant coefficient (2-adic inference on mixed-parity probes) and polynomial quotient (WeightedPolyFit with the ghost function as weight).

**Source:** [GhostResidualSolver.cpp](../../lib/core/GhostResidualSolver.cpp)
**Used in:** [Decomposition](decomposition.md#ghost-residual-solving)

---

### Hensel Lifting

Iterative algorithm for computing modular inverses of odd numbers modulo 2^w. Starting from the trivial inverse mod 2, each iteration doubles the precision using a Newton-like recurrence. Used in coefficient splitting to divide by factorials in modular arithmetic.

**Source:** [CoefficientSplitter.cpp](../../lib/core/CoefficientSplitter.cpp)
**Used in:** [Signature Techniques](signature-techniques.md#coefficient-extraction)
**Reference:** Dumas, [On Newton-Raphson iteration for multiplicative inverses modulo prime powers](https://arxiv.org/abs/1209.6626) (2012)

---

### Hybrid Decomposition

Extract-and-recurse at signature level via structured operation. Identifies a dominant subexpression, removes its contribution from the signature, and recursively simplifies the remainder.

**Source:** [HybridDecomposer.cpp](../../lib/core/HybridDecomposer.cpp)
**Used in:** [Signature Techniques](signature-techniques.md#hybrid-decomposition)

---

### Linear Shortcut Detection

Pre-check that evaluates a semilinear-classified expression at `{0, 2^i}` per bit position. If all semilinear signature rows are identical, the expression is linear in disguise and can stay on the cheaper signature-based path instead of entering full semilinear normalization.

**Source:** [SemilinearSignature.cpp](../../lib/core/SemilinearSignature.cpp)
**Used in:** [Semilinear Techniques](semilinear-techniques.md#linear-shortcut)

---

### Mask Elimination

Rewrites complement mask pairs with different coefficients: `a*(c & x) + b*(~c & x)` becomes `(a-b)*(c & x) + b*x`. Strips the mask from one term, replacing it with a bare variable reference. Applied when the pair does not qualify for XOR recovery.

**Source:** [StructureRecovery.cpp](../../lib/core/StructureRecovery.cpp)
**Used in:** [Semilinear Techniques](semilinear-techniques.md#structure-recovery)

---

### Mixed Product Rewriting (XOR Lowering)

Algebraic lowering of XOR within product subtrees: `x ^ y` becomes `x + y - 2*(x & y)`. Converts XOR to pure arithmetic, enabling the polynomial pipeline to handle the result.

**Source:** [MixedProductRewriter.cpp](../../lib/core/MixedProductRewriter.cpp)
**Used in:** [AST Processing](ast-processing.md#xor-lowering)

---

### Mobius Transform

Classical combinatorial transform that converts a Boolean function's truth table to its Algebraic Normal Form (ANF) coefficients over GF(2). CoBRA uses a packed implementation with word-level parallelism.

**Source:** [AnfTransform.cpp](../../lib/core/AnfTransform.cpp)
**Used in:** [Signature Techniques](signature-techniques.md#packed-mobius-transform)
**Reference:** Barbier et al., [On the computation of the Mobius transform](https://www.sciencedirect.com/science/article/pii/S0304397519307674) (TCS 2019)

---

### NOT-over-Arithmetic Lowering

Normalizes `~(a + b)` to `-(a + b) - 1` before classification. Ensures bitwise NOT over arithmetic subtrees is expressed in arithmetic form, preventing misclassification.

**Source:** [OrchestratorPasses.cpp](../../lib/core/OrchestratorPasses.cpp)
**Used in:** [AST Processing](ast-processing.md#not-over-arithmetic-lowering)

---

### Operand Simplification

Recursively simplifies operands of mixed products. Each operand is independently simplified and full-width verified before replacement. Reduces sub-problem complexity before the main solve.

**Source:** [OrchestratorPasses.cpp](../../lib/core/OrchestratorPasses.cpp)
**Used in:** [AST Processing](ast-processing.md#operand-simplification)

---

### OR Recognition

ANF cleanup pass that detects the pattern `x XOR y XOR (x & y)` and rewrites it as `x | y`. Produces more readable output for expressions that are naturally OR-based.

**Source:** [AnfCleanup.cpp](../../lib/core/AnfCleanup.cpp)
**Used in:** [Signature Techniques](signature-techniques.md#anf-cleanup)

---

### Pattern Matching

Fast-path recognition of known Boolean functions by comparing signature vectors against precomputed lookup tables. Covers all 16 two-variable and 254 three-variable Boolean functions, including scaled forms `k * f(vars) + c`.

**Source:** [PatternMatcher.cpp](../../lib/core/PatternMatcher.cpp)
**Used in:** [Signature Techniques](signature-techniques.md#pattern-matching)

---

### Polynomial Normalization

Canonicalizes polynomial expressions by sorting terms by total degree, then lexicographically by variable indices. Uses null polynomial reduction to ensure distinct coefficient vectors correspond to distinct polynomial functions over fixed-width integers.

**Source:** [PolyNormalizer.cpp](../../lib/core/PolyNormalizer.cpp)
**Used in:** [Signature Techniques](signature-techniques.md#multivariate-polynomial-recovery)
**Reference:** Gamez-Montolio et al., [Efficient Normalized Reduction and Generation of Equivalent Multivariate Binary Polynomials](https://www.ndss-symposium.org/ndss-paper/auto-draft-436/) (BAR/NDSS 2024)

---

### Product Identity Recovery

Recognizes the MBA identity `x * y = (x & y) * (x | y) + (x & ~y) * (~x & y)` and collapses structured products back to simple form. Checks 8 role assignments for factor arrangements and validates via Boolean-cube constraints.

**Source:** [OrchestratorPasses.cpp](../../lib/core/OrchestratorPasses.cpp)
**Used in:** [AST Processing](ast-processing.md#product-identity-collapse)

---

### Repeated Subexpression Lifting

Replaces common subtrees with virtual variables. Reduces expression size and arity for the inner solver, then substitutes back after simplification.

**Source:** [LiftingPasses.cpp](../../lib/core/LiftingPasses.cpp)
**Used in:** [Lifting](lifting.md)

---

### Shannon Decomposition

Classical cofactor expansion: `f(x, ...) = x * f(1, ...) + ~x * f(0, ...)`. Each cofactor depends on one fewer variable. CoBRA uses this recursively to reduce 4-5 variable problems to sizes covered by lookup tables.

**Source:** [PatternMatcher.cpp](../../lib/core/PatternMatcher.cpp)
**Used in:** [Signature Techniques](signature-techniques.md#shannon-decomposition-4-5-variables)
**Reference:** Boole, *Laws of Thought* (1854); Shannon (1949) -- [Wikipedia](https://en.wikipedia.org/wiki/Boole%27s_expansion_theorem)

---

### Signature Vector

Evaluates an expression on all 2^n Boolean input combinations to produce a vector of 2^n output values. Two expressions are equivalent (mod 2^bitwidth) if and only if their signature vectors match. This is the foundation of CoBRA's approach.

**Source:** [SignatureVector.cpp](../../lib/core/SignatureVector.cpp)
**Used in:** [Signature Techniques](signature-techniques.md#signature-vector)
**Reference:** Reichenwallner & Meerwald-Stadler, [Efficient Deobfuscation of Linear Mixed Boolean-Arithmetic Expressions](https://arxiv.org/abs/2209.06335) (SiMBA, 2022)

---

### Singleton Power Recovery

Detects `x^k` terms via finite differences on univariate slices. Evaluates at consecutive integers, computes forward differences to find the degree and leading coefficient, then uses Hensel lifting for modular inverse computation.

**Source:** [SingletonPowerRecovery.cpp](../../lib/core/SingletonPowerRecovery.cpp)
**Used in:** [Signature Techniques](signature-techniques.md#singleton-power-recovery)

---

### Structure Recovery (XOR Recovery)

Identifies complement mask pairs within same-basis atom groups where coefficients are negated: `m*(c & x) + (-m)*(~c & x)`. Rewrites to `-m*(c ^ x) + m*c`, recovering XOR operations hidden by semilinear decomposition.

**Source:** [StructureRecovery.cpp](../../lib/core/StructureRecovery.cpp)
**Used in:** [Semilinear Techniques](semilinear-techniques.md#structure-recovery)

---

### Template Decomposition

Bounded search over small compositions of known atoms. Builds a pool of candidate subexpressions (constants, variables, unary/pairwise ops) and searches for 1- or 2-layer compositions that match the target signature.

**Source:** [TemplateDecomposer.cpp](../../lib/core/TemplateDecomposer.cpp)
**Used in:** [Decomposition](decomposition.md#template-core-extraction)

---

### Term Coalescing

Per-bit coefficient analysis on same-basis atom groups. Computes the effective coefficient at each bit position, then groups bits by effective coefficient to form new partitions. For example, `3*(0x55 & x) + 3*(0xAA & x)` coalesces to `3*x`.

**Source:** [StructureRecovery.cpp](../../lib/core/StructureRecovery.cpp)
**Used in:** [Semilinear Techniques](semilinear-techniques.md#term-coalescing)

---

### Term Refinement

Local optimizations on individual semilinear terms. ReduceMask strips dead bits from masks (bits where the coefficient's contribution is zero). Same-coefficient merge combines two atoms with the same coefficient and compatible masks into a single term.

**Source:** [TermRefiner.cpp](../../lib/core/TermRefiner.cpp)
**Used in:** [Semilinear Techniques](semilinear-techniques.md#term-refinement)

---

### WeightedPolyFit

2-adic weighted Gaussian elimination for `target = q * weight`. Handles precision loss from dividing by weight values with high 2-adic valuation (many trailing zero bits) by performing forward elimination at reduced precision and back-substitution on the pivot system.

**Source:** [WeightedPolyFit.cpp](../../lib/core/WeightedPolyFit.cpp)
**Used in:** [Decomposition](decomposition.md#factored-ghost-solving)
