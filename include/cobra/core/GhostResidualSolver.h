#pragma once

#include "cobra/core/Expr.h"
#include "cobra/core/Simplifier.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace cobra {

    struct GhostSolveResult
    {
        std::unique_ptr< Expr > expr;
        std::vector< const char * > primitives_used;
        uint8_t num_terms = 0;
    };

    // Classifies whether a residual is boolean-null:
    // zero on all {0,1}^k inputs AND nonzero at some full-width point.
    // support: full-width live-variable indices in original space.
    // Probes are generated in support-local space and mapped to full
    // space via support[], so late-index variables are correctly tested.
    bool IsBooleanNullResidual(
        const Evaluator &residual_eval, const std::vector< uint32_t > &support,
        uint32_t num_vars, uint32_t bitwidth, const std::vector< uint64_t > &boolean_sig
    );

    // Attempts to solve a boolean-null residual as a constant-coefficient
    // single ghost primitive. Returns expression in original variable space.
    // Requires support.size() <= 6.
    std::optional< GhostSolveResult > SolveGhostResidual(
        const Evaluator &residual_eval, const std::vector< uint32_t > &support,
        uint32_t num_vars, uint32_t bitwidth
    );

    // Attempts to solve a boolean-null residual as q(x) * g(tuple),
    // where q is a constant polynomial and g is a ghost primitive.
    // Uses RecoverWeightedPoly with max_degree=0, grid_degree=2.
    // Enumerates ghost primitives in priority order (mul_sub_and first).
    // Requires support.size() <= 6.
    std::optional< GhostSolveResult > SolveFactoredGhostResidual(
        const Evaluator &residual_eval, const std::vector< uint32_t > &support,
        uint32_t num_vars, uint32_t bitwidth, uint8_t max_degree = 0, uint8_t grid_degree = 2
    );

} // namespace cobra
