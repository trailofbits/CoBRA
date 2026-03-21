#include "cobra/core/GhostResidualSolver.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/GhostBasis.h"
#include "cobra/core/MathUtils.h"
#include "cobra/core/PolyExprBuilder.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/WeightedPolyFit.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>

namespace cobra {

    namespace {

        // Fixed deterministic mixed-parity probe bank.
        // Shared by IsBooleanNullResidual and SolveGhostResidual.
        // Mixed parity gives varied 2-adic valuations.
        struct ProbePoint
        {
            std::array< uint64_t, 6 > values{}; // max 6 vars
        };

        constexpr int kNumProbes = 8;

        bool NextCombo(std::vector< uint32_t > &combo, uint8_t arity, uint32_t support_size) {
            int i = static_cast< int >(arity) - 1;
            while (i >= 0) {
                combo[static_cast< size_t >(i)]++;
                if (combo[static_cast< size_t >(i)]
                    <= support_size - arity + static_cast< uint32_t >(i))
                {
                    for (uint32_t j = static_cast< uint32_t >(i) + 1; j < arity; ++j) {
                        combo[j] = combo[j - 1] + 1;
                    }
                    return true;
                }
                --i;
            }
            return false;
        }

        void GenerateProbeBank(
            std::array< ProbePoint, kNumProbes > &bank, uint32_t num_vars, uint32_t bitwidth
        ) {
            const uint64_t kMask                        = Bitmask(bitwidth);
            // Deterministic seed values — mixed parity (even and odd).
            constexpr std::array< uint64_t, 48 > kSeeds = {
                3, 4,  7,  10, 13, 18, 23, 28, 37, 42, 51, 60, 71, 80, 97, 106,
                5, 6,  11, 14, 19, 22, 29, 34, 41, 50, 59, 66, 73, 82, 91, 100,
                9, 12, 17, 20, 25, 30, 35, 40, 47, 56, 63, 70, 79, 86, 95, 102,
            };
            for (int p = 0; p < kNumProbes; ++p) {
                for (uint32_t v = 0; v < num_vars && v < 6; ++v) {
                    bank[static_cast< size_t >(p)].values[v] =
                        kSeeds[static_cast< size_t >((p * 6) + v)] & kMask;
                }
            }
        }

    } // namespace

    bool IsBooleanNullResidual(
        const Evaluator &residual_eval, const std::vector< uint32_t > &support,
        uint32_t num_vars, uint32_t bitwidth, const std::vector< uint64_t > &boolean_sig
    ) {
        // Condition 1: all boolean-signature entries are zero
        if (!std::all_of(
                boolean_sig.begin(), boolean_sig.end(), [](uint64_t v) { return v == 0; }
            ))
        {
            return false;
        }

        // Condition 2: nonzero at some non-boolean full-width point
        // Probe in support-local space, map to full space via support[].
        const auto kSupportSize = static_cast< uint32_t >(support.size());
        if (kSupportSize > 6) { return false; }

        std::array< ProbePoint, kNumProbes > bank{};
        GenerateProbeBank(bank, kSupportSize, bitwidth);

        const uint64_t kMask = Bitmask(bitwidth);
        for (int p = 0; p < kNumProbes; ++p) {
            std::vector< uint64_t > point(num_vars, 0);
            for (uint32_t v = 0; v < kSupportSize; ++v) {
                point[support[v]] = bank[static_cast< size_t >(p)].values[v];
            }
            uint64_t val = residual_eval(point) & kMask;
            if (val != 0) { return true; }
        }
        return false;
    }

