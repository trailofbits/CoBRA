#include "cobra/core/SingletonPowerExprBuilder.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SingletonPowerPoly.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace cobra {

    std::vector< uint64_t >
    FactorialToMonomial(const std::vector< UnivariateTerm > &terms, uint32_t bitwidth) {
        if (terms.empty()) { return {}; }

        uint16_t d_max = 0;
        for (const auto &t : terms) { d_max = std::max(t.degree, d_max); }

        const uint64_t kMask = Bitmask(bitwidth);

        // Preindex factorial-basis coefficients into a dense array.
        std::vector< uint64_t > h(d_max + 1, 0);
        for (const auto &t : terms) { h[t.degree] = t.coeff; }

        // Accumulate monomial coefficients a[j] for j=0..d_max.
        // For each degree k, compute signed Stirling numbers s(k, j) via the
        // recurrence s(k, j) = s(k-1, j-1) - (k-1)*s(k-1, j), s(0,0)=1,
        // then accumulate h[k] * s(k, j) into a[j]. All arithmetic mod 2^w.
        std::vector< uint64_t > mono(d_max + 1, 0);
        std::vector< uint64_t > s_prev(d_max + 1, 0);
        std::vector< uint64_t > s_curr(d_max + 1, 0);
        s_prev[0] = 1; // s(0, 0) = 1

        for (uint16_t k = 1; k <= d_max; ++k) {
            // Compute s(k, j) from s(k-1, j).
            // Recurrence: s(k, j) = s(k-1, j-1) - (k-1) * s(k-1, j)
            std::fill(s_curr.begin(), s_curr.end(), 0);
            const auto kKm1 = static_cast< uint64_t >(k - 1);
            for (uint16_t j = 0; j <= k; ++j) {
                const uint64_t kFromLeft = (j > 0) ? s_prev[j - 1] : 0;
                const uint64_t kFromDiag = s_prev[j];
                // Modular: (kFromLeft - kKm1 * kFromDiag) mod 2^w.
                // Unsigned subtraction wraps correctly for all bitwidths.
                s_curr[j]                = (kFromLeft - ((kKm1 * kFromDiag) & kMask)) & kMask;
            }

            // Accumulate h[k] * s(k, j) into mono[j].
            if (h[k] != 0) {
                for (uint16_t j = 1; j <= k; ++j) {
                    mono[j] = (mono[j] + (h[k] * s_curr[j])) & kMask;
                }
            }

            std::swap(s_prev, s_curr);
        }

        return mono;
    }

    std::unique_ptr< Expr > BuildSingletonPowerExpr(const SingletonPowerResult &powers) {
        const uint32_t kW     = powers.bitwidth;
        const size_t kNumVars = powers.per_var.size();

        // Collect per-variable Expr trees.
        std::vector< std::unique_ptr< Expr > > var_exprs;

        for (size_t i = 0; i < kNumVars; ++i) {
            const auto &uni = powers.per_var[i];
            if (uni.terms.empty()) { continue; }

            auto mono = FactorialToMonomial(uni.terms, kW);

            // Build per-degree terms: for each j with mono[j] != 0, emit
            // coeff * x^j. Power is built incrementally to avoid redundant
            // clones; we clone only when the power is reused across degrees.
            std::vector< std::unique_ptr< Expr > > degree_exprs;
            std::unique_ptr< Expr > power = nullptr;

            for (size_t j = 1; j < mono.size(); ++j) {
                // Extend power: x^j = x^{j-1} * x.
                if (j == 1) {
                    power = Expr::Variable(static_cast< uint32_t >(i));
                } else {
                    // Previous power consumed by Mul; result is new power
                    power =
                        Expr::Mul(std::move(power), Expr::Variable(static_cast< uint32_t >(i)));
                }

                if (mono[j] != 0) {
                    // Move power on the last iteration; clone otherwise
                    if (j + 1 < mono.size()) {
                        degree_exprs.push_back(
                            ApplyCoefficient(CloneExpr(*power), mono[j], kW)
                        );
                    } else {
                        degree_exprs.push_back(ApplyCoefficient(std::move(power), mono[j], kW));
                    }
                }
            }

            if (degree_exprs.empty()) { continue; }

            // Reduce degree terms into a balanced add tree.
            while (degree_exprs.size() > 1) {
                std::vector< std::unique_ptr< Expr > > next;
                for (size_t k = 0; k < degree_exprs.size(); k += 2) {
                    if (k + 1 < degree_exprs.size()) {
                        next.push_back(
                            Expr::Add(
                                std::move(degree_exprs[k]), std::move(degree_exprs[k + 1])
                            )
                        );
                    } else {
                        next.push_back(std::move(degree_exprs[k]));
                    }
                }
                degree_exprs = std::move(next);
            }
            var_exprs.push_back(std::move(degree_exprs[0]));
        }

        if (var_exprs.empty()) { return nullptr; }

        // Reduce variable trees into a balanced add tree.
        while (var_exprs.size() > 1) {
            std::vector< std::unique_ptr< Expr > > next;
            for (size_t k = 0; k < var_exprs.size(); k += 2) {
                if (k + 1 < var_exprs.size()) {
                    next.push_back(
                        Expr::Add(std::move(var_exprs[k]), std::move(var_exprs[k + 1]))
                    );
                } else {
                    next.push_back(std::move(var_exprs[k]));
                }
            }
            var_exprs = std::move(next);
        }
        return std::move(var_exprs[0]);
    }

} // namespace cobra
