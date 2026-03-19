#pragma once

#include "cobra/core/ExponentTuple.h"
#include <cstdint>
#include <unordered_map>

namespace cobra {

    using Coeff = uint64_t;

    using CoeffMap = std::unordered_map< ExponentTuple, Coeff, ExponentTupleHash >;

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
                return false;
            }
            for (const auto &[tuple, c] : coeffs) {
                if (c == 0) {
                    return false;
                }
                uint32_t p   = tuple.packed;
                uint8_t twos = 0;
                for (uint8_t i = 0; i < num_vars; ++i) {
                    const uint8_t d = p % 3;
                    if (d > 2) {
                        return false;
                    }
                    if (d == 2) {
                        ++twos;
                    }
                    p /= 3;
                }
                if (twos >= bitwidth) {
                    return false;
                }
                const uint32_t bound_bits = bitwidth - twos;
                if (bound_bits < 64) {
                    if (c >= (1ULL << bound_bits)) {
                        return false;
                    }
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
