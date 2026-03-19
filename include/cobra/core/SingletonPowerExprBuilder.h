#pragma once

#include "cobra/core/Expr.h"
#include "cobra/core/SingletonPowerPoly.h"
#include <memory>
#include <vector>

namespace cobra {

    // Convert factorial-basis coefficients to monomial-basis coefficients
    // for a single variable. Uses signed Stirling numbers of the first kind.
    // Returns dense array a[0..d_max] where d_max is the highest input degree.
    // All arithmetic mod 2^bitwidth.
    std::vector< uint64_t >
    FactorialToMonomial(const std::vector< UnivariateTerm > &terms, uint32_t bitwidth);

    // Build an Expr tree from the singleton-power recovery result.
    // Returns nullptr if the result has no nonzero terms.
    std::unique_ptr< Expr > BuildSingletonPowerExpr(const SingletonPowerResult &powers);

} // namespace cobra
