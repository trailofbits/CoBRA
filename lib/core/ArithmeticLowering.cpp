#include "cobra/core/ArithmeticLowering.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/ExponentTuple.h"
#include "cobra/core/PolyIR.h"
#include "cobra/core/Result.h"
#include "cobra/core/Trace.h"
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cobra {

    Result< LoweringResult > LowerArithmeticFragment(
        const std::vector< Coeff > &and_coeffs, const std::vector< Coeff > &mul_coeffs,
        uint8_t num_vars, uint32_t bitwidth
    ) {
        COBRA_TRACE(
            "ArithLowering", "LowerArithmeticFragment: vars={} bitwidth={}", num_vars, bitwidth
        );
        if (num_vars > kMaxPolyVars) {
            return Err< LoweringResult >(
                CobraError::kTooManyVariables,
                "lower_arithmetic_fragment: num_vars (" + std::to_string(num_vars)
                    + ") exceeds kMaxPolyVars (" + std::to_string(kMaxPolyVars) + ")"
            );
        }
        const size_t kLen = size_t{ 1 } << num_vars;
        assert(and_coeffs.size() == kLen && mul_coeffs.size() == kLen);

        const uint64_t kMask = Bitmask(bitwidth);

        PolyIR poly; // NOLINT(misc-const-correctness)
        poly.num_vars = num_vars;
        poly.bitwidth = bitwidth;

        std::vector< Coeff > residual(and_coeffs);

        // Singleton terms: and_coeffs[1<<i] as x_i,
        // mul_coeffs[1<<i] as x_i^2
        for (uint8_t i = 0; i < num_vars; ++i) {
            const uint64_t kSingleton = 1ULL << i;
            const Coeff kLinear       = and_coeffs[kSingleton] & kMask;
            const Coeff kSquare       = mul_coeffs[kSingleton] & kMask;

            if (kLinear != 0) {
                std::array< uint8_t, kMaxPolyVars > exps{};
                exps[i]         = 1;
                auto key        = ExponentTuple::FromExponents(exps.data(), num_vars);
                poly.terms[key] = (poly.terms[key] + kLinear) & kMask;
            }

            if (kSquare != 0) {
                std::array< uint8_t, kMaxPolyVars > exps{};
                exps[i]         = 2;
                auto key        = ExponentTuple::FromExponents(exps.data(), num_vars);
                poly.terms[key] = (poly.terms[key] + kSquare) & kMask;
            }

            residual[kSingleton] = 0;
        }

        // Multi-variable products: transfer mul_coeffs[m] for |m|>=2
        for (size_t m = 0; m < kLen; ++m) {
            if (std::popcount(m) < 2) { continue; }
            const Coeff kC = mul_coeffs[m] & kMask;
            if (kC == 0) { continue; }

            std::array< uint8_t, kMaxPolyVars > exps{};
            for (uint8_t v = 0; v < num_vars; ++v) {
                if ((m & (1ULL << v)) != 0u) { exps[v] = 1; }
            }
            auto key        = ExponentTuple::FromExponents(exps.data(), num_vars);
            poly.terms[key] = (poly.terms[key] + kC) & kMask;
        }

        // Strip zeros
        for (auto it = poly.terms.begin(); it != poly.terms.end();) {
            if (it->second == 0) {
                it = poly.terms.erase(it);
            } else {
                ++it;
            }
        }

        auto result = LoweringResult{ .poly                = std::move(poly),
                                      .residual_and_coeffs = std::move(residual) };
        COBRA_TRACE("ArithLowering", "LowerArithmeticFragment: success={}", true);
        return Ok(std::move(result));
    }

} // namespace cobra
