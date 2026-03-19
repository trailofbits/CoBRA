#pragma once

#include <cstdint>
#include <vector>

namespace cobra {

    // Recover coefficients of a + b*x + c*y + d*(x&y) + ... directly from a
    // signature vector via in-place butterfly interpolation.
    // coeffs[i] = coefficient of the AND-product of variables whose bits are set in i.
    // All arithmetic is modulo 2^bitwidth.
    std::vector< uint64_t > InterpolateCoefficients(
        std::vector< uint64_t > sig, uint32_t num_vars, uint32_t bitwidth
    ); // NOLINT(readability-identifier-naming)

} // namespace cobra
