# Semilinear Pipeline

The semilinear pipeline handles MBA expressions where bitwise atoms involve constant masks — operations like `x & 0xFF` or `y | 0xF0` that operate on specific bit ranges of a variable rather than the full word.

**Example:** `(x & 0xFF) + (x & 0xFF00)` at 16-bit width simplifies to `x & 0x00FF | x & 0xFF00`

## When This Pipeline Runs

The classifier routes an expression here when it detects constant operands inside bitwise operations. Standard linear MBA techniques operate in 1-bit space, where all bitwise and arithmetic operations are interchangeable — but this assumption breaks when constants appear inside bitwise operators. A constant like `0xFF` selects specific bit positions, creating behavior that varies across the bits of the word.

**Reference:** Skees, [Deobfuscation of Semi-Linear Mixed Boolean-Arithmetic Expressions](https://arxiv.org/abs/2406.10016) (MSiMBA, 2024) — identifies this limitation of linear simplifiers and proposes the bit-partitioned approach.

## Pipeline Stages

```
Expression
    |
[SemilinearNormalizer] ── decompose into weighted atoms with truth tables
    |
[BitPartitioner] ── group atoms by constant mask into partitions
    |
[Per-Partition Simplification] ── run linear pipeline on each partition
    |
[MaskedAtomReconstructor] ── reassemble with bit-extraction masks
    |
Simplified Expression
```

## Stage 1: Semilinear Normalization

CoBRA first decomposes the expression into a sum of **weighted atoms**. Each atom is a bitwise function applied to some subset of the variables, possibly with a constant mask. The normalizer extracts:

- The **support** of each atom: which variables participate
- The **truth table** of each atom over its support variables
- The **coefficient** (weight) of each atom in the sum

For example, in `3*(x & 0xFF) + 5*(y | 0xF0)`, the atoms are `x & 0xFF` (coefficient 3) and `y | 0xF0` (coefficient 5).

## Stage 2: Bit Partitioning

The key insight is that constant masks divide the bit positions into independent groups. Within each group, the mask bits are uniform (all 1s or all 0s for each operand), so the expression behaves like a standard linear MBA within that partition.

The **BitPartitioner** analyzes all constant masks in the expression and identifies the minimal set of bit-position partitions where the mask pattern is consistent. For instance, with a 16-bit width and masks `0xFF` and `0xFF00`:

- Partition 1: bits 0-7 (where `0xFF` is active)
- Partition 2: bits 8-15 (where `0xFF00` is active)

Within each partition, the constants reduce to 0 or 1, and the problem becomes a standard linear MBA over fewer bits.

## Stage 3: Per-Partition Simplification

Each partition is simplified independently using the [linear pipeline](linear-pipeline.md) — signature vector evaluation, CoB interpolation, pattern matching, and ANF as needed. Since the constants have been factored out, the standard techniques apply directly.

## Stage 4: Reconstruction

The **MaskedAtomReconstructor** reassembles the simplified partitions into a single expression, applying the appropriate bit-extraction masks to each partition's result. The output uses explicit mask operations (e.g., `x & 0xFF`) to indicate which bit ranges each simplified term covers.

## Shift Handling

Constant left shifts (`x << 3`) are desugared to multiplication (`8 * x`) before simplification. Right shifts on bitwise subtrees (`(x & y) >> 2`) are routed through the semilinear pipeline, since the shift effectively creates a constant mask that selects the upper bits.
