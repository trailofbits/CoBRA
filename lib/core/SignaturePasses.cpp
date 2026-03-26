#include "SignaturePasses.h"
#include "CompetitionGroup.h"
#include "ContinuationTypes.h"
#include "OrchestratorPasses.h"
#include "cobra/core/AnfTransform.h"
#include "cobra/core/ArithmeticLowering.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Classification.h"
#include "cobra/core/CoBExprBuilder.h"
#include "cobra/core/CoeffInterpolator.h"
#include "cobra/core/CoefficientSplitter.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/MultivarPolyRecovery.h"
#include "cobra/core/PatternMatcher.h"
#include "cobra/core/PolyExprBuilder.h"
#include "cobra/core/PolyNormalizer.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureSimplifier.h"
#include "cobra/core/SingletonPowerExprBuilder.h"
#include "cobra/core/SingletonPowerPoly.h"
#include "cobra/core/SingletonPowerRecovery.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <numeric>
#include <optional>
#include <vector>

namespace cobra {

    namespace {

        // Build a mapped evaluator for the reduced variable space.
        // When the reduced space matches the original, returns the
        // evaluator directly. Otherwise builds a remapping lambda.
        std::optional< Evaluator > BuildMappedEvaluator(
            const OrchestratorContext &ctx, const SignatureSubproblemContext &sub_ctx
        ) {
            if (!ctx.evaluator) { return std::nullopt; }
            if (sub_ctx.real_vars.size() == ctx.original_vars.size()) { return *ctx.evaluator; }
            return Evaluator(
                [eval = *ctx.evaluator, idx_map = sub_ctx.original_indices,
                 original_vals = std::vector< uint64_t >(ctx.original_vars.size(), 0)](
                    const std::vector< uint64_t > &reduced_vals
                ) mutable -> uint64_t {
                    for (size_t i = 0; i < idx_map.size(); ++i) {
                        original_vals[idx_map[i]] = reduced_vals[i];
                    }
                    uint64_t result = eval(original_vals);
                    for (size_t i = 0; i < idx_map.size(); ++i) {
                        original_vals[idx_map[i]] = 0;
                    }
                    return result;
                }
            );
        }

        // Evaluate the singleton polynomial S_i(t) at t=2.
        // Only degrees 1 and 2 contribute because
        // falling_factorial(2, k) = 0 for k >= 3.
        uint64_t EvalUnivariateAt2(const UnivariateNormalizedPoly &poly, uint32_t bitwidth) {
            const uint64_t mask = Bitmask(bitwidth);
            uint64_t sum        = 0;
            for (const auto &term : poly.terms) {
                if (term.degree >= 3) { break; }
                sum = (sum + (term.coeff * 2)) & mask;
            }
            return sum;
        }

    } // namespace

    // ---------------------------------------------------------------
    // RunResolveCompetition (existing)
    // ---------------------------------------------------------------