    std::optional< GhostSolveResult > SolveGhostResidual(
        const Evaluator &residual_eval, const std::vector< uint32_t > &support,
        uint32_t num_vars, uint32_t bitwidth
    ) {
        const uint64_t kMask    = Bitmask(bitwidth);
        const auto kSupportSize = static_cast< uint32_t >(support.size());
        const auto &basis       = GetGhostBasis();

        std::array< ProbePoint, kNumProbes > bank{};
        GenerateProbeBank(bank, kSupportSize, bitwidth);

        // Evaluate residual at each probe point (in full original space)
        std::array< uint64_t, kNumProbes > r_vals{};
        for (int p = 0; p < kNumProbes; ++p) {
            std::vector< uint64_t > full(num_vars, 0);
            for (uint32_t v = 0; v < kSupportSize; ++v) {
                full[support[v]] = bank[static_cast< size_t >(p)].values[v];
            }
            r_vals[static_cast< size_t >(p)] = residual_eval(full) & kMask;
        }

        for (const auto &prim : basis) {
            if (prim.arity > kSupportSize) { continue; }

            // Enumerate strictly increasing index combinations
            std::vector< uint32_t > combo(prim.arity);
            for (uint8_t i = 0; i < prim.arity; ++i) { combo[i] = i; }

            do {
                // Evaluate ghost at each probe point
                std::array< uint64_t, kNumProbes > g_vals{};
                for (int p = 0; p < kNumProbes; ++p) {
                    std::vector< uint64_t > args(prim.arity);
                    for (uint8_t a = 0; a < prim.arity; ++a) {
                        args[a] = bank[static_cast< size_t >(p)].values[combo[a]];
                    }
                    g_vals[static_cast< size_t >(p)] = prim.eval(args, bitwidth);
                }

                // 2-adic coefficient inference
                int best_probe    = -1;
                uint32_t best_t   = bitwidth;
                uint64_t best_c   = 0;
                bool tuple_reject = false;

                for (int p = 0; p < kNumProbes; ++p) {
                    auto pidx = static_cast< size_t >(p);
                    if (g_vals[pidx] == 0) { continue; }
                    auto t = static_cast< uint32_t >(std::countr_zero(g_vals[pidx]));
                    // r must be divisible by 2^t
                    if (t > 0 && (r_vals[pidx] & ((1ULL << t) - 1)) != 0) {
                        tuple_reject = true;
                        break;
                    }
                    if (t < best_t) {
                        best_t        = t;
                        uint32_t prec = bitwidth - t;
                        if (prec == 0) { continue; }
                        uint64_t g_odd     = g_vals[pidx] >> t;
                        uint64_t r_shifted = r_vals[pidx] >> t;
                        uint64_t inv       = ModInverseOdd(g_odd, prec);
                        uint64_t prec_mask = (prec >= 64) ? UINT64_MAX : (1ULL << prec) - 1;
                        best_c             = (r_shifted * inv) & prec_mask;
                        best_probe         = p;
                    }
                }

                if (tuple_reject || best_probe < 0) { continue; }

                // Cross-check against all probes
                bool cross_ok = true;
                for (int p = 0; p < kNumProbes; ++p) {
                    auto pidx         = static_cast< size_t >(p);
                    uint64_t expected = (best_c * g_vals[pidx]) & kMask;
                    if (expected != r_vals[pidx]) {
                        cross_ok = false;
                        break;
                    }
                }
                if (!cross_ok) { continue; }

                // Build the expression: c * ghost(var_indices)
                std::vector< uint32_t > var_indices(prim.arity);
                for (uint8_t a = 0; a < prim.arity; ++a) { var_indices[a] = support[combo[a]]; }
                auto ghost_expr = prim.build(var_indices);

                std::unique_ptr< Expr > result_expr;
                if (best_c == 1) {
                    result_expr = std::move(ghost_expr);
                } else {
                    result_expr = Expr::Mul(Expr::Constant(best_c), std::move(ghost_expr));
                }

                // Verify against residual evaluator
                auto check =
                    FullWidthCheckEval(residual_eval, num_vars, *result_expr, bitwidth);
                if (check.passed) {
                    GhostSolveResult res;
                    res.expr = std::move(result_expr);
                    res.primitives_used.push_back(prim.name);
                    res.num_terms = 1;
                    return res;
                }
            } while (NextCombo(combo, prim.arity, kSupportSize));
        }

        return std::nullopt;
    }

    std::optional< GhostSolveResult > SolveFactoredGhostResidual(
        const Evaluator &residual_eval, const std::vector< uint32_t > &support,
        uint32_t num_vars, uint32_t bitwidth, uint8_t max_degree, uint8_t grid_degree
    ) {
        const auto kSupportSize = static_cast< uint32_t >(support.size());
        const auto &basis       = GetGhostBasis();

        for (const auto &prim : basis) {
            if (prim.arity > kSupportSize) { continue; }

            // Enumerate strictly increasing index combinations
            std::vector< uint32_t > combo(prim.arity);
            for (uint8_t i = 0; i < prim.arity; ++i) { combo[i] = i; }

            do {
                // Map support-local combo to original-space variable indices
                std::vector< uint32_t > var_indices(prim.arity);
                for (uint8_t a = 0; a < prim.arity; ++a) { var_indices[a] = support[combo[a]]; }

                // Build weight function from ghost primitive + tuple
                WeightFn weight =
                    [&prim, combo](std::span< const uint64_t > args, uint32_t bw) -> uint64_t {
                    std::vector< uint64_t > ghost_args(prim.arity);
                    for (uint8_t a = 0; a < prim.arity; ++a) { ghost_args[a] = args[combo[a]]; }
                    return prim.eval(ghost_args, bw);
                };

                auto fit = RecoverWeightedPoly(
                    residual_eval, weight, support, num_vars, bitwidth, max_degree, grid_degree
                );
                if (!fit.has_value()) { continue; }

                auto q_expr = BuildPolyExpr(fit->poly);
                if (!q_expr.has_value()) { continue; }

                auto g_expr   = prim.build(var_indices);
                auto combined = Expr::Mul(std::move(*q_expr), std::move(g_expr));

                auto check = FullWidthCheckEval(residual_eval, num_vars, *combined, bitwidth);
                if (check.passed) {
                    GhostSolveResult res;
                    res.expr = std::move(combined);
                    res.primitives_used.push_back(prim.name);
                    res.num_terms = 1;
                    return res;
                }
            } while (NextCombo(combo, prim.arity, kSupportSize));
        }

        return std::nullopt;
    }

} // namespace cobra
