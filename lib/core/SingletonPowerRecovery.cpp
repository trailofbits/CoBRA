#include "cobra/core/SingletonPowerRecovery.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/CoefficientSplitter.h"
#include "cobra/core/MathUtils.h"
#include "cobra/core/Profile.h"
#include "cobra/core/Result.h"
#include "cobra/core/SingletonPowerPoly.h"
#include "cobra/core/Trace.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cobra {

    Result< SingletonPowerResult >
    RecoverSingletonPowers(const Evaluator &eval, uint32_t num_vars, uint32_t bitwidth) {
        COBRA_ZONE_N("RecoverSingletonPowers");
        COBRA_TRACE(
            "PowerRecovery", "RecoverSingletonPowers: vars={} bitwidth={}", num_vars, bitwidth
        );
        if (bitwidth < 2 || bitwidth > 64) {
            return Err< SingletonPowerResult >(
                CobraError::kNoReduction,
                "recover_singleton_powers: bitwidth must be 2..64, got "
                    + std::to_string(bitwidth)
            );
        }

        const uint32_t kMaxDegree = DegreeCap(bitwidth);
        const uint64_t kMask      = Bitmask(bitwidth);

        // Precompute per-degree metadata (once per call).
        struct DegreeInfo
        {
            uint32_t twos;           // number of factors of 2 in k!
            uint64_t odd_inverse;    // odd_part(k!)^{-1} mod 2^precision
            uint32_t precision_bits; // bitwidth - twos
        };

        std::vector< DegreeInfo > info(kMaxDegree);
        for (uint32_t k = 1; k < kMaxDegree; ++k) {
            const uint32_t kV    = TwosInFactorial(k);
            const uint32_t kPrec = bitwidth - kV;
            const uint64_t kOdd  = OddPartFactorial(k, kPrec);
            const uint64_t kInv  = ModInverseOdd(kOdd, kPrec);
            info[k]              = { .twos = kV, .odd_inverse = kInv, .precision_bits = kPrec };
        }

        SingletonPowerResult result; // NOLINT(misc-const-correctness)
        result.num_vars = num_vars;
        result.bitwidth = bitwidth;
        result.per_var.resize(num_vars, { .bitwidth = bitwidth, .terms = {} });

        std::vector< uint64_t > point(num_vars, 0);
        std::vector< uint64_t > table(kMaxDegree);

        for (uint32_t var = 0; var < num_vars; ++var) {
            // Step 1: Evaluate univariate slice g_var(t) for t = 0..max_degree-1.
            for (uint32_t t = 0; t < kMaxDegree; ++t) {
                point[var] = t;
                table[t]   = eval(point) & kMask;
            }
            point[var] = 0; // restore for next variable

            // Step 2: Forward differences in-place.
            for (uint32_t k = 1; k < kMaxDegree; ++k) {
                // Safe: k >= 1, so when t decrements from k to k-1,
                // the unsigned comparison (k-1) >= k is false.
                for (uint32_t t = kMaxDegree - 1; t >= k; --t) {
                    table[t] = (table[t] - table[t - 1]) & kMask;
                }
            }

            // Step 3: Recover factorial-basis coefficients.
            std::vector< UnivariateTerm > terms;
            for (uint32_t k = 1; k < kMaxDegree; ++k) {
                const uint64_t kDk = table[k];
                const uint32_t kV  = info[k].twos;

                // Divisibility check: low kV bits must be zero.
                if (kV > 0 && (kDk & ((1ULL << kV) - 1)) != 0) {
                    return Err< SingletonPowerResult >(
                        CobraError::kNoReduction,
                        "singleton-power recovery failed: variable " + std::to_string(var)
                            + ", degree " + std::to_string(k) + " — divisibility check failed"
                    );
                }

                const uint64_t kShifted = kDk >> kV;
                const uint64_t kFactorialCoeff =
                    (kShifted * info[k].odd_inverse) & Bitmask(info[k].precision_bits);

                if (kFactorialCoeff != 0) {
                    terms.push_back(
                        { .degree = static_cast< uint16_t >(k), .coeff = kFactorialCoeff }
                    );
                }
            }

            result.per_var[var].terms = std::move(terms);
        }

        COBRA_TRACE("PowerRecovery", "RecoverSingletonPowers: found={}", true);
        return Ok(std::move(result));
    }

} // namespace cobra
