#pragma once

#include "cobra/core/ExprCost.h"
#include "cobra/core/PassContract.h"
#include "cobra/core/SignatureSimplifier.h"
#include <cstdint>
#include <vector>

namespace cobra {

    // Bounded template-based decomposition.
    // Tries to express the target function as a small composition of
    // atoms drawn from a precomputed pool (constants, variables, unary
    // ops, pairwise ops, and their negations/NOTs).
    //
    // Layer 1: target = G(A, B) for atoms A, B and gate G.
    // Layer 2: target = G_out(A, G_in(B, C)) for atoms A, B, C.
    //
    // All candidates are full-width verified via the evaluator.
    SolverResult< SignaturePayload > TryTemplateDecomposition(
        const SignatureContext &ctx, const Options &opts, uint32_t num_vars,
        const ExprCost *baseline_cost
    );

} // namespace cobra
