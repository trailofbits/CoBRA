#include "cobra/core/CoefficientSplitter.h"
#include "cobra/core/BitWidth.h"
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace cobra {

    uint64_t ModInverseOddHalf(uint64_t x, uint32_t w) {
        assert(w >= 2);
        assert(x & 1);

        // Target: x^{-1} mod 2^{w-1}.
        // Hensel lifting: inv = x is correct mod 2^3 (since x^2 = 1 mod 8
        // for all odd x). Each iteration doubles correct bits.
        const uint32_t target_bits = w - 1;
        const uint64_t mod_mask = (target_bits >= 64) ? UINT64_MAX : (1ULL << target_bits) - 1;

        uint64_t inv = x & mod_mask;
        // ceil(log2(target_bits)) iterations, starting from 3 correct bits
        for (uint32_t bits = 3; bits < target_bits; bits *= 2) {
            inv = (inv * (2 - (x * inv))) & mod_mask;
        }
        return inv & mod_mask;
    }

    namespace {

        /// Correction factor for MUL terms at a structured evaluation point.
        /// When all active variables equal 2, the MUL-product value is:
        ///   popcount=1 → 4 (= 2^2, the squared value)
        ///   popcount≥2 → 2^popcount
        uint64_t CorrectionFactor(uint32_t popcount, uint32_t bitwidth) {
            const uint32_t deg = (popcount == 1) ? 2 : popcount;
            if (deg >= bitwidth) {
                return 0;
            }
            return 1ULL << deg;
        }

        /// Evaluate the known contribution at structured point P_m.
        /// At P_m (vars in m = 2, rest = 0), only submasks of m contribute.
        /// AND_s(P_m) = 2 for any nonempty s.
        /// MUL_s(P_m) = correction_factor(popcount(s)).
        ///
        /// When singleton_at_2 is non-empty, singleton submasks (popcount=1)
        /// use the recovered polynomial evaluation S_i(2) instead of the
        /// CoB-linear model 2*and_coeffs[s].
        uint64_t EvalKnownContribution(
            const std::vector< uint64_t > &and_coeffs,
            const std::vector< uint64_t > &mul_coeffs, uint64_t active_mask, uint32_t bitwidth,
            const std::vector< uint64_t > &singleton_at_2
        ) {
            const uint64_t mod_mask = Bitmask(bitwidth);
            uint64_t g              = and_coeffs[0] & mod_mask;

            // Enumerate all nonempty submasks of active_mask
            for (uint64_t s = active_mask; s != 0; s = (s - 1) & active_mask) {
                const auto popcount = static_cast< uint32_t >(std::popcount(s));
                if (popcount == 1 && !singleton_at_2.empty()) {
                    const auto bit_idx = static_cast< uint32_t >(std::countr_zero(s));
                    g                  = (g + singleton_at_2[bit_idx]) & mod_mask;
                } else {
                    const uint64_t and_val = 2;
                    const uint64_t mul_val = CorrectionFactor(popcount, bitwidth);
                    g = (g + (and_val * and_coeffs[s]) + (mul_val * mul_coeffs[s])) & mod_mask;
                }
            }
            return g;
        }

    } // anonymous namespace

    SplitResult SplitCoefficients(
        const std::vector< uint64_t > &cob, const Evaluator &eval, uint32_t num_vars,
        uint32_t bitwidth, const std::vector< uint64_t > &singleton_at_2
    ) {
        assert(bitwidth >= 2);
        const size_t len = size_t{ 1 } << num_vars;
        assert(cob.size() == len);
        assert(singleton_at_2.empty() || singleton_at_2.size() == num_vars);

        const uint64_t mod_mask = Bitmask(bitwidth);
        const uint64_t half_mod = Bitmask(bitwidth - 1);

        SplitResult result;
        result.and_coeffs.resize(len);
        result.mul_coeffs.resize(len, 0);
        for (size_t i = 0; i < len; ++i) {
            result.and_coeffs[i] = cob[i];
        }

        // Precompute inverse table: odd_inverses[deg] = (2^{deg-1} - 1)^{-1}
        // mod 2^{bitwidth-1}.  deg ranges over [2, max(num_vars, 2)].
        const uint32_t max_deg = (num_vars < 2) ? 2 : num_vars;
        std::vector< uint64_t > odd_inverses(max_deg + 1, 0);
        for (uint32_t d = 2; d <= max_deg; ++d) {
            const uint64_t u = (1ULL << (d - 1)) - 1;
            odd_inverses[d]  = ModInverseOddHalf(u, bitwidth);
        }

        // Bottom-up by popcount.
        // When singleton_at_2 is provided, skip popcount=1 masks —
        // singleton contributions are modeled externally.
        std::vector< uint64_t > point(num_vars, 0);
        for (uint32_t k = 1; k <= num_vars; ++k) {
            if (k == 1 && !singleton_at_2.empty()) {
                continue;
            }

            for (uint64_t m = 0; m < len; ++m) {
                if (std::cmp_not_equal(std::popcount(m), k)) {
                    continue;
                }
                if (cob[m] == 0) {
                    continue;
                }

                const uint32_t deg = (k < 2) ? 2 : k;

                // Build P_m: vars in m = 2, rest = 0
                for (uint32_t v = 0; v < num_vars; ++v) {
                    point[v] = ((m & (1ULL << v)) != 0u) ? 2 : 0;
                }

                const uint64_t f = eval(point) & mod_mask;
                const uint64_t g = EvalKnownContribution(
                    result.and_coeffs, result.mul_coeffs, m, bitwidth, singleton_at_2
                );

                const uint64_t eval_diff = (f - g) & mod_mask;
                if (eval_diff == 0) {
                    continue;
                }

                assert((eval_diff & 1) == 0);

                const uint64_t mul_coeff = ((eval_diff >> 1) * odd_inverses[deg]) & half_mod;
                result.mul_coeffs[m]     = mul_coeff;
                result.and_coeffs[m]     = (cob[m] - mul_coeff) & mod_mask;
            }
        }

        // Zero singleton masks when externally modeled
        if (!singleton_at_2.empty()) {
            for (uint32_t i = 0; i < num_vars; ++i) {
                result.and_coeffs[1ULL << i] = 0;
                result.mul_coeffs[1ULL << i] = 0;
            }
        }

        return result;
    }

} // namespace cobra
