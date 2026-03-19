#pragma once

#include "cobra/core/PolyIR.h"
#include <cstdint>
#include <vector>

namespace cobra {

    struct NullPolyConfig
    {
        uint32_t num_terms; // upper bound on sampled factorial-basis coordinates
        uint8_t max_degree; // max exponent per variable; asserted == 2
        uint64_t rng_seed;  // deterministic reproducibility
    };

    // Add a random null polynomial (in monomial basis) to seed.
    // Returns a new PolyIR semantically equivalent to seed mod 2^w.
    // Invariant: NormalizePolynomial(result) == NormalizePolynomial(seed).
    PolyIR AddNullPolynomial(const PolyIR &seed, const NullPolyConfig &config);

    // Generate count equivalent variants of seed, each with
    // independent null-polynomial injection.
    std::vector< PolyIR > GenerateEquivalentVariants(
        const PolyIR &seed, uint32_t count, const NullPolyConfig &config
    );

} // namespace cobra
