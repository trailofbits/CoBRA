#include "cobra/core/ArithmeticLowering.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/ExponentTuple.h"
#include "cobra/core/PolyIR.h"
#include "cobra/core/Result.h"
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
        if (num_vars > kMaxPolyVars) {
            return Err< LoweringResult >(
                CobraError::kTooManyVariables,
                "lower_arithmetic_fragment: num_vars (" + std::to_string(num_vars)
                    + ") exceeds kMaxPolyVars (" + std::to_string(kMaxPolyVars) + ")"
            );
        }
        const size_t len = size_t{ 1 } << num_vars;
        assert(and_coeffs.size() == len && mul_coeffs.size() == len);

        const uint64_t mask = Bitmask(bitwidth);

        PolyIR poly;
        poly.num_vars = num_vars;
        poly.bitwidth = bitwidth;

        std::vector< Coeff > residual(and_coeffs);

        // Singleton terms: and_coeffs[1<<i] as x_i,
        // mul_coeffs[1<<i] as x_i^2
        for (uint8_t i = 0; i < num_vars; ++i) {
            const uint64_t singleton = 1ULL << i;
            const Coeff linear       = and_coeffs[singleton] & mask;
            const Coeff square       = mul_coeffs[singleton] & mask;

            if (linear != 0) {
                uint8_t exps[kMaxPolyVars] = {};
                exps[i]                    = 1;
                auto key                   = ExponentTuple::FromExponents(exps, num_vars);
                poly.terms[key]            = (poly.terms[key] + linear) & mask;
            }

            if (square != 0) {
                uint8_t exps[kMaxPolyVars] = {};
                exps[i]                    = 2;
                auto key                   = ExponentTuple::FromExponents(exps, num_vars);
                poly.terms[key]            = (poly.terms[key] + square) & mask;
            }

            residual[singleton] = 0;
        }

        // Multi-variable products: transfer mul_coeffs[m] for |m|>=2
        for (size_t m = 0; m < len; ++m) {
            if (std::popcount(m) < 2) {
                continue;
            }
            const Coeff c = mul_coeffs[m] & mask;
            if (c == 0) {
                continue;
            }

            uint8_t exps[kMaxPolyVars] = {};
            for (uint8_t v = 0; v < num_vars; ++v) {
                if ((m & (1ULL << v)) != 0u) {
                    exps[v] = 1;
                }
            }
            auto key        = ExponentTuple::FromExponents(exps, num_vars);
            poly.terms[key] = (poly.terms[key] + c) & mask;
        }

        // Strip zeros
        for (auto it = poly.terms.begin(); it != poly.terms.end();) {
            if (it->second == 0) {
                it = poly.terms.erase(it);
            } else {
                ++it;
            }
        }

        return Ok(LoweringResult{ .poly                = std::move(poly),
                                  .residual_and_coeffs = std::move(residual) });
    }

} // namespace cobra
