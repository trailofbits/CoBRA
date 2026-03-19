#pragma once

#include "cobra/core/BitWidth.h"
#include "cobra/core/MathUtils.h"
#include <cstdint>
#include <vector>

namespace cobra {

    struct UnivariateTerm
    {
        uint16_t degree; // k: coefficient of x_(k)
        uint64_t coeff;  // h_k, reduced mod 2^{w - TwosInFactorial(k)}
    };

    struct UnivariateNormalizedPoly
    {
        uint32_t bitwidth;
        // Nonzero factorial-basis coefficients for degrees 1..d_w-1.
        // Degree 0 is excluded (constant owned by coefficient splitting).
        //
        // Invariants:
        // - sorted by strictly increasing degree
        // - degrees are unique
        // - 1 <= degree < DegreeCap(bitwidth)
        // - 0 < coeff < 2^{bitwidth - TwosInFactorial(degree)}
        std::vector< UnivariateTerm > terms;
    };

    struct SingletonPowerResult
    {
        uint32_t num_vars{};
        uint32_t bitwidth{};
        // One entry per variable, indexed by variable position.
        // Empty terms vector means that variable has no singleton-power
        // contribution beyond what the bitwise basis captures.
        std::vector< UnivariateNormalizedPoly > per_var;
    };

} // namespace cobra
