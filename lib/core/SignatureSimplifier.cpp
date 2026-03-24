#include "cobra/core/SignatureSimplifier.h"
#include "cobra/core/AnfTransform.h"
#include "cobra/core/ArithmeticLowering.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/BitwiseDecomposer.h"
#include "cobra/core/Classification.h"
#include "cobra/core/CoBExprBuilder.h"
#include "cobra/core/CoeffInterpolator.h"
#include "cobra/core/CoefficientSplitter.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/HybridDecomposer.h"
#include "cobra/core/MultivarPolyRecovery.h"
#include "cobra/core/PatternMatcher.h"
#include "cobra/core/PolyExprBuilder.h"
#include "cobra/core/PolyNormalizer.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/Simplifier.h"
#include "cobra/core/SingletonPowerExprBuilder.h"
#include "cobra/core/SingletonPowerPoly.h"
#include "cobra/core/SingletonPowerRecovery.h"
#include "cobra/core/Trace.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

namespace cobra {

    bool IsBooleanValued(const std::vector< uint64_t > &sig) {
        return std::all_of(sig.begin(), sig.end(), [](uint64_t v) { return v <= 1; });
    }

    namespace {

        bool VerifyCandidate(
            const Expr &expr, const std::vector< uint64_t > &sig, uint32_t num_vars,
            const SignatureContext &ctx, const Options &opts
        ) {
            if (opts.spot_check) {
                auto check = SignatureCheck(sig, expr, num_vars, opts.bitwidth);
                if (!check.passed) { return false; }
            }
            if (ctx.eval.has_value()) {
                auto check = FullWidthCheckEval(*ctx.eval, num_vars, expr, opts.bitwidth);
                if (!check.passed) { return false; }
            }
            return true;
        }

        // NOLINTNEXTLINE(readability-identifier-naming)
        void TryUpdateBest(
            std::optional< SubResult > &best, std::unique_ptr< Expr > candidate, bool verified,
            const ExprCost *baseline_cost
        ) {
            auto info = ComputeCost(*candidate);
            if ((baseline_cost != nullptr) && !IsBetter(info.cost, *baseline_cost)) { return; }
            if (best.has_value() && !IsBetter(info.cost, best->cost)) { return; }
            SubResult sub;
            sub.expr     = std::move(candidate);
            sub.cost     = info.cost;
            sub.verified = verified;
            best         = std::move(sub);
        }

        /// Evaluate the singleton polynomial S_i(t) at t=2.
        ///
        /// Only degrees 1 and 2 contribute because
        /// falling_factorial(2, k) = 0 for k >= 3.
        /// Both falling_factorial(2, 1) and falling_factorial(2, 2) equal 2.
        uint64_t EvalUnivariateAt2(const UnivariateNormalizedPoly &poly, uint32_t bitwidth) {
            const uint64_t kMask = Bitmask(bitwidth);
            uint64_t sum         = 0;
            for (const auto &term : poly.terms) {
                if (term.degree >= 3) {
                    break; // sorted ascending
                }
                sum = (sum + (term.coeff * 2)) & kMask;
            }
            return sum;
        }

    } // namespace

