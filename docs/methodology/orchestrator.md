# Orchestrator Architecture

CoBRA's orchestrator treats simplification as a graph exploration problem. Rather than routing expressions through fixed pipelines, it maintains a worklist of work items that flow through a state machine driven by a pass scheduler. Each expression enters as a single work item, potentially spawns child items as techniques decompose or transform it, and terminates when a verified candidate is found or the search budget is exhausted.

The orchestrator replaces the earlier route-dispatch model, where expressions were classified once and sent down a single pipeline. The new design lets the scheduler interleave multiple technique families on the same input and lets specific passes fork local alternatives or child solves. Those local branches may be resolved by competition groups, but there is no single global "best of all techniques" arena.


## Work Items and State Kinds

A **work item** (`WorkItem`) is the fundamental unit of work. Each item carries:

- A **payload** (`StateData` variant) containing the expression data in its current form
- **State features** (`StateFeatures`): classification, provenance, verification requirements
- **Item metadata** (`ItemMetadata`): signature vector, verification state, rewrite round count, diagnostic cause chains, reason codes
- An **attempted mask** (`uint64_t`) tracking which passes have already been tried on this item
- **Depth** and **rewrite generation** counters for priority ordering and budget enforcement
- An optional **group ID** linking the item to a competition group
- A **history** vector recording the sequence of passes that produced this item

The 11 state kinds represent the forms an expression passes through:

| State Kind | Payload Type | Description |
|------------|-------------|-------------|
| `kFoldedAst` | `AstPayload` | Expression AST after constant folding. Entry point for AST-level passes. Carries optional `AstSolveContext` with variables, evaluator, and input signature. |
| `kSignatureState` | `SignatureStatePayload` | Boolean-point evaluation vector with auxiliary variable elimination applied. Entry point for signature-based techniques. |
| `kSignatureCoeffState` | `SignatureCoeffStatePayload` | Signature augmented with Change-of-Basis coefficients. Consumed by CoB reconstruction and singleton-power recovery. |
| `kCoreCandidate` | `CoreCandidatePayload` | A polynomial core extracted by a decomposition extractor. Carries the core expression, extractor kind, degree, and a `RemainderTargetContext` for computing the residual. |
| `kRemainderState` | `RemainderStatePayload` | The residual after subtracting a core from the target. Carries the prefix expression, remainder evaluator, boolean-null flag, and degree floor. |
| `kSemilinearNormalizedIr` | `NormalizedSemilinearPayload` | Weighted bitwise atoms with truth tables and coefficients (SemilinearIR). |
| `kSemilinearCheckedIr` | `CheckedSemilinearPayload` | Post-self-check IR, verified for round-trip equivalence. |
| `kSemilinearRewrittenIr` | `RewrittenSemilinearPayload` | IR after structure recovery, term refinement, and coalescing. |
| `kLiftedSkeleton` | `LiftedSkeletonPayload` | An expression where complex subexpressions have been replaced with virtual variables. Carries the bindings mapping virtual variables back to original subtrees. |
| `kCandidateExpr` | `CandidatePayload` | A candidate solution with expression, cost, producing pass, and verification status. |
| `kCompetitionResolved` | `CompetitionResolvedPayload` | The resolved winner of a competition group, ready for recombination via the group's continuation. |

Provenance (`Provenance`) tracks an item's origin:
- `kOriginal` -- the input expression as parsed
- `kLowered` -- after NOT-over-arithmetic lowering
- `kRewritten` -- after a structural transform (operand simplify, XOR lowering, etc.)


## Pass Descriptors

Each pass is described by a `PassDescriptor`:

```
PassId id           -- unique identifier
StateKind consumes  -- what state kind this pass operates on
PassTag tag         -- classification: kAnalysis, kRewrite, kSolver, kVerifier
ApplicabilityFn     -- predicate: can this pass run on a given item?
PassFn              -- execution: run the pass, return a PassResult
```

A pass returns a `PassResult` containing a decision and a disposition:

| `PassDecision` | Meaning |
|----------------|---------|
| `kAdvance` | Progress made. Child items produced with new state kinds. |
| `kSolvedCandidate` | A candidate solution was found. |
| `kNoProgress` | Pass ran but could not make progress. |
| `kNotApplicable` | Pass does not apply to this item's current state. |
| `kBlocked` | Pass is blocked (budget exhausted, verification failed, etc.). |

| `ItemDisposition` | Meaning |
|--------------------|---------|
| `kRetainCurrent` | Keep the current item in the worklist for other passes. |
| `kReplaceCurrent` | Replace the current item with the produced children. |
| `kConsumeCurrent` | Remove the current item; children take its place. |


## The 36 Passes

Organized by the state kind they consume.

### kFoldedAst

