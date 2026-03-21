# Semilinear Pipeline

The semilinear pipeline handles MBA expressions where bitwise atoms involve constant masks — operations like `x & 0xFF` or `y | 0xF0` that operate on specific bit ranges of a variable rather than the full word.

**Example:** `(x ^ 0x10) + 2*(x & 0x10)` simplifies to `x + 16`

## When This Pipeline Runs

The classifier routes an expression here when it detects constant operands inside bitwise operations. Standard linear MBA techniques operate in 1-bit space, where all bitwise and arithmetic operations are interchangeable — but this assumption breaks when constants appear inside bitwise operators. A constant like `0xFF` selects specific bit positions, creating behavior that varies across the bits of the word.

A **linear shortcut** check runs first: if all semi-linear signature rows are identical across bit positions, the expression is actually linear in disguise and is routed to the [linear pipeline](linear-pipeline.md) instead.

**Reference:** Skees, [Deobfuscation of Semi-Linear Mixed Boolean-Arithmetic Expressions](https://arxiv.org/abs/2406.10016) (MSiMBA, 2024) — identifies this limitation of linear simplifiers and proposes the bit-partitioned approach.

## Pipeline Stages

```
Expression
    |
[SemilinearNormalizer] ── decompose into weighted atoms with truth tables
    |                      (XOR/OR/NOT-AND constant lowering to AND-basis)
    |
[SimplifyStructure] ── truth-table deduplication of atoms
    |
[SelfCheckSemilinear] ── verify round-trip equivalence (structural)
    |
[RecoverStructure] ── XOR recovery, mask elimination from complement pairs
    |
[RefineTerms] ── dead-bit mask reduction, same-coefficient merge
    |
[CoalesceTerms] ── per-bit coefficient analysis, partition merge
    |
[FullWidthCheck] ── post-rewrite equivalence probe (random full-width inputs)
    |
[CompactAtomTable] ── prune dead intermediate atoms
    |
[BitPartitioner] ── group bit positions by semantic profile
    |
[MaskedAtomReconstructor] ── reassemble with OR-rewrite for disjoint masks
    |
Simplified Expression
```

## Stage 1: Semilinear Normalization

The normalizer decomposes the expression into a sum of **weighted atoms** — bitwise functions applied to subsets of the variables, possibly with constant masks. It extracts:

- The **support** of each atom: which variables participate
- The **truth table** of each atom over its support variables
- The **coefficient** (weight) of each atom in the sum

For example, in `3*(x & 0xFF) + 5*(y | 0xF0)`, the atoms are `x & 0xFF` (coefficient 3) and `y | 0xF0` (coefficient 5).

### Constant Lowering

Before registering atoms, the normalizer lowers XOR, OR, and NOT-AND patterns with constant operands into AND-basis form:

| Pattern | Decomposition | Identity |
|---------|---------------|----------|
| `a ^ c` | `a + c - 2*(a & c)` | XOR as arithmetic over AND |
| `a \| c` | `a + c - (a & c)` | OR as arithmetic over AND |
| `(~a) & c` | `c - (a & c)` | NOT-AND as complement |

This creates additional `(a & c)` terms that participate in coefficient deduplication and cancellation. For instance, `(x ^ 0x10) + 2*(x & 0x10)` lowers to `x + 0x10 - 2*(x & 0x10) + 2*(x & 0x10)`, where the AND terms cancel, leaving `x + 16`.

## Stage 2: Structure Simplification

**SimplifyStructure** deduplicates atoms that have identical truth tables and merges their coefficients. Two atoms with the same support and truth table are semantically identical even if their ASTs differ structurally.

## Stage 3: Structural Self-Check

**SelfCheckSemilinear** re-normalizes the reconstructed expression and compares the resulting atom truth tables against the original IR. This catches normalization bugs before the rewrite stages mutate the IR. The self-check runs before structure recovery because XOR recovery changes the atom decomposition.

## Stage 4: Structure Recovery

**RecoverStructure** identifies algebraic relationships between atoms that share the same basis expression (the variable-containing part of `basis & mask`). It decomposes each atom via `DecomposeAtom` into `(basis, mask)` pairs and groups atoms by basis.

Within each basis group, it looks for **complement mask pairs** — two atoms whose masks OR to the full modular mask and AND to zero:

- **XOR recovery:** If `m*(c & x) + (-m)*(~c & x)`, the pair is rewritten to `-m*(c ^ x) + m*c`. This recovers XOR operations that were hidden by the semilinear decomposition.

- **Mask elimination:** If `a*(c & x) + b*(~c & x)` with `a != -b`, the pair is rewritten to `(a-b)*(c & x) + b*x`. This strips the mask from one term, replacing it with a bare variable reference.

## Stage 5: Term Refinement

**RefineTerms** applies local optimizations to individual terms:

- **ReduceMask:** Strips dead bits from masks — bits where the coefficient's contribution is zero (e.g., bit 63 when the coefficient is even in 64-bit arithmetic).
- **Same-coefficient merge:** Two atoms with the same coefficient and compatible masks are merged into a single term with a wider mask.

## Stage 6: Term Coalescing

**CoalesceTerms** performs per-bit coefficient analysis on same-basis groups. For each variable-basis group, it computes the effective coefficient at each bit position:

```
eff[bit] = sum(coeff_k * ((mask_k >> bit) & 1))  mod 2^bw
```

It then groups bits by effective coefficient to form new partitions. If the new partition count is smaller than the current term count, the group is replaced with the more compact representation.

For example, `3*(0x55 & x) + 3*(0xAA & x)` has the same effective coefficient (3) at every bit, so it coalesces to `3*x`.

## Stage 7: Post-Rewrite Verification

After the rewrite stages, a **FullWidthCheck** evaluates the original and rewritten expressions at 16 random full-width inputs to catch rewrite bugs. This works without Z3 and provides high confidence at negligible cost.

## Stage 8: Atom Table Compaction

**CompactAtomTable** removes atoms from the atom table that are no longer referenced by any term. The rewrite stages (RecoverStructure, RefineTerms, CoalesceTerms) create new atoms via `CreateAtom` but don't delete the old ones they replace. Compaction runs before `ComputePartitions` to avoid paying partitioning cost for dead intermediate atoms.

## Stage 9: Bit Partitioning

The **BitPartitioner** evaluates each atom at each bit position to build a per-bit semantic profile. Bit positions with identical profiles across all atoms are grouped into the same partition class.

For instance, with masks `0xFF` and `0xFF00` at 16-bit width:

- Partition 1: bits 0-7 (where `0xFF` is active)
- Partition 2: bits 8-15 (where `0xFF00` is active)

## Stage 10: Reconstruction

The **MaskedAtomReconstructor** reassembles the IR into an expression tree. For terms with coefficient 1 whose active bit masks are disjoint, it uses OR assembly (`a | b`) instead of addition (`a + b`) — a semantically equivalent but more natural form.

## Shift Handling

Constant left shifts (`x << 3`) are desugared to multiplication (`8 * x`) before simplification. Right shifts on bitwise subtrees (`(x & y) >> 2`) are routed through the semilinear pipeline, since the shift effectively creates a constant mask that selects the upper bits. A shift amount of zero is folded away (identity).

## Verification Contract

Semilinear results follow a strict verification contract:

- **Z3 builds:** Results are always verified via Z3 equivalence proof.
- **Non-Z3 + `--strict`:** An error is raised (no unverified output).
- **Non-Z3 (default):** A warning is printed that the result is unverified.

The linear shortcut also honors this contract — expressions classified as semilinear but routed to the linear pipeline still receive semilinear-level verification.
