#pragma once

#include "cobra/core/Expr.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cobra {

    struct Z3VerifyResult
    {
        bool equivalent{};
        std::string counterexample;
    };

    /// Formally prove equivalence of the affine reconstruction (from CoB
    /// coefficients) and the simplified expression using Z3 bitvector theory.
    /// The coefficients must be the output of cob_transform() on the reduced sig.
    Z3VerifyResult Z3Verify(
        const std::vector< uint64_t > &cob_coeffs, const Expr &simplified,
        const std::vector< std::string > &var_names, uint32_t num_vars, uint32_t bitwidth,
        uint32_t timeout_ms = 500
    );

    /// Compare two Expr trees for equivalence over all w-bit inputs.
    /// No CoB coefficients needed — used by the semilinear pipeline.
    Z3VerifyResult Z3VerifyExprs(
        const Expr &original, const Expr &simplified,
        const std::vector< std::string > &var_names, uint32_t bitwidth,
        uint32_t timeout_ms = 500
    );

} // namespace cobra