| Pass | Tag | Purpose |
|------|-----|---------|
| `kLowerNotOverArith` | Analysis | Lower NOT-over-arithmetic patterns. Runs as a prerequisite during seeding, not via the scheduler. |
| `kClassifyAst` | Analysis | Structural analysis. Determines technique eligibility by computing flags and semantic class. Also runs during seeding. |
| `kBuildSignatureState` | Analysis | Compute boolean-point signature vector, eliminate auxiliary variables, transition to `kSignatureState`. |
| `kSemilinearNormalize` | Analysis | Normalize a semilinear AST into weighted bitwise-atom IR, subject to semilinear guards and the linear-shortcut check. |
| `kExtractProductCore` | Solver | Extract variable-product terms as a polynomial core. |
| `kExtractPolyCoreD2` | Solver | Polynomial core extraction at degree 2. |
| `kExtractPolyCoreD3` | Solver | Polynomial core extraction at degree 3. |
| `kExtractPolyCoreD4` | Solver | Polynomial core extraction at degree 4. |
| `kExtractTemplateCore` | Solver | Bounded template matching for core extraction. |
| `kPrepareDirectRemainder` | Analysis | Prepare a remainder without core subtraction (boolean-null path). |
| `kOperandSimplify` | Rewrite | Recursively simplify operands of mixed products. Increments rewrite generation. |
| `kProductIdentityCollapse` | Rewrite | Collapse MBA product identities. Increments rewrite generation. |
| `kXorLowering` | Rewrite | Rewrite XOR to arithmetic for polynomial analysis. Increments rewrite generation. |
| `kLiftArithmeticAtoms` | Rewrite | Replace pure arithmetic subexpressions under bitwise parents with virtual variables, transition to `kLiftedSkeleton`. |
| `kLiftRepeatedSubexpressions` | Rewrite | Replace common subtrees with virtual variables, transition to `kLiftedSkeleton`. |

### kSignatureState

| Pass | Tag | Purpose |
|------|-----|---------|
| `kSignaturePatternMatch` | Solver | Fast-path lookup against known Boolean functions. |
| `kSignatureAnf` | Solver | Algebraic Normal Form via Mobius transform. |
| `kPrepareCoeffModel` | Analysis | Build Change-of-Basis coefficient model, transition to `kSignatureCoeffState`. |
| `kSignatureMultivarPolyRecovery` | Solver | Multivariate polynomial recovery via finite differences. |
| `kSignatureBitwiseDecompose` | Solver | Shannon-style decomposition into cofactor sub-problems. |
| `kSignatureHybridDecompose` | Solver | Hybrid extract-and-recurse decomposition. |

### kSignatureCoeffState

| Pass | Tag | Purpose |
|------|-----|---------|
| `kSignatureCobCandidate` | Solver | Reconstruct expression from CoB coefficients. |
| `kSignatureSingletonPolyRecovery` | Solver | Detect x^k terms via finite differences. |

### kSemilinearNormalizedIr

| Pass | Tag | Purpose |
|------|-----|---------|
| `kSemilinearCheck` | Analysis | Verify round-trip equivalence (self-check). |

### kSemilinearCheckedIr

| Pass | Tag | Purpose |
|------|-----|---------|
| `kSemilinearRewrite` | Rewrite | Structure recovery, term refinement, coalescing. |

### kSemilinearRewrittenIr

| Pass | Tag | Purpose |
|------|-----|---------|
| `kSemilinearReconstruct` | Solver | Bit-partition and reassemble to expression. |

### kCoreCandidate

| Pass | Tag | Purpose |
|------|-----|---------|
| `kPrepareRemainderFromCore` | Analysis | Subtract core from target, compute residual evaluator. |

### kRemainderState

| Pass | Tag | Purpose |
|------|-----|---------|
| `kResidualSupported` | Solver | Run supported techniques on the residual. |
| `kResidualPolyRecovery` | Solver | Polynomial recovery on the residual. |
| `kResidualGhost` | Solver | Ghost primitive matching (boolean-null residuals). |
| `kResidualFactoredGhost` | Solver | Polynomial-times-ghost matching. |
| `kResidualFactoredGhostEscalated` | Solver | Higher-degree factored ghost matching. |
| `kResidualTemplate` | Solver | Template decomposition fallback. |

### kLiftedSkeleton

| Pass | Tag | Purpose |
|------|-----|---------|
| `kPrepareLiftedOuterSolve` | Analysis | Create the outer-solve child item and attach the lifted-substitution continuation. |

### kCandidateExpr

| Pass | Tag | Purpose |
|------|-----|---------|
| `kVerifyCandidate` | Verifier | Spot-check or full-width verification of a candidate. |

### kCompetitionResolved

