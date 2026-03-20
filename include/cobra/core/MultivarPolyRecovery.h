#pragma once

#include "cobra/core/Expr.h"
#include "cobra/core/PolyIR.h"
#include "cobra/core/Simplifier.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace cobra {

    // Multivariate polynomial recovery via falling-factorial interpolation
    // on the {0, ..., max_degree}^k grid.
    //
    // Recovers a NormalizedPoly when the target function is representable
    // as an ordinary polynomial (using +, -, *) over Z/2^w with
    // per-variable degree <= max_degree.
    //
    // Returns nullopt if:
    //   - preconditions violated (empty support, index out of range, etc.)
    //   - any falling-factorial coefficient fails the divisibility gate
    //     (function is not representable in the monomial polynomial class)
    //
    // Non-support variables are fixed to 0 during evaluation.
    std::optional< NormalizedPoly > RecoverMultivarPoly(
        const Evaluator &eval, const std::vector< uint32_t > &support_vars,
        uint32_t total_num_vars, uint32_t bitwidth, uint8_t max_degree = 2
    );

    struct PolyRecoveryResult
    {
        std::unique_ptr< Expr > expr;
        uint8_t degree_used;
    };

    // Degree-escalating polynomial recovery with evaluation verification.
    // Tries degrees min_degree..max_degree_cap, returning the first result whose
    // built Expr passes FullWidthCheckEval. Returns nullopt if no degree
    // produces a verified polynomial (or if max_degree_cap < min_degree).
    std::optional< PolyRecoveryResult > RecoverAndVerifyPoly(
        const Evaluator &eval, const std::vector< uint32_t > &support_vars,
        uint32_t total_num_vars, uint32_t bitwidth, uint8_t max_degree_cap = 4,
        uint8_t min_degree = 2
    );

} // namespace cobra
