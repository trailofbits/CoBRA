#pragma once

#include "cobra/core/PolyIR.h"
#include "cobra/core/Simplifier.h"
#include <cstdint>
#include <optional>
#include <vector>

namespace cobra {

    // Multivariate polynomial recovery via falling-factorial interpolation
    // on the {0,1,2}^k grid.
    //
    // Recovers a NormalizedPoly when the target function is representable
    // as an ordinary polynomial (using +, -, *) over Z/2^w with
    // per-variable degree <= 2.
    //
    // Returns nullopt if:
    //   - preconditions violated (empty support, index out of range, etc.)
    //   - any falling-factorial coefficient fails the divisibility gate
    //     (function is not representable in the monomial polynomial class)
    //
    // Non-support variables are fixed to 0 during evaluation.
    std::optional< NormalizedPoly > RecoverMultivarPoly(
        const Evaluator &eval, const std::vector< uint32_t > &support_vars,
        uint32_t total_num_vars, uint32_t bitwidth
    );

} // namespace cobra