| Pass | Tag | Purpose |
|------|-----|---------|
| `kResolveCompetition` | Analysis | Pick the winner from a competition group, apply the continuation to recombine. |


## Scheduler

`SelectNextPass` determines which pass to try next for each work item. The selection logic depends on the item's state kind.

**Candidate items** (`kCandidateExpr`) that still require original-space verification are routed to `kVerifyCandidate`, subject to the verification budget (`max_candidates`). Candidates already marked `needs_original_space_verification = false` are accepted immediately by the main loop or submitted into their owning group.

**Signature items** (`kSignatureState`) iterate through a fixed priority table: pattern match, ANF, coeff model preparation, multivariate polynomial recovery, bitwise decomposition, hybrid decomposition.

**Signature coefficient items** (`kSignatureCoeffState`) try CoB candidate reconstruction, then singleton polynomial recovery.

**Semilinear items** follow a linear chain: normalize, check, rewrite, reconstruct. Each state kind maps to exactly one pass.

**Remainder items** (`kRemainderState`) select from one of three solver tables depending on origin:

| Origin | Solver Order |
|--------|-------------|
| Direct boolean-null | Ghost, factored ghost, escalated ghost, polynomial recovery, template |
| Core-derived boolean-null | Polynomial recovery, ghost, factored ghost, template |
| Core-derived standard | Supported pass, polynomial recovery, template |

**Folded AST items** (`kFoldedAst`) follow the most complex selection path:

1. Original provenance with semilinear classification routes to `kSemilinearNormalize`.
2. Rewritten semilinear items with a `solve_ctx` also route to `kSemilinearNormalize`.
3. Original provenance without semilinear classification is terminal; the seed logic ensures a `kLowered` or otherwise non-original copy carries the general solving path.
4. Non-original items without classification or with unknown shape are terminal.
5. Non-exploration candidates (lacking the flags for structural rewrite eligibility) route only to `kBuildSignatureState`.
6. Exploration candidates iterate the full `kFoldedAstPasses` table in priority order, subject to prerequisite masks and rewrite generation limits.

The `kFoldedAstPasses` table defines prerequisite dependencies. For example, `kOperandSimplify` and `kProductIdentityCollapse` require `kExtractProductCore` to have run first (it sets up the structural analysis they depend on).

At every step, two deduplication checks gate pass execution:

1. The item's `attempted_mask` -- a bitmask of passes already tried on this specific item.
2. The `PassAttemptCache` -- a fingerprint-indexed map of (state, pass) pairs already tried across all items. If an item's fingerprint matches a previously attempted combination, the pass is skipped.


## Worklist Priority

The worklist is a priority queue. `Pop` scans for the highest-priority item using `IsBetterPriority`, which compares items in this order:

1. **Band**: Candidate and competition-resolved items (band 0) are popped before all other work (band 1).
2. **Sub-band within band 0**: `kCandidateExpr` before `kCompetitionResolved`.
3. **Depth**: Lower depth preferred (breadth-first bias).
4. **Provenance**: Earlier provenance values preferred (`kOriginal` < `kLowered` < `kRewritten`).
5. **History length**: Fewer passes in the history preferred.

This ordering ensures that candidates ready for immediate acceptance surface quickly, competition groups resolve promptly, and the search explores broadly before diving deep.


## Competition Groups

Competition groups are a local coordination primitive created by passes that fork alternatives or child solves, such as `kBuildSignatureState`, bitwise and hybrid decomposition, residual-supported recombination, lifted substitution, and the join-backed rewrite passes. They are not a single top-level arena for every technique family.

Each group tracks:

- `open_handles` -- count of active branches (incremented by `AcquireHandle`, decremented by `ReleaseHandle`)
- `best` -- the best `CandidateRecord` seen so far (lowest expression cost that beats the baseline)
- `baseline_cost` -- optional cost threshold; candidates must beat this to be accepted
- `continuation` -- a `ContinuationData` variant describing how to recombine the winner with surrounding context
- `technique_failures` -- accumulated `ReasonDetail` from branches that failed

Lifecycle:

1. `CreateGroup` allocates a new group with `open_handles = 1`.
2. Each branch that the orchestrator spawns calls `AcquireHandle` to increment the count.
3. When a branch produces a candidate, it calls `SubmitCandidate`. The candidate is accepted only if it beats the baseline and any current best within that group.
4. When a branch fails, is blocked, or has no more passes to try, `ReleaseHandle` decrements the count.
5. When `open_handles` reaches zero, `ReleaseHandle` returns a `kCompetitionResolved` work item. The `kResolveCompetition` pass picks the winner and applies the continuation.


## Continuations

Several pass families need to suspend work, emit child items, and resume when the children complete. CoBRA uses **continuations** -- data structures attached to competition groups or join states that describe how to recombine results.

