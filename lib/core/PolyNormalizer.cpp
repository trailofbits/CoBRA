#include "cobra/core/PolyNormalizer.h"
#include "cobra/core/BasisTransform.h"
#include "cobra/core/PolyIR.h"
#include <cstdint>
#include <utility>

namespace cobra {

    NormalizedPoly NormalizePolynomial(const PolyIR &poly) {
        const uint8_t n  = poly.num_vars;
        const uint32_t w = poly.bitwidth;

        // Step 1: Per-variable basis transform (monomial -> factorial)
        CoeffMap current = ToFactorialBasis(poly.terms, n, w);

        // Step 2: Coefficient reduction — mod 2^{w - #twos} per entry
        for (auto it = current.begin(); it != current.end();) {
            uint32_t p   = it->first.packed;
            uint8_t twos = 0;
            for (uint8_t i = 0; i < n; ++i) {
                if (p % 3 == 2) {
                    ++twos;
                }
                p /= 3;
            }
            if (twos >= w) {
                it = current.erase(it);
                continue;
            }
            const uint32_t bound_bits = w - twos;
            // bound_bits >= 64: all uint64_t values valid mod 2^64
            if (bound_bits < 64) {
                it->second &= (1ULL << bound_bits) - 1;
            }
            if (it->second == 0) {
                it = current.erase(it);
            } else {
                ++it;
            }
        }

        NormalizedPoly result;
        result.num_vars = n;
        result.bitwidth = w;
        result.coeffs   = std::move(current);
        return result;
    }

} // namespace cobra
