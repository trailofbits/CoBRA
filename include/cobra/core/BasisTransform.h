#pragma once

#include "cobra/core/PolyIR.h"

namespace cobra {

    // Monomial -> Factorial basis (F_3^{otimes n}).
    //
    // Contract:
    // - Input and output use exponent tuples in {0,1,2}^n.
    // - All accumulation is modulo 2^bitwidth.
    // - Zero coefficients are stripped from the returned map.
    // - 2 <= bitwidth <= 64.
    CoeffMap ToFactorialBasis(const CoeffMap &terms, uint8_t num_vars, uint32_t bitwidth);

    // Factorial -> Monomial basis (C_3^{otimes n}).
    //
    // Same contract as to_factorial_basis.
    CoeffMap ToMonomialBasis(const CoeffMap &coeffs, uint8_t num_vars, uint32_t bitwidth);

} // namespace cobra