    Result< PassResult > RunResolveCompetition(const WorkItem &item, OrchestratorContext &ctx) {
        if (!std::holds_alternative< CompetitionResolvedPayload >(item.payload)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        const auto &resolved = std::get< CompetitionResolvedPayload >(item.payload);
        auto group_it        = ctx.competition_groups.find(resolved.group_id);
        assert(group_it != ctx.competition_groups.end());
        auto &group = group_it->second;

        static const ContinuationData kDefaultCont{ std::monostate{} };
        const ContinuationData &cont =
            group.continuation.has_value() ? *group.continuation : kDefaultCont;

        PassResult result = std::visit(
            [&](const auto &c) -> PassResult {
                using T = std::decay_t< decltype(c) >;
                if constexpr (std::is_same_v< T, std::monostate >) {
                    if (group.best.has_value()) {
                        auto &winner = *group.best;
                        WorkItem cand_item;
                        cand_item.payload = CandidatePayload{
                            .expr           = CloneExpr(*winner.expr),
                            .real_vars      = winner.real_vars,
                            .cost           = winner.cost,
                            .producing_pass = winner.source_pass,
                            .needs_original_space_verification =
                                winner.needs_original_space_verification,
                        };
                        cand_item.features              = item.features;
                        cand_item.metadata              = item.metadata;
                        cand_item.metadata.verification = winner.verification;
                        cand_item.metadata.sig_vector   = winner.sig_vector;
                        cand_item.depth                 = item.depth;
                        cand_item.rewrite_gen           = item.rewrite_gen;
                        cand_item.attempted_mask        = item.attempted_mask;
                        cand_item.history               = item.history;

                        PassResult pr;
                        pr.decision    = PassDecision::kSolvedCandidate;
                        pr.disposition = ItemDisposition::kConsumeCurrent;
                        pr.next.push_back(std::move(cand_item));
                        return pr;
                    }

                    ReasonDetail reason;
                    if (!group.technique_failures.empty()) {
                        reason = group.technique_failures.front();
                        for (size_t i = 1; i < group.technique_failures.size(); ++i) {
                            reason.causes.push_back(group.technique_failures[i].top);
                        }
                    } else {
                        reason.top.code = {
                            ReasonCategory::kSearchExhausted,
                            ReasonDomain::kOrchestrator,
                        };
                        reason.top.message = "Competition group resolved with no "
                                             "winner";
                    }
                    return PassResult{
                        .decision    = PassDecision::kBlocked,
                        .disposition = ItemDisposition::kConsumeCurrent,
                        .reason      = std::move(reason),
                    };
                } else {
                    return PassResult{
                        .decision = PassDecision::kBlocked,
                        .disposition =
                            ItemDisposition::kConsumeCurrent,
                        .reason =
                            ReasonDetail{
                                .top =
                                    {
                                        .code =
                                            {
                                                ReasonCategory::
                                                    kInapplicable,
                                                ReasonDomain::
                                                    kOrchestrator,
                                            },
                                        .message =
                                            "Continuation type not "
                                            "yet "
                                            "implemented",
                                    },
                            },
                    };
                }
            },
            cont
        );

        ctx.competition_groups.erase(group_it);
        return Ok(std::move(result));
    }

    // ---------------------------------------------------------------
    // kSignaturePatternMatch
    // ---------------------------------------------------------------

    Result< PassResult >
    RunSignaturePatternMatch(const WorkItem &item, OrchestratorContext &ctx) {
        if (!std::holds_alternative< SignatureStatePayload >(item.payload)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        const auto &sig_payload = std::get< SignatureStatePayload >(item.payload);
        const auto &sub_ctx     = sig_payload.ctx;
        const auto &sig         = sub_ctx.elimination.reduced_sig;
        const auto num_vars     = static_cast< uint32_t >(sub_ctx.real_vars.size());

        auto pm = MatchPattern(sig, num_vars, ctx.bitwidth);
        if (!pm) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNoProgress,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        // Full-width check if evaluator available
        auto mapped_eval = BuildMappedEvaluator(ctx, sub_ctx);
        bool accepted    = true;
        if (mapped_eval) {
            auto check = FullWidthCheckEval(*mapped_eval, num_vars, **pm, ctx.bitwidth);
            if (!check.passed) { accepted = false; }
        }

        if (!accepted) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNoProgress,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        auto vs = VerificationState::kVerified;
        if (ctx.opts.spot_check) {
            auto check = SignatureCheck(sig, **pm, num_vars, ctx.bitwidth);
            if (!check.passed) {
                return Ok(
                    PassResult{
                        .decision    = PassDecision::kNoProgress,
                        .disposition = ItemDisposition::kRetainCurrent,
                    }
                );
            }
        }

        auto cost_info = ComputeCost(**pm);

        assert(item.group_id.has_value());
        SubmitCandidate(
            ctx.competition_groups, *item.group_id,
            CandidateRecord{
                .expr                              = std::move(*pm),
                .cost                              = cost_info.cost,
                .verification                      = vs,
                .real_vars                         = sub_ctx.real_vars,
                .source_pass                       = PassId::kSignaturePatternMatch,
                .needs_original_space_verification = sub_ctx.needs_original_space_verification,
                .sig_vector                        = sub_ctx.elimination.reduced_sig,
            }
        );

        return Ok(
            PassResult{
                .decision    = PassDecision::kAdvance,
                .disposition = ItemDisposition::kRetainCurrent,
            }
        );
    }

    // ---------------------------------------------------------------
    // kSignatureAnf
    // ---------------------------------------------------------------

    Result< PassResult > RunSignatureAnf(const WorkItem &item, OrchestratorContext &ctx) {
        if (!std::holds_alternative< SignatureStatePayload >(item.payload)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        const auto &sig_payload = std::get< SignatureStatePayload >(item.payload);
        const auto &sub_ctx     = sig_payload.ctx;
        const auto &sig         = sub_ctx.elimination.reduced_sig;
        const auto num_vars     = static_cast< uint32_t >(sub_ctx.real_vars.size());

        if (!IsBooleanValued(sig) || num_vars > ctx.opts.max_vars) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNoProgress,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        auto anf      = ComputeAnf(sig, num_vars);
        auto anf_expr = BuildAnfExpr(anf, num_vars);

        // Spot check
        if (ctx.opts.spot_check) {
            auto spot = SignatureCheck(sig, *anf_expr, num_vars, ctx.bitwidth);
            if (!spot.passed) {
                return Ok(
                    PassResult{
                        .decision    = PassDecision::kNoProgress,
                        .disposition = ItemDisposition::kRetainCurrent,
                    }
                );
            }
        }

        // Full-width check
        auto mapped_eval = BuildMappedEvaluator(ctx, sub_ctx);
        if (mapped_eval) {
            auto fw = FullWidthCheckEval(*mapped_eval, num_vars, *anf_expr, ctx.bitwidth);
            if (!fw.passed) {
                return Ok(
                    PassResult{
                        .decision    = PassDecision::kNoProgress,
                        .disposition = ItemDisposition::kRetainCurrent,
                    }
                );
            }
        }

        auto cost_info = ComputeCost(*anf_expr);

        assert(item.group_id.has_value());
        SubmitCandidate(
            ctx.competition_groups, *item.group_id,
            CandidateRecord{
                .expr                              = std::move(anf_expr),
                .cost                              = cost_info.cost,
                .verification                      = VerificationState::kVerified,
                .real_vars                         = sub_ctx.real_vars,
                .source_pass                       = PassId::kSignatureAnf,
                .needs_original_space_verification = sub_ctx.needs_original_space_verification,
                .sig_vector                        = sub_ctx.elimination.reduced_sig,
            }
        );

        return Ok(
            PassResult{
                .decision    = PassDecision::kAdvance,
                .disposition = ItemDisposition::kRetainCurrent,
            }
        );
    }

    // ---------------------------------------------------------------
    // kPrepareCoeffModel
    // ---------------------------------------------------------------

    Result< PassResult > RunPrepareCoeffModel(const WorkItem &item, OrchestratorContext &ctx) {
        if (!std::holds_alternative< SignatureStatePayload >(item.payload)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        const auto &sig_payload = std::get< SignatureStatePayload >(item.payload);
        const auto &sub_ctx     = sig_payload.ctx;
        const auto &sig         = sub_ctx.elimination.reduced_sig;
        const auto num_vars     = static_cast< uint32_t >(sub_ctx.real_vars.size());

        auto coeffs = InterpolateCoefficients(sig, num_vars, ctx.bitwidth);

        assert(item.group_id.has_value());
        AcquireHandle(ctx.competition_groups, *item.group_id);

        WorkItem child;
        child.payload = SignatureCoeffStatePayload{
            .ctx    = sub_ctx,
            .coeffs = std::move(coeffs),
        };
        child.features                  = item.features;
        child.metadata                  = item.metadata;
        child.depth                     = item.depth;
        child.rewrite_gen               = item.rewrite_gen;
        child.attempted_mask            = 0;
        child.signature_recursion_depth = item.signature_recursion_depth;
        child.group_id                  = item.group_id;
        child.history                   = item.history;

        PassResult result;
        result.decision    = PassDecision::kAdvance;
        result.disposition = ItemDisposition::kRetainCurrent;
        result.next.push_back(std::move(child));
        return Ok(std::move(result));
    }

    // ---------------------------------------------------------------
    // kSignatureCobCandidate
    // ---------------------------------------------------------------

    Result< PassResult >
    RunSignatureCobCandidate(const WorkItem &item, OrchestratorContext &ctx) {
        if (!std::holds_alternative< SignatureCoeffStatePayload >(item.payload)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        const auto &coeff_payload = std::get< SignatureCoeffStatePayload >(item.payload);
        const auto &sub_ctx       = coeff_payload.ctx;
        const auto &sig           = sub_ctx.elimination.reduced_sig;
        const auto &coeffs        = coeff_payload.coeffs;
        const auto num_vars       = static_cast< uint32_t >(sub_ctx.real_vars.size());

        auto expr = BuildCobExpr(coeffs, num_vars, ctx.bitwidth);

        // Spot check
        auto vs = VerificationState::kUnverified;
        if (ctx.opts.spot_check) {
            auto spot = SignatureCheck(sig, *expr, num_vars, ctx.bitwidth);
            if (spot.passed) { vs = VerificationState::kVerified; }
        }

        // Full-width check — reject if evaluator proves mismatch
        auto mapped_eval = BuildMappedEvaluator(ctx, sub_ctx);
        if (mapped_eval) {
            auto fw = FullWidthCheckEval(*mapped_eval, num_vars, *expr, ctx.bitwidth);
            if (fw.passed) {
                vs = VerificationState::kVerified;
            } else {
                return Ok(
                    PassResult{
                        .decision    = PassDecision::kNoProgress,
                        .disposition = ItemDisposition::kRetainCurrent,
                        .reason =
                            ReasonDetail{
                                         .top = { .code    = { ReasonCategory::kVerifyFailed,
                                                      ReasonDomain::kSignature },
                                         .message = "CoB candidate failed full-width "
                                                    "check" },
                                         },
                }
                );
            }
        }

        auto cost_info = ComputeCost(*expr);

        assert(item.group_id.has_value());
        SubmitCandidate(
            ctx.competition_groups, *item.group_id,
            CandidateRecord{
                .expr                              = std::move(expr),
                .cost                              = cost_info.cost,
                .verification                      = vs,
                .real_vars                         = sub_ctx.real_vars,
                .source_pass                       = PassId::kSignatureCobCandidate,
                .needs_original_space_verification = sub_ctx.needs_original_space_verification,
                .sig_vector                        = sub_ctx.elimination.reduced_sig,
            }
        );

        return Ok(
            PassResult{
                .decision    = PassDecision::kAdvance,
                .disposition = ItemDisposition::kRetainCurrent,
            }
        );
    }

    // ---------------------------------------------------------------
    // kSignatureSingletonPolyRecovery
    // ---------------------------------------------------------------

    Result< PassResult >
    RunSignatureSingletonPolyRecovery(const WorkItem &item, OrchestratorContext &ctx) {
        if (!std::holds_alternative< SignatureCoeffStatePayload >(item.payload)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        const auto &coeff_payload = std::get< SignatureCoeffStatePayload >(item.payload);
        const auto &sub_ctx       = coeff_payload.ctx;
        const auto &coeffs        = coeff_payload.coeffs;
        const auto num_vars       = static_cast< uint32_t >(sub_ctx.real_vars.size());

        auto mapped_eval = BuildMappedEvaluator(ctx, sub_ctx);
        if (!mapped_eval) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNoProgress,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        // Singleton power recovery
        auto singleton_result = RecoverSingletonPowers(*mapped_eval, num_vars, ctx.bitwidth);

        std::vector< uint64_t > singleton_at_2;
        if (singleton_result.has_value()) {
            const auto &sr = singleton_result.value();
            singleton_at_2.resize(num_vars, 0);
            for (uint32_t i = 0; i < num_vars; ++i) {
                singleton_at_2[i] = EvalUnivariateAt2(sr.per_var[i], ctx.bitwidth);
            }
        }

        // Coefficient splitting
        auto split =
            SplitCoefficients(coeffs, *mapped_eval, num_vars, ctx.bitwidth, singleton_at_2);

        const bool has_mul =
            std::any_of(split.mul_coeffs.begin(), split.mul_coeffs.end(), [](uint64_t c) {
                return c != 0;
            });

        std::unique_ptr< Expr > poly_expr = nullptr;
        std::vector< uint64_t > residual  = split.and_coeffs;

        if (has_mul) {
            auto lowered = LowerArithmeticFragment(
                split.and_coeffs, split.mul_coeffs, static_cast< uint8_t >(num_vars),
                ctx.bitwidth
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

        auto bit_expr = BuildCobExpr(residual, num_vars, ctx.bitwidth);

        const bool bit_is_zero =
            bit_expr && bit_expr->kind == Expr::Kind::kConstant && bit_expr->constant_val == 0;

        std::unique_ptr< Expr > combined;
        if (bit_expr && !bit_is_zero) { combined = std::move(bit_expr); }
        if (poly_expr) {
            combined = combined ? Expr::Add(std::move(combined), std::move(poly_expr))
                                : std::move(poly_expr);
        }
        if (singleton_expr) {
            combined = combined ? Expr::Add(std::move(combined), std::move(singleton_expr))
                                : std::move(singleton_expr);
        }
        if (!combined) { combined = Expr::Constant(0); }

        auto fw_check = FullWidthCheckEval(*mapped_eval, num_vars, *combined, ctx.bitwidth);
        if (!fw_check.passed) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNoProgress,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        auto cost_info = ComputeCost(*combined);

        assert(item.group_id.has_value());
        SubmitCandidate(
            ctx.competition_groups, *item.group_id,
            CandidateRecord{
                .expr                              = std::move(combined),
                .cost                              = cost_info.cost,
                .verification                      = VerificationState::kVerified,
                .real_vars                         = sub_ctx.real_vars,
                .source_pass                       = PassId::kSignatureSingletonPolyRecovery,
                .needs_original_space_verification = sub_ctx.needs_original_space_verification,
                .sig_vector                        = sub_ctx.elimination.reduced_sig,
            }
        );

        return Ok(
            PassResult{
                .decision    = PassDecision::kAdvance,
                .disposition = ItemDisposition::kRetainCurrent,
            }
        );
    }

    // ---------------------------------------------------------------
    // kSignatureMultivarPolyRecovery
    // ---------------------------------------------------------------

    Result< PassResult >
    RunSignatureMultivarPolyRecovery(const WorkItem &item, OrchestratorContext &ctx) {
        if (!std::holds_alternative< SignatureStatePayload >(item.payload)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        const auto &sig_payload = std::get< SignatureStatePayload >(item.payload);
        const auto &sub_ctx     = sig_payload.ctx;
        const auto num_vars     = static_cast< uint32_t >(sub_ctx.real_vars.size());

        // Guard: need structural flags with multivar high power,
        // num_vars <= 6, and an evaluator
        bool has_multivar_flag = false;
        if (item.features.classification) {
            has_multivar_flag =
                HasFlag(item.features.classification->flags, kSfHasMultivarHighPower);
        }

        auto mapped_eval = BuildMappedEvaluator(ctx, sub_ctx);
        if (!has_multivar_flag || num_vars > 6 || !mapped_eval) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNoProgress,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        std::vector< uint32_t > support(num_vars);
        std::iota(support.begin(), support.end(), 0U);
        auto recovery = RecoverAndVerifyPoly(*mapped_eval, support, num_vars, ctx.bitwidth);

        if (!recovery.Succeeded()) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNoProgress,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        auto payload   = recovery.TakePayload();
        auto cost_info = ComputeCost(*payload.expr);

        assert(item.group_id.has_value());
        SubmitCandidate(
            ctx.competition_groups, *item.group_id,
            CandidateRecord{
                .expr                              = std::move(payload.expr),
                .cost                              = cost_info.cost,
                .verification                      = VerificationState::kVerified,
                .real_vars                         = sub_ctx.real_vars,
                .source_pass                       = PassId::kSignatureMultivarPolyRecovery,
                .needs_original_space_verification = sub_ctx.needs_original_space_verification,
                .sig_vector                        = sub_ctx.elimination.reduced_sig,
            }
        );

        return Ok(
            PassResult{
                .decision    = PassDecision::kAdvance,
                .disposition = ItemDisposition::kRetainCurrent,
            }
        );
    }

} // namespace cobra
