# Lifting

Lifting reduces problem complexity by replacing complex subexpressions with virtual variables, solving the simpler outer skeleton, and substituting back. This transforms an expression that might have too many variables or too much structural complexity for direct techniques into a smaller problem that fits within their reach.


## Concept

Consider an expression like `((x + y) & z) ^ ((u * v) & w)`, where arithmetic subexpressions sit under bitwise parents. Direct solving sees mixed bitwise/arithmetic structure. But if we introduce virtual variables `v0 = x + y` and `v1 = u * v`, the outer skeleton becomes `(v0 & z) ^ (v1 & w)` -- potentially a simpler function that signature, semilinear, or decomposition passes can handle more effectively.

After solving the outer skeleton, we substitute back the original arithmetic subexpressions.

The key requirement: the outer solution must be verified against the **original evaluator** (with the real subexpressions, not the virtual variables) at full width before acceptance.


## Arithmetic Atom Lifting

**Pass:** `kLiftArithmeticAtoms`
**Source:** [LiftingPasses.cpp](../../lib/core/LiftingPasses.cpp), [LiftingPasses.h](../../lib/core/LiftingPasses.h)

Identifies pure arithmetic subexpressions (products, sums, negations -- no bitwise ops) that appear as children of bitwise operators, and replaces each with a virtual variable. This targets expressions where arithmetic computations are nested inside bitwise structure.

The pass walks the AST tracking whether the parent node is bitwise. When it finds a non-trivial arithmetic subtree under a bitwise parent, that subtree becomes a lift candidate. Candidates are deduplicated by structural hash and rendered string equality so that identical subtrees share one virtual variable.

Each lifted subtree becomes a `LiftedBinding` with kind `kArithmeticAtom`. The binding records:
- The virtual variable index in the outer skeleton
- The original subtree (the arithmetic expression)
- A structural hash for deduplication
- The original variable support (which real variables participate)

The outer variable list is the original variables plus virtual variables named `v0`, `v1`, etc. The total variable count (original + virtual) must not exceed `max_vars`, or the pass reports `kResourceLimit` and blocks.


## Repeated Subexpression Lifting

**Pass:** `kLiftRepeatedSubexpressions`
**Source:** [LiftingPasses.cpp](../../lib/core/LiftingPasses.cpp), [LiftingPasses.h](../../lib/core/LiftingPasses.h)

Identifies non-leaf subtrees that appear multiple times in the AST and replaces each unique repeated subtree with a single virtual variable. This is distinct from arithmetic atom lifting -- it targets structural repetition regardless of operator type.

For example, if `(a ^ b)` appears in three places within a complex expression, it gets one virtual variable `r0`, reducing the effective variable count and potentially making the outer skeleton tractable.

The pass traverses all non-leaf subtrees, grouping by structural hash and rendered string equality. It keeps candidates with at least 2 occurrences and at least 4 AST nodes (the `kMinRepeatSize` threshold filters out trivially small repeats). Candidates are sorted by occurrence count, then subtree size, then pre-order position, and selected greedily with a non-overlapping constraint: if a candidate is an ancestor or descendant of an already-selected candidate, it is skipped.

Each lifted subtree becomes a `LiftedBinding` with kind `kRepeatedSubexpression`. Virtual variables are named `r0`, `r1`, etc.


## Skeleton Solving

**Pass:** `kPrepareLiftedOuterSolve`
**Source:** [OrchestratorPasses.cpp](../../lib/core/OrchestratorPasses.cpp)

Once subexpressions are lifted out, the outer skeleton is a function of the virtual variables plus any original variables that were not lifted. `RunPrepareLiftedOuterSolve` consumes the `kLiftedSkeleton` work item and emits a new `AstPayload` child work item containing the outer expression. This child is solved by the standard pass pipeline -- signature techniques, semilinear handling, decomposition, or further lifting if the skeleton is still complex.

The pass builds a `LiftedSubstituteCont` continuation on a child competition group that owns the outer solve. This continuation carries:
- The bindings (virtual variable to original subtree mappings)
- The outer variable names and the original variable count
- The original evaluator and variable names for full-width verification after substitution
- The source signature of the un-lifted expression

An outer evaluator is constructed from the skeleton expression so that downstream passes (signature, decomposition) can verify against the reduced problem independently. The continuation's original evaluator is separate -- it is used only during substitution verification.


## Substitution and Verification

When the skeleton solve completes and the competition group resolves, `ResolveLiftedSubstitute` in [SignaturePasses.cpp](../../lib/core/SignaturePasses.cpp) drives the substitution:

1. **Remap to full outer space.** The winning expression may use fewer variables than the outer skeleton (due to auxiliary variable elimination). Variable indices are remapped from the reduced space back to the full outer variable list.

2. **Substitute bindings.** Each virtual variable reference (any variable index >= `original_var_count`) is replaced with its original subtree from the corresponding `LiftedBinding`. The substitution recurses through the expression tree.

3. **Full-width verification.** The substituted expression is verified against the original evaluator using `FullWidthCheckEval`. This check is required because the skeleton solve treats virtual variables as independent, but in reality they may share underlying variables (e.g., `v0 = x & y` and `v1 = x | y` both depend on `x` and `y`). The full-width check catches cases where the skeleton solution is correct in the virtual-variable space but incorrect when the dependencies are restored.

If verification passes, the result is emitted as a `CandidatePayload` with `needs_original_space_verification = false` (since the original-space check was already performed).


## When Lifting Helps

Lifting is most effective when:
- The expression contains arithmetic subexpressions nested inside bitwise operators that resist direct decomposition
- Multiple complex subexpressions repeat throughout the AST, inflating the effective problem size
- The effective variable count after lifting drops enough that signature-based techniques (which scale as 2^n in the number of variables) become feasible

It is less effective when:
- The lifted skeleton is still too complex (too many virtual variables, or the outer function is itself a mixed expression)
- The subexpressions interact in ways that the independent-variable assumption misses, causing full-width verification to reject solutions


## Scheduling

Both lifting passes run on `kFoldedAst` items at priorities 7 and 8, after the core extraction passes (product, polynomial, template) but before operand simplification and XOR lowering. This positioning means lifting is attempted only when direct extraction techniques have already had a chance to solve the expression outright.

The scheduler routes `kLiftedSkeleton` items directly to `kPrepareLiftedOuterSolve` with no alternatives -- there is exactly one pass that consumes this state kind.

Both lifting passes use `ItemDisposition::kRetainCurrent`, meaning the original work item remains live alongside the lifted skeleton. This is worklist interleaving rather than a dedicated competition group between the original AST and the lifted branch. The lifted branch creates its own child group only so the outer solve can be recombined through `LiftedSubstituteCont`.


## Relationship to Other Techniques

Lifting complements decomposition: where decomposition extracts a polynomial core and solves the residual, lifting extracts subexpressions and solves the outer skeleton. They attack the same class of mixed expressions from different angles, and the worklist can keep both approaches alive on the same input.

Lifting can also compose with itself. If the skeleton produced by arithmetic atom lifting still contains repeated subexpressions, those can be lifted in a subsequent pass when the skeleton re-enters the pipeline. The `original_ctx` field on `LiftedSkeletonPayload` ensures that nested lifting resolves back into the correct parent-local variable space rather than the global original space.
