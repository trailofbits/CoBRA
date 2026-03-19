#include "cobra/core/MultivarPolyRecovery.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/ExponentTuple.h"
#include "cobra/core/PolyIR.h"
#include "cobra/core/Simplifier.h"
#include <cassert>
#include <cstdint>
#include <optional>
#include <vector>

namespace cobra {

    std::optional< NormalizedPoly > RecoverMultivarPoly(
        const Evaluator &eval, const std::vector< uint32_t > &support_vars,
        uint32_t total_num_vars, uint32_t bitwidth
    ) {
        if (support_vars.empty()) {
            return std::nullopt;
        }
        if (total_num_vars > kMaxPolyVars) {
            return std::nullopt;
        }
        if (bitwidth < 2 || bitwidth > 64) {
            return std::nullopt;
        }
        for (auto idx : support_vars) {
            if (idx >= total_num_vars) {
                return std::nullopt;
            }
        }

        const auto k        = static_cast< uint32_t >(support_vars.size());
        const uint64_t mask = Bitmask(bitwidth);

        // Compute 3^k (table size)
        uint32_t table_size = 1;
        for (uint32_t i = 0; i < k; ++i) {
            table_size *= 3;
        }

        // Evaluate on {0,1,2}^k grid.
        // Little-endian mixed-radix: index = e_0 + 3*e_1 + 9*e_2 + ...
        std::vector< uint64_t > table(table_size);
        std::vector< uint64_t > point(total_num_vars, 0);

        for (uint32_t idx = 0; idx < table_size; ++idx) {
            uint32_t tmp = idx;
            for (uint32_t i = 0; i < k; ++i) {
                point[support_vars[i]]  = tmp % 3;
                tmp                    /= 3;
            }
            table[idx] = eval(point) & mask;
        }

        for (uint32_t i = 0; i < k; ++i) {
            point[support_vars[i]] = 0;
        }

        // Tensor-product forward differences (Newton's forward
        // difference tableau). For each dimension, apply two passes
        // of in-place subtraction to compute second-order differences.
        for (uint32_t dim = 0; dim < k; ++dim) {
            uint32_t stride = 1;
            for (uint32_t i = 0; i < dim; ++i) {
                stride *= 3;
            }

            // Two passes: pass p subtracts from entries with coord >= p
            for (uint32_t pass = 1; pass <= 2; ++pass) {
                for (uint32_t idx = table_size; idx-- > 0;) {
                    const uint32_t coord = (idx / stride) % 3;
                    if (coord < pass) {
                        continue;
                    }
                    table[idx] = (table[idx] - table[idx - stride]) & mask;
                }
            }
        }

        // Divisibility gate + convert to factorial-basis coefficients.
        auto nv = static_cast< uint8_t >(total_num_vars);
        NormalizedPoly result;
        result.num_vars = nv;
        result.bitwidth = bitwidth;

        for (uint32_t idx = 0; idx < table_size; ++idx) {
            const uint64_t alpha = table[idx];
            if (alpha == 0) {
                continue;
            }

            // Decode little-endian mixed-radix to per-variable exponents
            uint8_t exps[kMaxPolyVars] = {};
            uint32_t q                 = 0;
            uint32_t tmp               = idx;
            for (uint32_t i = 0; i < k; ++i) {
                const auto e          = static_cast< uint8_t >(tmp % 3);
                exps[support_vars[i]] = e;
                if (e == 2) {
                    ++q;
                }
                tmp /= 3;
            }

            // alpha must be divisible by 2^q
            if (q > 0) {
                if (q >= bitwidth) {
                    continue;
                }
                const uint64_t low_bits = alpha & ((1ULL << q) - 1);
                if (low_bits != 0) {
                    return std::nullopt;
                }
            }

            // h_e = alpha / 2^q, reduced mod 2^{w-q}
            uint64_t h                = alpha >> q;
            const uint32_t bound_bits = bitwidth - q;
            if (bound_bits < 64) {
                h &= (1ULL << bound_bits) - 1;
            }

            if (h == 0) {
                continue;
            }

            auto key           = ExponentTuple::FromExponents(exps, nv);
            result.coeffs[key] = h;
        }

        return result;
    }

} // namespace cobra