| Continuation | Used By | Recombination |
|-------------|---------|---------------|
| `BitwiseComposeCont` | Bitwise decomposition | Compose cofactors via gate (AND/OR/XOR) with addition coefficient |
| `HybridComposeCont` | Hybrid decomposition | Compose via extract operation |
| `RemainderRecombineCont` | Decomposition engine | Add polynomial core + solved residual |
| `OperandRewriteCont` | Operand simplification | Replace simplified operands in product (via `OperandJoinState`) |
| `ProductCollapseCont` | Product identity collapse | Replace collapsed factors in product (via `ProductJoinState`) |
| `LiftedSubstituteCont` | Lifting passes | Substitute virtual variables back to original subexpressions |

`OperandRewriteCont` and `ProductCollapseCont` use a **join state** (`JoinState`) rather than a competition group. A join state tracks two operands (LHS/RHS or X/Y) that must both resolve before the parent can proceed. Each operand has its own competition group; when both resolve, the join state recombines them into the parent expression.


## Deduplication

The orchestrator deduplicates work via two mechanisms.

**StateFingerprint**: A struct containing the item's `StateKind`, a hash of the payload content, the variable names, the bitwidth, and the provenance. Two items with identical fingerprints represent the same simplification sub-problem. The hash computation is payload-specific: AST items hash the expression tree, signature items hash the evaluation vector (folding in group ID and recursion depth), semilinear items use a canonical term-sorted key, and so on.

**PassAttemptCache**: Maps `StateFingerprint` to the list of `PassId` values already attempted. Before running a pass, the scheduler checks whether this (fingerprint, pass) pair has been seen. This prevents:

- Re-running expensive passes on states that were already explored via a different path
- Infinite loops when structural rewrites produce states equivalent to earlier ones
- Redundant work when lifting or decomposition creates sub-problems identical to ones already solved


## Policy

The orchestrator is bounded by `OrchestratorPolicy`:

| Parameter | Default | Purpose |
|-----------|---------|---------|
| `max_expansions` | 1024 | Total pass executions before the orchestrator gives up. |
| `max_rewrite_gen` | 3 | Maximum structural rewrite generations (operand simplify, XOR lowering, etc.) before those passes are disabled for an item. |
| `max_candidates` | 8 | Maximum candidate verifications before the verifier pass is disabled. |

When the budget is exhausted, the orchestrator returns the best unsupported candidate with diagnostic metadata explaining which techniques were attempted and why they failed. The "best" unsupported candidate is selected by `UnsupportedRankBetter`, which prefers candidates (verification-failed items) over non-candidates, then deeper depth, more rewrite generations, more passes attempted, and finally the presence of structural transform terminal evidence.


## Main Loop

The complete flow:

**1. Seed.** The input expression is preprocessed and pushed onto the worklist. For the with-AST path (`SeedWithAst`):

- Run `kLowerNotOverArith` on the input. If it fires, record that the signature must be recomputed.
- Run `kClassifyAst` to determine the expression's semantic class and structural flags.
- Push a `kLowered` copy onto the worklist, even if NOT lowering was a no-op.
- If the expression is semilinear-eligible, also push the original item so the semilinear path can run alongside the lowered/general path.

For the no-AST path (`SeedNoAst`, signature-only input):

- Check for constant expressions via pattern matching (early exit).
- Eliminate auxiliary variables.
- Push a `kSignatureState` item into a new competition group.

**2. Loop.** While the worklist is non-empty and the expansion budget remains:

- Pop the highest-priority item.
- Track it as the best unsupported candidate if it ranks higher than the current best.
- If it is a `kCandidateExpr` that no longer needs original-space verification: return it immediately when ungrouped, or submit it and release the handle when grouped.
- Call `SelectNextPass` to find the next eligible pass.
- If no pass is eligible: release the group handle (if any) and continue.
- Run the pass. Record the attempt in the `PassAttemptCache`.
- On `kAdvance` or `kSolvedCandidate`: push child items (depth + 1, pass appended to history). If the disposition is `kRetainCurrent`, re-queue the current item at depth + 2.
- On `kBlocked` or `kNoProgress`: accumulate diagnostic metadata (failure reasons, cause chains, transform terminal signals). If the disposition is `kRetainCurrent`, re-queue for the next eligible pass.

**3. Termination.** If the loop exits without finding a verified candidate, the orchestrator builds an unsupported result from the best candidate's metadata. It derives the final reason code from the strongest structural transform terminal observed, propagates decomposition cause chains, and falls back to a `kSearchExhausted` reason if nothing else applies.

The result is converted to a `SimplifyOutcome` with telemetry (total expansions, max depth reached, candidates verified, queue high-water mark) and returned to the caller.
