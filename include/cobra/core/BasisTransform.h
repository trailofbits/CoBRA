#pragma once

#include "cobra/core/PolyIR.h"

namespace cobra {

    // Monomial -> Factorial basis via Stirling numbers of the second kind.
    //
    // Contract:
    // - Input and output use exponent tuples of arbitrary degree.
    // - All accumulation is modulo 2^bitwidth.
    // - Zero coefficients are stripped from the returned map.
    // - 2 <= bitwidth <= 64.
    CoeffMap ToFactorialBasis(const CoeffMap &terms, uint8_t num_vars, uint32_t bitwidth);

    // Factorial -> Monomial basis via signed Stirling numbers of the first kind.
    //
    // Same contract as ToFactorialBasis.
    CoeffMap ToMonomialBasis(const CoeffMap &coeffs, uint8_t num_vars, uint32_t bitwidth);

} // namespace cobra
