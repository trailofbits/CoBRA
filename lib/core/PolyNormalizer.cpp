#include "cobra/core/PolyNormalizer.h"
#include "cobra/core/BasisTransform.h"
#include "cobra/core/PolyIR.h"
#include <cstdint>
#include <utility>

namespace cobra {

    NormalizedPoly NormalizePolynomial(const PolyIR &poly) {
        const uint8_t kN  = poly.num_vars;
        const uint32_t kW = poly.bitwidth;

        // Step 1: Per-variable basis transform (monomial -> factorial)
        CoeffMap current = ToFactorialBasis(poly.terms, kN, kW);

        // Step 2: Coefficient reduction — mod 2^{kW - #twos} per entry
        for (auto it = current.begin(); it != current.end();) {
            uint32_t p   = it->first.packed;
            uint8_t twos = 0;
            for (uint8_t i = 0; i < kN; ++i) {
                if (p % 3 == 2) { ++twos; }
                p /= 3;
            }
            if (twos >= kW) {
                it = current.erase(it);
                continue;
            }
            const uint32_t kBoundBits = kW - twos;
            // kBoundBits >= 64: all uint64_t values valid mod 2^64
            if (kBoundBits < 64) { it->second &= (1ULL << kBoundBits) - 1; }
            if (it->second == 0) {
                it = current.erase(it);
            } else {
                ++it;
            }
        }

        NormalizedPoly result;
        result.num_vars = kN;
        result.bitwidth = kW;
        result.coeffs   = std::move(current);
        return result;
    }

} // namespace cobra