    std::optional< SubResult > SimplifyFromSignature(
        const std::vector< uint64_t > &sig, const SignatureContext &ctx, const Options &opts,
        uint32_t depth, const ExprCost *baseline_cost
    ) {
        const auto kNumVars = static_cast< uint32_t >(ctx.vars.size());
        COBRA_TRACE(
            "SigSimplifier", "SimplifyFromSignature: vars={} depth={} has_baseline={}",
            kNumVars, depth, baseline_cost != nullptr
        );
        COBRA_TRACE_SIG("SigSimplifier", "input sig", sig);
        std::optional< SubResult > best;

        // Step 1: Fast-match canonical patterns on signature
        auto pm = MatchPattern(sig, kNumVars, opts.bitwidth);
        if (pm) {
            bool pm_accepted = true;
            if (ctx.eval.has_value()) {
                auto check = FullWidthCheckEval(*ctx.eval, kNumVars, **pm, opts.bitwidth);
                if (!check.passed) { pm_accepted = false; }
            }

            if (pm_accepted) {
                bool pm_verified = true;
                if (opts.spot_check) {
                    auto check = SignatureCheck(sig, **pm, kNumVars, opts.bitwidth);
                    if (!check.passed) {
                        pm_verified = false;
                        pm_accepted = false;
                    }
                }
                if (pm_accepted) {
                    TryUpdateBest(best, std::move(*pm), pm_verified, baseline_cost);
                }
            }
            COBRA_TRACE(
                "SigSimplifier", "Stage1 PatternMatch: found={} accepted={}", pm.has_value(),
                pm.has_value() && pm_accepted
            );
        }

        // Step 2: ANF fast path for Boolean signatures
        if (IsBooleanValued(sig) && kNumVars <= opts.max_vars) {
            auto anf      = ComputeAnf(sig, kNumVars);
            auto anf_expr = BuildAnfExpr(anf, kNumVars);

            bool anf_accepted = true;
            if (opts.spot_check) {
                auto check = SignatureCheck(sig, *anf_expr, kNumVars, opts.bitwidth);
                if (!check.passed) { anf_accepted = false; }
            }
            if (anf_accepted && ctx.eval.has_value()) {
                auto check = FullWidthCheckEval(*ctx.eval, kNumVars, *anf_expr, opts.bitwidth);
                if (!check.passed) { anf_accepted = false; }
            }

            if (anf_accepted) {
                TryUpdateBest(best, std::move(anf_expr), /*verified=*/true, baseline_cost);
            }
            COBRA_TRACE(
                "SigSimplifier", "Stage2 ANF: boolean_valued={} accepted={}",
                IsBooleanValued(sig), anf_accepted
            );
        }

        // Step 3: Interpolate coefficients
        auto coeffs = InterpolateCoefficients(sig, kNumVars, opts.bitwidth);
        COBRA_TRACE_SIG("SigSimplifier", "Stage3 coefficients", coeffs);

        // Step 4: Build expression from CoB coefficients
        auto expr = BuildCobExpr(coeffs, kNumVars, opts.bitwidth);
        COBRA_TRACE_EXPR("SigSimplifier", "Stage4 CoB expr", *expr, ctx.vars, opts.bitwidth);

        // Step 4b: kPolynomial recovery via singleton powers + coefficient splitting.
        // Singleton recovery runs first so the splitter can use the
        // recovered polynomial evaluations S_i(2) instead of the
        // CoB-linear model for singleton submask contributions.
        bool poly_recovery_verified = false;
        if (ctx.eval.has_value()) {
            auto singleton_result = RecoverSingletonPowers(*ctx.eval, kNumVars, opts.bitwidth);
            COBRA_TRACE(
                "SigSimplifier", "Stage4b: singleton_recovery found={}",
                singleton_result.has_value()
            );

            std::vector< uint64_t > singleton_at_2;
            if (singleton_result.has_value()) {
                const auto &sr = singleton_result.value();
                singleton_at_2.resize(kNumVars, 0);
                for (uint32_t i = 0; i < kNumVars; ++i) {
                    singleton_at_2[i] = EvalUnivariateAt2(sr.per_var[i], opts.bitwidth);
                }
            }

            auto split =
                SplitCoefficients(coeffs, *ctx.eval, kNumVars, opts.bitwidth, singleton_at_2);

            const bool has_mul =
                std::any_of(split.mul_coeffs.begin(), split.mul_coeffs.end(), [](uint64_t c) {
                    return c != 0;
                });

            std::unique_ptr< Expr > poly_expr = nullptr;
            std::vector< uint64_t > residual  = split.and_coeffs;

            if (has_mul) {
                auto lowered = LowerArithmeticFragment(
                    split.and_coeffs, split.mul_coeffs, static_cast< uint8_t >(kNumVars),
                    opts.bitwidth
                );

                if (lowered.has_value()) {
                    auto normalized = NormalizePolynomial(lowered.value().poly);
                    auto built      = BuildPolyExpr(normalized);
                    if (built.has_value()) {
                        poly_expr = std::move(built.value());
                        residual  = lowered.value().residual_and_coeffs;
                    }
                }
            }

            std::unique_ptr< Expr > singleton_expr = nullptr;
            if (singleton_result.has_value()) {
                singleton_expr = BuildSingletonPowerExpr(singleton_result.value());
            }

            auto bit_expr = BuildCobExpr(residual, kNumVars, opts.bitwidth);

            const bool kBitIsZero = bit_expr && bit_expr->kind == Expr::Kind::kConstant
                && bit_expr->constant_val == 0;

            std::unique_ptr< Expr > combined;
            if (bit_expr && !kBitIsZero) { combined = std::move(bit_expr); }
            if (poly_expr) {
                combined = combined ? Expr::Add(std::move(combined), std::move(poly_expr))
                                    : std::move(poly_expr);
            }
            if (singleton_expr) {
                combined = combined ? Expr::Add(std::move(combined), std::move(singleton_expr))
                                    : std::move(singleton_expr);
            }
            if (!combined) { combined = Expr::Constant(0); }

            auto check = FullWidthCheckEval(*ctx.eval, kNumVars, *combined, opts.bitwidth);
            COBRA_TRACE("SigSimplifier", "Stage4b: poly_recovery verified={}", check.passed);
            if (check.passed) {
                poly_recovery_verified = true;
                expr                   = std::move(combined);
            }
        }

        // Step 4c: Multivariate falling-factorial polynomial recovery.
        // Only fires for multivar-high-power expressions where step 4b
        // did not produce a full-width-verified candidate.
        if (ctx.eval.has_value() && !poly_recovery_verified
            && HasFlag(opts.structural_flags, kSfHasMultivarHighPower) && kNumVars <= 6)
        {
            std::vector< uint32_t > support(kNumVars);
            std::iota(support.begin(), support.end(), 0U);
            auto recovery = RecoverAndVerifyPoly(*ctx.eval, support, kNumVars, opts.bitwidth);
            if (recovery.Succeeded()) {
                TryUpdateBest(
                    best, std::move(recovery.TakePayload().expr), /*verified=*/true,
                    baseline_cost
                );
            }
        }

        // Step 5: Cofactor bitwise decomposition
        if (opts.enable_bitwise_decomposition && ctx.eval.has_value() && depth < 2) {
            const ExprCost *bl = best.has_value() ? &best->cost : baseline_cost;
            auto decomp        = TryBitwiseDecomposition(sig, ctx, opts, depth, bl);
            if (decomp.has_value()) {
                if (!best.has_value() || IsBetter(decomp->cost, best->cost)) {
                    best = std::move(decomp);
                }
            }
            COBRA_TRACE("SigSimplifier", "Stage5: bitwise_decomp found={}", decomp.has_value());
        }

        // Step 5b: Variable-extraction hybrid decomposition.
        // Only try when no full-width-verified result exists yet —
        // avoids expensive extraction for expressions already solved
        // by steps 1–5.
        if (opts.enable_bitwise_decomposition && ctx.eval.has_value() && depth < 2
            && (!best.has_value() || !best->verified))
        {
            const ExprCost *bl = best.has_value() ? &best->cost : baseline_cost;
            auto hybrid        = TryHybridDecomposition(sig, ctx, opts, depth, bl);
            if (hybrid.has_value()) {
                if (!best.has_value() || IsBetter(hybrid->cost, best->cost)) {
                    best = std::move(hybrid);
                }
            }
            COBRA_TRACE("SigSimplifier", "Stage5b: hybrid_decomp found={}", hybrid.has_value());
        }

        // Step 6: Accept CoB result as final candidate
        {
            bool cob_verified = false;
            if (opts.spot_check) {
                auto check   = SignatureCheck(sig, *expr, kNumVars, opts.bitwidth);
                cob_verified = check.passed;
            }
            if (ctx.eval.has_value()) {
                auto check = FullWidthCheckEval(*ctx.eval, kNumVars, *expr, opts.bitwidth);
                if (check.passed) {
                    cob_verified = true;
                } else if (best.has_value() && best->verified) {
                    // Don't let a full-width-incorrect CoB result
                    // override an already-verified candidate.
                    return best;
                }
            }
            TryUpdateBest(best, std::move(expr), cob_verified, baseline_cost);
        }

        COBRA_TRACE(
            "SigSimplifier", "Final: has_result={} verified={}", best.has_value(),
            best.has_value() && best->verified
        );
        return best;
    }

} // namespace cobra
