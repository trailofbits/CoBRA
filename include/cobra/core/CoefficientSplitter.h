#pragma once

#include "cobra/core/Evaluator.h"
#include <cstdint>
#include <vector>

namespace cobra {

    /// Compute x^{-1} mod 2^{w-1} for odd x (half the modulus).
    /// Precondition: x is odd, w >= 2.
    /// Uses Hensel lifting with ceil(log2(w)) iterations.
    uint64_t ModInverseOddHalf(uint64_t x, uint32_t w);

    struct SplitResult
    {
        std::vector< uint64_t > and_coeffs;
        std::vector< uint64_t > mul_coeffs;
    };

    /// Deterministic coefficient splitting.
    ///
    /// Given CoB coefficients (which conflate AND and MUL on {0,1}),
    /// evaluate the original expression at structured non-binary points
    /// to recover the AND vs MUL contribution for each mask.
    ///
    /// When singleton_at_2 is provided (one entry per variable),
    /// singleton masks (popcount=1) are not split. Instead, the
    /// recovered singleton evaluation S_i(2) is used directly in the
    /// correction model for cross-term masks.  Singleton masks in the
    /// output are zeroed (their contributions live in the external
    /// singleton model).
    ///
    /// Preconditions:
    ///   - bitwidth >= 2
    ///   - cob.size() == 2^num_vars
    ///   - eval returns results mod 2^bitwidth
    ///   - singleton_at_2 is empty or has size == num_vars
    SplitResult SplitCoefficients(
        const std::vector< uint64_t > &cob, const Evaluator &eval, uint32_t num_vars,
        uint32_t bitwidth, const std::vector< uint64_t > &singleton_at_2 = {}
    );

} // namespace cobra
