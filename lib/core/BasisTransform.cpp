#include "cobra/core/BasisTransform.h"
#include "cobra/core/BitWidth.h"
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

            for (uint8_t var = 0; var < num_vars; ++var) {
                CoeffMap next;
                for (const auto &[tuple, c] : current) {
                    const uint8_t exp_i = tuple.ExponentAt(var, num_vars);
                    if (exp_i <= 1) {
                        next[tuple] = (next[tuple] + c) & kMask;
                    } else {
                        // exp_i == 2: split contribution to rows 1 and 2.
                        // F_3 (ToFactorial): +c to row 1, +c to row 2
                        // C_3 (ToMonomial):  -c to row 1, +c to row 2
                        auto t1 = tuple.WithExponent(var, 1, num_vars);
                        if (dir == Direction::kToFactorial) {
                            next[t1] = (next[t1] + c) & kMask;
                        } else {
                            next[t1] = (next[t1] + ModNeg(c, bitwidth)) & kMask;
                        }
                        next[tuple] = (next[tuple] + c) & kMask;
                    }
                }
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
