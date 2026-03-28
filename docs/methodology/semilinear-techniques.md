# Semilinear Techniques

These passes handle MBA expressions where bitwise atoms involve constant masks -- operations like `x & 0xFF` or `y | 0xF0` that operate on specific bit ranges rather than the full word. Standard linear techniques work in 1-bit space where bitwise and arithmetic are interchangeable, but this assumption breaks when constants appear inside bitwise operators. A constant like `0xFF` selects specific bit positions, creating behavior that varies across the bits of the word.

**Example:** `(x ^ 0x10) + 2*(x & 0x10)` simplifies to `x + 16`

**Reference:** Skees, [Deobfuscation of Semi-Linear Mixed Boolean-Arithmetic Expressions](https://arxiv.org/abs/2406.10016) (MSiMBA, 2024) -- identifies this limitation of linear simplifiers and proposes the bit-partitioned approach that CoBRA implements.

## Linear Shortcut

**Source:** [SemilinearSignature.cpp](../../lib/core/SemilinearSignature.cpp)

Before full semilinear processing, a shortcut check determines whether the expression is linear in disguise. The check evaluates the expression at `{0, 2^i}` for each variable at each bit position and extracts per-variable coefficients from the bit-0 evaluation. It then verifies that the delta at every other bit position matches `coeff[j] * 2^bit`. If all deltas match, the constant masks don't create bit-dependent behavior and the expression can be routed to the signature-based linear techniques instead, avoiding the overhead of full semilinear normalization.

The check is O(num_vars * bitwidth) evaluations and bails out on the first mismatch.

## Normalization

**Pass:** `kSemilinearNormalize` (transitions `kFoldedAst` to `kSemilinearNormalizedIr`)
**Source:** [SemilinearNormalizer.cpp](../../lib/core/SemilinearNormalizer.cpp)

Decomposes the expression into a sum of **weighted atoms** -- bitwise functions applied to subsets of variables, possibly with constant masks. For each atom, the normalizer extracts:

- The **support**: which variables participate
- The **truth table**: the atom's Boolean function over its support variables
- The **coefficient** (weight) in the sum

For example, in `3*(x & 0xFF) + 5*(y | 0xF0)`, the atoms are `x & 0xFF` (coefficient 3) and `y | 0xF0` (coefficient 5).

The normalizer walks the expression tree recursively via `CollectTerms`, threading a running coefficient through arithmetic nodes. Addition distributes the coefficient to both children. Negation flips the coefficient sign (mod 2^w). Multiplication by a constant folds into the running coefficient. Purely bitwise subtrees with variables are registered as atoms.

After collection, terms with the same atom ID have their coefficients summed (mod 2^w), and zero-coefficient terms are discarded.

### Constant Lowering

Before registering atoms, the normalizer lowers XOR, OR, and NOT-AND patterns with constant operands into AND-basis form:

| Pattern | Decomposition | Identity |
|---------|---------------|----------|
| `a ^ c` | `a + c - 2*(a & c)` | XOR as arithmetic over AND |
| `a \| c` | `a + c - (a & c)` | OR as arithmetic over AND |
| `(~a) & c` | `c - (a & c)` | NOT-AND as complement |

This creates additional `(a & c)` terms that participate in coefficient deduplication and cancellation. For instance, `(x ^ 0x10) + 2*(x & 0x10)` lowers to `x + 0x10 - 2*(x & 0x10) + 2*(x & 0x10)`, where the AND terms cancel, leaving `x + 16`.

The synthesized AND atoms from XOR/OR lowering are passed through `CollectTerms` (not `RegisterAtom`) so that further lowerings -- particularly `(~a) & c` -- fire on the first pass. This keeps the normalize-reconstruct-re-normalize round-trip idempotent.

### Truth-Table Deduplication

Two atoms with the same support and truth table are semantically identical on Boolean inputs, so they share a single atom table entry. However, this deduplication only applies to **pure-variable atoms** (no constants, no shifts). Atoms containing constants get separate entries even if their Boolean truth tables match, because `x & 0xFF` and `x & 0x00FF` would have the same 1-bit truth table but differ at full width.

A structural-hash fast path skips truth-table computation entirely when two atoms have identical ASTs.

## Self-Check

**Pass:** `kSemilinearCheck` (transitions `kSemilinearNormalizedIr` to `kSemilinearCheckedIr`)
**Source:** [SemilinearPasses.cpp](../../lib/core/SemilinearPasses.cpp)

After normalization, `SimplifyStructure` deduplicates atoms with identical truth tables and merges their coefficients. The IR is then reconstructed into an expression and re-normalized. The resulting atom truth tables are compared against the original IR to catch normalization bugs before the rewrite stages mutate the representation.

The self-check runs before structure recovery because XOR recovery changes the atom decomposition, which would break the round-trip comparison.

If the self-check fails, the pass returns `kBlocked` with a diagnostic reason, and the expression falls through to other techniques.

## Atom Flattening

**Part of pass:** `kSemilinearRewrite`
**Source:** [StructureRecovery.cpp](../../lib/core/StructureRecovery.cpp)

`FlattenComplexAtoms` decomposes single-variable atoms with non-trivial structure (anything beyond bare variables or `var & constant`) into a canonical `f(0) + (x & pass_mask) - (x & invert_mask)` form. It evaluates the atom at x=0 and x=all-ones to determine which bits pass through unchanged and which invert, then replaces the atom with masked-AND terms.

This runs first in the rewrite pass. If any atoms were flattened, `CoalesceTerms` runs immediately to merge any newly compatible terms before structure recovery.

Atoms with shifts or multi-variable support are left untouched -- shifts mix bits across positions, breaking the per-bit decomposition.

## Structure Recovery

**Part of pass:** `kSemilinearRewrite`
**Source:** [StructureRecovery.cpp](../../lib/core/StructureRecovery.cpp)

`RecoverStructure` identifies algebraic relationships between atoms sharing the same basis expression (the variable part of `basis & mask`). Each atom is decomposed via `DecomposeAtom` into a `(basis, mask)` pair, and atoms are grouped by basis hash.

Within each basis group, the pass looks for **complement mask pairs** -- two atoms whose masks OR to the full modular mask and AND to zero.

**XOR recovery:** When `m*(c & x) + (-m)*(~c & x)` (coefficients are negated), rewrites to `-m*(c ^ x) + m*c`. This recovers XOR operations that were hidden by the semilinear decomposition. The pass prefers the mask with fewer set bits as the XOR constant to minimize the additive `m*c` term.

**Mask elimination:** When `a*(c & x) + b*(~c & x)` with `a != -b`, rewrites to `(a-b)*(c & x) + b*x`. This strips the mask from one term, replacing it with a bare variable reference.

**Ordering constraint:** `RecoverStructure` must run before `RefineTerms`. `ReduceMask` in RefineTerms strips dead bits from masks (e.g., bit 63 when the coefficient is even in 64-bit), which would break complement-pair detection needed for XOR recovery.

## Term Refinement

**Part of pass:** `kSemilinearRewrite`
**Source:** [TermRefiner.cpp](../../lib/core/TermRefiner.cpp)

`RefineTerms` groups atoms by basis hash and applies a multi-step optimization sequence within each group, following the procedure from Section 5.2 of [MSiMBA](https://arxiv.org/abs/2406.10016):

1. **ReduceMask** -- strips dead bits from each mask. A bit is dead if `coeff * 2^bit = 0 (mod 2^w)`. For example, when the coefficient is even, the MSB contributes nothing modulo 2^w.

2. **Discard zero-effective terms** -- if `CanChangeCoefficientTo(coeff, 0, mask, bitwidth)` is true, the term contributes nothing and is removed.

3. **Same-coefficient merge** -- two atoms with the same coefficient and disjoint masks are merged into a single term with a wider mask: `c*(m1 & x) + c*(m2 & x)` becomes `c*((m1 | m2) & x)`.

4. **Coefficient adjustment merge** -- if two terms have disjoint masks and one coefficient can be changed to match the other without affecting the result (checked by `CanChangeCoefficientTo`), the coefficient is adjusted and the terms are merged.

5. **Normalize to -1** -- where possible, coefficients are changed to -1 (the modular inverse of 1), which often enables further merges in a second merge pass.

6. **Three-term collapse** -- when three terms satisfy `coeff_a + coeff_b = coeff_c` with pairwise-disjoint masks, rewrites from three terms to two by distributing c's mask into a and b: `m1*(c1 & x) + m2*(c2 & x) + m3*(c3 & x)` becomes `m1*((c1|c3) & x) + m2*((c2|c3) & x)`.

### CanChangeCoefficientTo

The key predicate for merge decisions. Returns true if replacing `old_coeff` with `new_coeff` preserves the term's contribution at every bit position:

```
for each bit i:
    old_coeff * (2^i & mask) = new_coeff * (2^i & mask)  (mod 2^w)
```

This is possible when the old and new coefficients differ only in bit positions that the mask doesn't activate.

## Term Coalescing

**Part of pass:** `kSemilinearRewrite`
**Source:** [StructureRecovery.cpp](../../lib/core/StructureRecovery.cpp)

`CoalesceTerms` performs per-bit coefficient analysis on same-basis groups (restricted to single-variable bases). For each group, it computes the effective coefficient at each bit position:

```
eff[bit] = sum(coeff_k * ((mask_k >> bit) & 1))  mod 2^bw
```

Bits with the same effective coefficient are grouped into a single term with a combined mask. If the resulting partition count is smaller than the current term count, the group is replaced.

**Example:** `3*(0x55 & x) + 3*(0xAA & x)` has effective coefficient 3 at every bit, so it coalesces to `3*x`.

This is a global optimization over the group, unlike RefineTerms which works on pairs and triples. The tradeoff is that CoalesceTerms only handles single-variable bases, while RefineTerms works on arbitrary bases.

## Post-Rewrite Verification

**Part of pass:** `kSemilinearRewrite`
**Source:** [SemilinearPasses.cpp](../../lib/core/SemilinearPasses.cpp)

After all rewrite stages, a full-width check evaluates the original and rewritten expressions at 16 random full-width inputs to catch rewrite bugs. This works without Z3 and provides high confidence at negligible cost. If the probe fails, the pass returns `kBlocked` and the expression falls through to other techniques.

## Atom Table Compaction

**Part of pass:** `kSemilinearReconstruct`
**Source:** [SemilinearPasses.cpp](../../lib/core/SemilinearPasses.cpp)

`CompactAtomTable` removes atoms from the atom table that are no longer referenced by any term. The rewrite stages create new atoms via `CreateAtom` but don't delete the old ones they replace. Compaction runs before bit partitioning to avoid paying partitioning cost for dead intermediate atoms.

## Bit Partitioning

**Part of pass:** `kSemilinearReconstruct` (transitions `kSemilinearRewrittenIr` to `kCandidateExpr`)
**Source:** [BitPartitioner.cpp](../../lib/core/BitPartitioner.cpp)

The core insight of [MSiMBA](https://arxiv.org/abs/2406.10016): constants inside bitwise operations create bit-dependent behavior, but only a few distinct behaviors exist across all bit positions. The partitioner evaluates each atom at each bit position to build a per-bit semantic profile -- a 1-bit truth table computed by reducing constants to their value at that bit position.

Bit positions with identical profiles across all atoms are grouped into the same partition class. For atoms with support larger than 5 variables, an opaque sentinel is used instead of the full truth table (the 2^n evaluation would be prohibitive).

**Example** with masks `0xFF` and `0xFF00` at 16-bit width:

- Partition 1: bits 0-7 (where `0xFF` is active, reduces to 1)
- Partition 2: bits 8-15 (where `0xFF00` is active, reduces to 1)

Within each partition, all constants reduce to 0 or 1, so standard linear simplification (signature vector + CoB butterfly) can solve each partition independently via SiMBA re-invocation.

**Reference:** Skees, [Deobfuscation of Semi-Linear Mixed Boolean-Arithmetic Expressions](https://arxiv.org/abs/2406.10016) (MSiMBA, 2024)

## Reconstruction

**Part of pass:** `kSemilinearReconstruct`
**Source:** [MaskedAtomReconstructor.cpp](../../lib/core/MaskedAtomReconstructor.cpp)

`ReconstructMaskedAtoms` reassembles the IR into an expression tree. Each term is reconstructed as `coeff * atom_subtree`, and terms are combined with addition.

For terms with coefficient 1 whose active bit masks are disjoint (determined from the partition profiles), the reconstructor uses OR assembly (`a | b`) instead of addition (`a + b`). The two are semantically equivalent when the operands have no overlapping active bits, but OR is more natural and often matches the original expression structure.

After reconstruction, `SimplifyXorConstant` applies a final pattern match: `k + k*(c ^ x)` rewrites to `(-k)*(~c ^ x)`. This absorbs additive constants generated by XOR recovery back into the XOR term.

## Shift Handling

Constant left shifts (`x << 3`) are desugared to multiplication (`8 * x`) before simplification. Right shifts on bitwise subtrees (`(x & y) >> 2`) route through the semilinear techniques since the shift creates a constant mask selecting the upper bits. The `EvalAtomAtBit` function in the bit partitioner handles shifts by offsetting the source bit position (`bit_pos + shift_amount`), mapping the shifted result back to the correct partition.
