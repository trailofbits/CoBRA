#include "cobra/core/SingletonPowerRecovery.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/CoefficientSplitter.h"
#include "cobra/core/MathUtils.h"
#include "cobra/core/Result.h"
#include "cobra/core/SingletonPowerPoly.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cobra {

    Result< SingletonPowerResult >
    RecoverSingletonPowers(const Evaluator &eval, uint32_t num_vars, uint32_t bitwidth) {
        if (bitwidth < 2 || bitwidth > 64) {
            return Err< SingletonPowerResult >(
                CobraError::kNoReduction,
                "recover_singleton_powers: bitwidth must be 2..64, got "
                    + std::to_string(bitwidth)
            );
        }

        const uint32_t max_degree = DegreeCap(bitwidth);
        const uint64_t mask       = Bitmask(bitwidth);

        // Precompute per-degree metadata (once per call).
        struct DegreeInfo
        {
            uint32_t twos;           // number of factors of 2 in k!
            uint64_t odd_inverse;    // odd_part(k!)^{-1} mod 2^precision
            uint32_t precision_bits; // bitwidth - twos
        };

        std::vector< DegreeInfo > info(max_degree);
        for (uint32_t k = 1; k < max_degree; ++k) {
            const uint32_t v    = TwosInFactorial(k);
            const uint32_t prec = bitwidth - v;
            const uint64_t odd  = OddPartFactorial(k, prec);
            const uint64_t inv  = ModInverseOdd(odd, prec);
            info[k]             = { .twos = v, .odd_inverse = inv, .precision_bits = prec };
        }

        SingletonPowerResult result;
        result.num_vars = num_vars;
        result.bitwidth = bitwidth;
        result.per_var.resize(num_vars, { .bitwidth = bitwidth, .terms = {} });

        std::vector< uint64_t > point(num_vars, 0);
        std::vector< uint64_t > table(max_degree);

        for (uint32_t var = 0; var < num_vars; ++var) {
            // Step 1: Evaluate univariate slice g_var(t) for t = 0..max_degree-1.
            for (uint32_t t = 0; t < max_degree; ++t) {
                point[var] = t;
                table[t]   = eval(point) & mask;
            }
            point[var] = 0; // restore for next variable

            // Step 2: Forward differences in-place.
            for (uint32_t k = 1; k < max_degree; ++k) {
                // Safe: k >= 1, so when t decrements from k to k-1,
                // the unsigned comparison (k-1) >= k is false.
                for (uint32_t t = max_degree - 1; t >= k; --t) {
                    table[t] = (table[t] - table[t - 1]) & mask;
                }
            }

            // Step 3: Recover factorial-basis coefficients.
            std::vector< UnivariateTerm > terms;
            for (uint32_t k = 1; k < max_degree; ++k) {
                const uint64_t dk = table[k];
                const uint32_t v  = info[k].twos;

                // Divisibility check: low v bits must be zero.
                if (v > 0 && (dk & ((1ULL << v) - 1)) != 0) {
                    return Err< SingletonPowerResult >(
                        CobraError::kNoReduction,
                        "singleton-power recovery failed: variable " + std::to_string(var)
                            + ", degree " + std::to_string(k) + " — divisibility check failed"
                    );
                }

                const uint64_t shifted = dk >> v;
                const uint64_t factorial_coeff =
                    (shifted * info[k].odd_inverse) & Bitmask(info[k].precision_bits);

                if (factorial_coeff != 0) {
                    terms.push_back({ .degree = static_cast< uint16_t >(k),
                                      .coeff  = factorial_coeff });
                }
            }

            result.per_var[var].terms = std::move(terms);
        }

        return Ok(std::move(result));
    }

} // namespace cobra
