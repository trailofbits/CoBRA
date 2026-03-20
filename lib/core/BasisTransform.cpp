#include "cobra/core/BasisTransform.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/MathUtils.h"
#include "cobra/core/PolyIR.h"
#include <cstdint>
#include <utility>

namespace cobra {

    namespace {

        enum class Direction { kToFactorial, kToMonomial };

        CoeffMap TransformBasis(
            const CoeffMap &input, uint8_t num_vars, uint32_t bitwidth, Direction dir
        ) {
            const uint64_t kMask = Bitmask(bitwidth);
            CoeffMap current     = input;

            // Infer max degree from input
            uint8_t max_deg = 0;
            for (const auto &[key, _] : current) {
                uint8_t md = key.MaxDegree(num_vars);
                if (md > max_deg) { max_deg = md; }
            }
            if (max_deg <= 1) { return current; } // identity for degree <= 1

            // Precompute Stirling table
            auto stirling = (dir == Direction::kToFactorial)
                ? BuildStirlingSecondKind(max_deg, bitwidth)
                : BuildStirlingFirstKind(max_deg, bitwidth);

            for (uint8_t var = 0; var < num_vars; ++var) {
                CoeffMap next;
                for (const auto &[tuple, c] : current) {
                    const uint8_t e = tuple.ExponentAt(var);
                    if (e <= 1) {
                        next[tuple] = (next[tuple] + c) & kMask;
                    } else {
                        // Redistribute across j = 1..e using Stirling row
                        for (uint8_t j = 1; j <= e; ++j) {
                            uint64_t s_coeff = stirling[e][j];
                            if (s_coeff == 0) { continue; }
                            auto new_tuple = tuple.WithExponent(var, j);
                            next[new_tuple] =
                                (next[new_tuple] + ((c * s_coeff) & kMask)) & kMask;
                        }
                    }
                }
                // Strip zeros
                for (auto it = next.begin(); it != next.end();) {
                    if (it->second == 0) {
                        it = next.erase(it);
                    } else {
                        ++it;
                    }
                }
                current = std::move(next);
            }
            return current;
        }

    } // anonymous namespace

    CoeffMap ToFactorialBasis(const CoeffMap &terms, uint8_t num_vars, uint32_t bitwidth) {
        return TransformBasis(terms, num_vars, bitwidth, Direction::kToFactorial);
    }

    CoeffMap ToMonomialBasis(const CoeffMap &coeffs, uint8_t num_vars, uint32_t bitwidth) {
        return TransformBasis(coeffs, num_vars, bitwidth, Direction::kToMonomial);
    }

} // namespace cobra
