#pragma once

#include "cobra/core/MonomialKey.h"
#include <cstdint>
#include <unordered_map>

namespace cobra {

    using Coeff = uint64_t;

    using CoeffMap = std::unordered_map< MonomialKey, Coeff >;

    struct PolyIR
    {
        uint8_t num_vars{};
        uint32_t bitwidth{}; // 2 <= bitwidth <= 64
        CoeffMap terms;
    };

    struct NormalizedPoly
    {
        uint8_t num_vars{};
        uint32_t bitwidth{}; // 2 <= bitwidth <= 64
        CoeffMap coeffs;

        bool IsValid() const {
            if (bitwidth < 2 || bitwidth > 64) {
                return false; // NOLINT(readability-simplify-boolean-expr)
            }
            for (const auto &[tuple, c] : coeffs) {
                if (c == 0) { return false; }
                uint32_t q = tuple.V2FactorialWeight(num_vars);
                if (q >= bitwidth) { return false; }
                const uint32_t bound_bits = bitwidth - q;
                if (bound_bits < 64) {
                    if (c >= (1ULL << bound_bits)) { return false; }
                }
            }
            return true;
        }

        bool operator==(const NormalizedPoly &o) const {
            return num_vars == o.num_vars && bitwidth == o.bitwidth && coeffs == o.coeffs;
        }

        bool operator!=(const NormalizedPoly &o) const { return !(*this == o); }
    };

} // namespace cobra
