#include "SignaturePasses.h"
#include "CompetitionGroup.h"
#include "ContinuationTypes.h"
#include "JoinState.h"
#include "OrchestratorPasses.h"
#include "cobra/core/AnfTransform.h"
#include "cobra/core/ArithmeticLowering.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/BitwiseDecomposer.h"
#include "cobra/core/Classification.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/CoBExprBuilder.h"
#include "cobra/core/CoeffInterpolator.h"
#include "cobra/core/CoefficientSplitter.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/HybridDecomposer.h"
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
#include <random>
#include <utility>
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

        // Resolve a child competition group whose continuation is
        // BitwiseComposeCont: remap winner, compose with split var,
        // FW-verify, submit to parent group, release parent handle.
        // Build a mapped evaluator for verification in the parent's
        // reduced variable space.
        std::optional< Evaluator > BuildParentMappedEvaluator(
            const OrchestratorContext &ctx,
            const std::vector< uint32_t > &parent_original_indices, uint32_t parent_num_vars
        ) {
            if (!ctx.evaluator) { return std::nullopt; }
            if (parent_num_vars == ctx.original_vars.size()) { return *ctx.evaluator; }
            return Evaluator(
                [eval = *ctx.evaluator, idx_map = parent_original_indices,
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

        PassResult ResolveBitwiseCompose(
            const BitwiseComposeCont &cont, CompetitionGroup &group, const WorkItem &item,
            OrchestratorContext &ctx
        ) {
            PassResult pr;
            pr.disposition = ItemDisposition::kConsumeCurrent;

            if (group.best.has_value()) {
                auto &winner  = *group.best;
                auto remapped = RemapVars(*winner.expr, cont.active_context_indices);
                auto composed =
                    Compose(cont.gate, cont.var_k, std::move(remapped), cont.add_coeff);

                // FW-verify in parent's reduced variable space
                auto mapped_eval = BuildParentMappedEvaluator(
                    ctx, cont.parent_original_indices, cont.parent_num_vars
                );
                bool fw_ok = true;
                if (mapped_eval) {
                    auto check = FullWidthCheckEval(
                        *mapped_eval, cont.parent_num_vars, *composed, ctx.bitwidth
                    );
                    fw_ok = check.passed;
                }

                if (fw_ok) {
                    auto cost_info = ComputeCost(*composed);
                    bool needs_osv = !cont.parent_original_indices.empty()
                        && cont.parent_num_vars != ctx.original_vars.size();
                    SubmitCandidate(
                        ctx.competition_groups, cont.parent_group_id,
                        CandidateRecord{
                            .expr         = std::move(composed),
                            .cost         = cost_info.cost,
                            .verification = mapped_eval ? VerificationState::kVerified
                                                        : VerificationState::kUnverified,
                            .real_vars    = cont.parent_real_vars,
                            .source_pass  = PassId::kSignatureBitwiseDecompose,
                            .needs_original_space_verification = needs_osv,
                        }
                    );
                }
            }

            // Fire-and-forget: no parent handle to release.
            // The parent group resolves when its original handle
            // is released by the sig item exhausting all passes.
            pr.decision = PassDecision::kAdvance;
            return pr;
        }

        PassResult ResolveHybridCompose(
            const HybridComposeCont &cont, CompetitionGroup &group, const WorkItem &item,
            OrchestratorContext &ctx
        ) {
            PassResult pr;
            pr.disposition = ItemDisposition::kConsumeCurrent;

            if (group.best.has_value()) {
                auto &winner  = *group.best;
                auto composed = ComposeExtraction(cont.op, cont.var_k, CloneExpr(*winner.expr));

                // FW-verify in parent's reduced variable space
                auto mapped_eval = BuildParentMappedEvaluator(
                    ctx, cont.parent_original_indices, cont.parent_num_vars
                );
                bool fw_ok = true;
                if (mapped_eval) {
                    auto check = FullWidthCheckEval(
                        *mapped_eval, cont.parent_num_vars, *composed, ctx.bitwidth
                    );
                    fw_ok = check.passed;
                }

                if (fw_ok) {
                    auto cost_info = ComputeCost(*composed);
                    bool needs_osv = !cont.parent_original_indices.empty()
                        && cont.parent_num_vars != ctx.original_vars.size();
                    SubmitCandidate(
                        ctx.competition_groups, cont.parent_group_id,
                        CandidateRecord{
                            .expr         = std::move(composed),
                            .cost         = cost_info.cost,
                            .verification = mapped_eval ? VerificationState::kVerified
                                                        : VerificationState::kUnverified,
                            .real_vars    = cont.parent_real_vars,
                            .source_pass  = PassId::kSignatureHybridDecompose,
                            .needs_original_space_verification = needs_osv,
                        }
                    );
                }
            }

            pr.decision = PassDecision::kAdvance;
            return pr;
        }

        PassResult ResolveOperandRewrite(
            const OperandRewriteCont &cont, CompetitionGroup &group, const WorkItem &item,
            OrchestratorContext &ctx
        ) {
            PassResult pr;
            pr.disposition = ItemDisposition::kConsumeCurrent;
            pr.decision    = PassDecision::kAdvance;

            auto join_it = ctx.join_states.find(cont.join_id);
            if (join_it == ctx.join_states.end()) { return pr; }
            auto *join = std::get_if< OperandJoinState >(&join_it->second);
            if (join == nullptr) { return pr; }

            if (cont.role == OperandRewriteCont::OperandRole::kLhs) {
                join->lhs_resolved = true;
                if (group.best.has_value()) {
                    join->lhs_winner = CandidateRecord{
                        .expr        = CloneExpr(*group.best->expr),
                        .cost        = group.best->cost,
                        .source_pass = group.best->source_pass,
                    };
                }
            } else {
                join->rhs_resolved = true;
                if (group.best.has_value()) {
                    join->rhs_winner = CandidateRecord{
                        .expr        = CloneExpr(*group.best->expr),
                        .cost        = group.best->cost,
                        .source_pass = group.best->source_pass,
                    };
                }
            }

            if (!join->lhs_resolved || !join->rhs_resolved) { return pr; }

            // Both resolved. Build up to 3 candidates.
            const auto &orig       = *join->original_mul;
            auto baseline          = join->baseline_cost;
            const auto bw          = join->bitwidth;
            const auto num_vars    = static_cast< uint32_t >(join->vars.size());
            const uint64_t fw_mask = Bitmask(bw);

            struct Candidate
            {
                std::unique_ptr< Expr > expr;
                ExprCost cost;
            };

            std::optional< Candidate > best;

            auto try_cand = [&](std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs) {
                auto mul = Expr::Mul(std::move(lhs), std::move(rhs));
                auto c   = ComputeCost(*mul).cost;
                if (!IsBetter(c, baseline)) { return; }
                if (best.has_value() && !IsBetter(c, best->cost)) { return; }

                // 8-probe FW check matching OperandSimplifier lines 166-173
                std::mt19937_64 rng(0xCA5E + num_vars);
                constexpr int kProbes = 8;
                std::vector< uint64_t > pt(num_vars);
                for (int p = 0; p < kProbes; ++p) {
                    for (uint32_t vi = 0; vi < num_vars; ++vi) { pt[vi] = rng() & fw_mask; }
                    uint64_t orig_val = EvalExpr(orig, pt, bw) & fw_mask;
                    uint64_t simp_val = EvalExpr(*mul, pt, bw) & fw_mask;
                    if (orig_val != simp_val) { return; }
                }

                best = Candidate{ .expr = std::move(mul), .cost = c };
            };

            if (join->lhs_winner.has_value()) {
                try_cand(CloneExpr(*join->lhs_winner->expr), CloneExpr(*orig.children[1]));
            }
            if (join->rhs_winner.has_value()) {
                try_cand(CloneExpr(*orig.children[0]), CloneExpr(*join->rhs_winner->expr));
            }
            if (join->lhs_winner.has_value() && join->rhs_winner.has_value()) {
                try_cand(
                    CloneExpr(*join->lhs_winner->expr), CloneExpr(*join->rhs_winner->expr)
                );
            }

            if (best.has_value()) {
                // Splice the replacement Mul into the full AST.
                auto rebuilt_ast = CloneExpr(*join->full_ast);
                bool replaced    = false;
                rebuilt_ast      = ReplaceByHash(
                    std::move(rebuilt_ast), join->target_hash, best->expr, replaced
                );

                auto new_cls = ClassifyStructural(*rebuilt_ast);

                WorkItem rewritten;
                rewritten.payload = AstPayload{
                    .expr           = std::move(rebuilt_ast),
                    .classification = new_cls,
                    .provenance     = Provenance::kRewritten,
                };
                rewritten.features                = item.features;
                rewritten.features.classification = new_cls;
                rewritten.features.provenance     = Provenance::kRewritten;
                rewritten.metadata                = item.metadata;
                rewritten.depth                   = item.depth;
                rewritten.rewrite_gen             = join->rewrite_gen + 1;
                rewritten.attempted_mask          = 0;
                rewritten.history                 = item.history;

                pr.next.push_back(std::move(rewritten));
            }

            ctx.join_states.erase(join_it);
            return pr;
        }

        PassResult ResolveProductCollapse(
            const ProductCollapseCont &cont, CompetitionGroup &group, const WorkItem &item,
            OrchestratorContext &ctx
        ) {
            PassResult pr;
            pr.disposition = ItemDisposition::kConsumeCurrent;
            pr.decision    = PassDecision::kAdvance;

            auto join_it = ctx.join_states.find(cont.join_id);
            if (join_it == ctx.join_states.end()) { return pr; }
            auto *join = std::get_if< ProductJoinState >(&join_it->second);
            if (join == nullptr) { return pr; }

            if (cont.role == ProductCollapseCont::FactorRole::kX) {
                join->x_resolved = true;
                if (group.best.has_value()) {
                    join->x_winner = CandidateRecord{
                        .expr        = CloneExpr(*group.best->expr),
                        .cost        = group.best->cost,
                        .source_pass = group.best->source_pass,
                    };
                }
            } else {
                join->y_resolved = true;
                if (group.best.has_value()) {
                    join->y_winner = CandidateRecord{
                        .expr        = CloneExpr(*group.best->expr),
                        .cost        = group.best->cost,
                        .source_pass = group.best->source_pass,
                    };
                }
            }

            if (!join->x_resolved || !join->y_resolved) { return pr; }

            // Both resolved. Product collapse requires BOTH factors.
            if (join->x_winner.has_value() && join->y_winner.has_value()) {
                auto candidate = Expr::Mul(
                    CloneExpr(*join->x_winner->expr), CloneExpr(*join->y_winner->expr)
                );

                const auto &orig    = *join->original_expr;
                const auto bw       = join->bitwidth;
                const auto num_vars = static_cast< uint32_t >(join->vars.size());

                auto check = FullWidthCheck(orig, num_vars, *candidate, {}, bw);
                if (check.passed) {
                    auto cand_cost = ComputeCost(*candidate).cost;
                    if (IsBetter(cand_cost, join->baseline_cost)) {
                        auto new_cls = ClassifyStructural(*candidate);

                        WorkItem rewritten;
                        rewritten.payload = AstPayload{
                            .expr           = std::move(candidate),
                            .classification = new_cls,
                            .provenance     = Provenance::kRewritten,
                        };
                        rewritten.features                = item.features;
                        rewritten.features.classification = new_cls;
                        rewritten.features.provenance     = Provenance::kRewritten;
                        rewritten.metadata                = item.metadata;
                        rewritten.depth                   = item.depth;
                        rewritten.rewrite_gen             = join->rewrite_gen + 1;
                        rewritten.attempted_mask          = 0;
                        rewritten.history                 = item.history;

                        pr.next.push_back(std::move(rewritten));
                    }
                }
            }

            ctx.join_states.erase(join_it);
            return pr;
        }

        ExtractorKind ProjectExtractorKind(ResidualOrigin origin) {
            switch (origin) {
                case ResidualOrigin::kDirectBooleanNull:
                    return ExtractorKind::kBooleanNullDirect;
                case ResidualOrigin::kProductCore:
                    return ExtractorKind::kProductAST;
                case ResidualOrigin::kPolynomialCore:
                    return ExtractorKind::kPolynomial;
                case ResidualOrigin::kTemplateCore:
                    return ExtractorKind::kTemplate;
            }
            return ExtractorKind::kBooleanNullDirect;
        }

        PassResult ResolveResidualRecombine(
            const ResidualRecombineCont &cont, CompetitionGroup &group, const WorkItem &item,
            OrchestratorContext &ctx
        ) {
            PassResult pr;
            pr.disposition = ItemDisposition::kConsumeCurrent;
            pr.decision    = PassDecision::kAdvance;

            if (!group.best.has_value()) { return pr; }
            if (!ctx.evaluator) { return pr; }

            auto solved = CloneExpr(*group.best->expr);

            // Remap winner from reduced variable space to original.
            // The winner's real_vars may be a subset of original_vars
            // when aux-var elimination reduced the variable set.
            if (!cont.residual_support.empty()
                && group.best->real_vars.size() < ctx.original_vars.size())
            {
                RemapVarIndices(*solved, cont.residual_support);
            }

            // Strengthened 64-probe FW check of solved expr against
            // residual_eval, matching RunResidualSupported's check.
            const auto num_vars = static_cast< uint32_t >(ctx.original_vars.size());
            auto res_check =
                FullWidthCheckEval(cont.residual_eval, num_vars, *solved, ctx.bitwidth, 64);
            if (!res_check.passed) { return pr; }

            // Recombine: core_expr + solved_residual, or just solved
            // for direct boolean-null (core_expr is null).
            auto combined = cont.core_expr
                ? Expr::Add(CloneExpr(*cont.core_expr), std::move(solved))
                : std::move(solved);

            // Verify the recombined expression against the original
            // evaluator.
            auto orig_check =
                FullWidthCheckEval(*ctx.evaluator, num_vars, *combined, ctx.bitwidth);
            if (!orig_check.passed) { return pr; }

            auto cost_info = ComputeCost(*combined);

            if (cont.parent_group_id.has_value()) {
                SubmitCandidate(
                    ctx.competition_groups, *cont.parent_group_id,
                    CandidateRecord{
                        .expr                              = std::move(combined),
                        .cost                              = cost_info.cost,
                        .verification                      = VerificationState::kVerified,
                        .real_vars                         = ctx.original_vars,
                        .source_pass                       = PassId::kResidualSupported,
                        .needs_original_space_verification = false,
                    }
                );
            } else {
                // No parent group — emit as a candidate directly.
                WorkItem cand_item;
                cand_item.payload = CandidatePayload{
                    .expr                              = std::move(combined),
                    .real_vars                         = ctx.original_vars,
                    .cost                              = cost_info.cost,
                    .producing_pass                    = PassId::kResidualSupported,
                    .needs_original_space_verification = false,
                };
                cand_item.features                    = item.features;
                cand_item.metadata                    = item.metadata;
                cand_item.metadata.verification       = VerificationState::kVerified;
                cand_item.metadata.sig_vector         = cont.source_sig;
                cand_item.metadata.decomposition_meta = DecompositionMeta{
                    .extractor_kind = static_cast< uint8_t >(ProjectExtractorKind(cont.origin)),
                    .solver_kind =
                        static_cast< uint8_t >(ResidualSolverKind::kSupportedPipeline),
                    .has_solver  = true,
                    .core_degree = cont.core_degree,
                };
                cand_item.depth          = item.depth;
                cand_item.rewrite_gen    = item.rewrite_gen;
                cand_item.attempted_mask = item.attempted_mask;
                cand_item.history        = item.history;

                pr.decision = PassDecision::kSolvedCandidate;
                pr.next.push_back(std::move(cand_item));
            }

            return pr;
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
                } else if constexpr (std::is_same_v< T, BitwiseComposeCont >) {
                    return ResolveBitwiseCompose(c, group, item, ctx);
                } else if constexpr (std::is_same_v< T, HybridComposeCont >) {
                    return ResolveHybridCompose(c, group, item, ctx);
                } else if constexpr (std::is_same_v< T, OperandRewriteCont >) {
                    return ResolveOperandRewrite(c, group, item, ctx);
                } else if constexpr (std::is_same_v< T, ProductCollapseCont >) {
                    return ResolveProductCollapse(c, group, item, ctx);
                } else if constexpr (std::is_same_v< T, ResidualRecombineCont >) {
                    return ResolveResidualRecombine(c, group, item, ctx);
                } else {
                    return PassResult{
                        .decision    = PassDecision::kBlocked,
                        .disposition = ItemDisposition::kConsumeCurrent,
                        .reason =
                            ReasonDetail{
                                         .top = { .code    = { ReasonCategory::kInapplicable,
                                                      ReasonDomain::kOrchestrator },
                                         .message = "Continuation type not yet implemented" },
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

    // ---------------------------------------------------------------
    // kSignatureBitwiseDecompose — fanout pass
    // ---------------------------------------------------------------

    Result< PassResult >
    RunSignatureBitwiseDecompose(const WorkItem &item, OrchestratorContext &ctx) {
        if (!std::holds_alternative< SignatureStatePayload >(item.payload)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        // Skip if the competition group already has a candidate from
        // a direct technique pass (pattern match, ANF, CoB, etc.).
        if (item.group_id.has_value()) {
            auto git = ctx.competition_groups.find(*item.group_id);
            if (git != ctx.competition_groups.end() && git->second.best.has_value()) {
                return Ok(
                    PassResult{
                        .decision    = PassDecision::kNoProgress,
                        .disposition = ItemDisposition::kRetainCurrent,
                    }
                );
            }
        }

        if (item.signature_recursion_depth >= 2) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNoProgress,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        if (!ctx.evaluator) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNoProgress,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        const auto &sig_payload = std::get< SignatureStatePayload >(item.payload);
        const auto &sub_ctx     = sig_payload.ctx;
        const auto &sig         = sub_ctx.elimination.reduced_sig;
        const auto num_vars     = static_cast< uint32_t >(sub_ctx.real_vars.size());

        if (sig.size() < 2 || num_vars > 6) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNoProgress,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        auto candidates = EnumerateBitwiseCandidates(sig, num_vars);
        if (candidates.empty()) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNoProgress,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        constexpr size_t kMaxCandidates = 8;
        if (candidates.size() > kMaxCandidates) { candidates.resize(kMaxCandidates); }

        assert(item.group_id.has_value());
        const auto parent_group_id = *item.group_id;

        PassResult result;
        result.decision    = PassDecision::kAdvance;
        result.disposition = ItemDisposition::kRetainCurrent;

        for (const auto &cand : candidates) {
            std::vector< uint32_t > g_context_indices;
            for (uint32_t v = 0; v < num_vars; ++v) {
                if (v == cand.var_k) { continue; }
                g_context_indices.push_back(v);
            }

            const auto n_g = static_cast< uint32_t >(g_context_indices.size());
            auto [compacted_sig, active_var_indices] = CompactSignature(cand.g_sig, n_g);

            std::vector< uint32_t > active_context_indices;
            for (const uint32_t ai : active_var_indices) {
                active_context_indices.push_back(g_context_indices[ai]);
            }

            // Handle constant g_sig inline (no child needed)
            if (active_var_indices.empty()) {
                auto g_expr = Expr::Constant(cand.g_sig[0]);
                auto composed =
                    Compose(cand.gate, cand.var_k, std::move(g_expr), cand.add_coeff);

                auto fw = FullWidthCheckEval(
                    *ctx.evaluator, static_cast< uint32_t >(ctx.original_vars.size()),
                    *composed, ctx.bitwidth
                );
                if (!fw.passed) { continue; }

                auto cost_info = ComputeCost(*composed);
                SubmitCandidate(
                    ctx.competition_groups, parent_group_id,
                    CandidateRecord{
                        .expr                              = std::move(composed),
                        .cost                              = cost_info.cost,
                        .verification                      = VerificationState::kVerified,
                        .real_vars                         = ctx.original_vars,
                        .source_pass                       = PassId::kSignatureBitwiseDecompose,
                        .needs_original_space_verification = false,
                    }
                );
                continue;
            }

            // Build active-variable context for the child
            std::vector< std::string > active_vars;
            std::vector< uint32_t > child_original_indices;
            for (const uint32_t ai : active_var_indices) {
                active_vars.push_back(sub_ctx.real_vars[g_context_indices[ai]]);
                child_original_indices.push_back(
                    sub_ctx.original_indices[g_context_indices[ai]]
                );
            }

            // Build sub-evaluator for the compacted child
            uint64_t fixed_val = 0;
            if (cand.gate == GateKind::kAnd) {
                fixed_val = (ctx.bitwidth == 64) ? UINT64_MAX : ((1ULL << ctx.bitwidth) - 1);
            } else if (cand.gate == GateKind::kMul) {
                fixed_val = 1;
            }

            std::vector< uint32_t > compact_to_parent;
            compact_to_parent.reserve(active_var_indices.size());
            for (const uint32_t ai : active_var_indices) {
                const uint32_t parent_idx = (g_context_indices[ai] < cand.var_k)
                    ? g_context_indices[ai]
                    : (g_context_indices[ai] + 1);
                compact_to_parent.push_back(
                    sub_ctx.original_indices
                        [parent_idx < num_vars ? parent_idx : g_context_indices[ai]]
                );
            }

            // Create child competition group with continuation
            auto child_group_id = CreateGroup(ctx.competition_groups, ctx.next_group_id);
            ctx.competition_groups.at(child_group_id).continuation = ContinuationData{
                BitwiseComposeCont{
                                   .var_k                   = cand.var_k,
                                   .gate                    = cand.gate,
                                   .add_coeff               = cand.add_coeff,
                                   .active_context_indices  = active_context_indices,
                                   .parent_group_id         = parent_group_id,
                                   .parent_real_vars        = sub_ctx.real_vars,
                                   .parent_original_indices = sub_ctx.original_indices,
                                   .parent_num_vars         = num_vars,
                                   }
            };

            // Build child elimination result for the compacted sig
            EliminationResult child_elim;
            child_elim.reduced_sig = compacted_sig;
            child_elim.real_vars   = active_vars;

            WorkItem child;
            child.payload = SignatureStatePayload{
                .ctx = {
                    .sig                               = compacted_sig,
                    .real_vars                         = active_vars,
                    .elimination                       = std::move(child_elim),
                    .original_indices                  = child_original_indices,
                    .needs_original_space_verification = false,
                },
            };
            child.features                  = item.features;
            child.metadata                  = item.metadata;
            child.depth                     = item.depth;
            child.rewrite_gen               = item.rewrite_gen;
            child.attempted_mask            = 0;
            child.signature_recursion_depth = item.signature_recursion_depth + 1;
            child.group_id                  = child_group_id;
            child.history                   = item.history;

            result.next.push_back(std::move(child));
        }

        return Ok(std::move(result));
    }

    // ---------------------------------------------------------------
    // kSignatureHybridDecompose — fanout pass
    // ---------------------------------------------------------------

    Result< PassResult >
    RunSignatureHybridDecompose(const WorkItem &item, OrchestratorContext &ctx) {
        if (!std::holds_alternative< SignatureStatePayload >(item.payload)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        // Skip if the competition group already has a candidate.
        if (item.group_id.has_value()) {
            auto git = ctx.competition_groups.find(*item.group_id);
            if (git != ctx.competition_groups.end() && git->second.best.has_value()) {
                return Ok(
                    PassResult{
                        .decision    = PassDecision::kNoProgress,
                        .disposition = ItemDisposition::kRetainCurrent,
                    }
                );
            }
        }

        if (item.signature_recursion_depth >= 1) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNoProgress,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        if (!ctx.evaluator) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNoProgress,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        const auto &sig_payload = std::get< SignatureStatePayload >(item.payload);
        const auto &sub_ctx     = sig_payload.ctx;
        const auto &sig         = sub_ctx.elimination.reduced_sig;
        const auto num_vars     = static_cast< uint32_t >(sub_ctx.real_vars.size());

        if (sig.size() < 2 || num_vars > 6) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNoProgress,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        auto candidates = EnumerateHybridCandidates(sig, num_vars);
        if (candidates.empty()) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNoProgress,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        constexpr size_t kMaxCandidates = 8;
        if (candidates.size() > kMaxCandidates) { candidates.resize(kMaxCandidates); }

        assert(item.group_id.has_value());
        const auto parent_group_id = *item.group_id;

        PassResult result;
        result.decision    = PassDecision::kAdvance;
        result.disposition = ItemDisposition::kRetainCurrent;

        for (const auto &cand : candidates) {
            // Create child competition group with continuation
            auto child_group_id = CreateGroup(ctx.competition_groups, ctx.next_group_id);
            ctx.competition_groups.at(child_group_id).continuation = ContinuationData{
                HybridComposeCont{
                                  .var_k                   = cand.var_k,
                                  .op                      = cand.op,
                                  .parent_group_id         = parent_group_id,
                                  .parent_real_vars        = sub_ctx.real_vars,
                                  .parent_original_indices = sub_ctx.original_indices,
                                  .parent_num_vars         = num_vars,
                                  }
            };

            // Hybrid uses full variable space (no compaction)
            WorkItem child;
            child.payload = SignatureStatePayload{
                .ctx = {
                    .sig                               = cand.r_sig,
                    .real_vars                         = sub_ctx.real_vars,
                    .elimination = {
                        .reduced_sig = cand.r_sig,
                        .real_vars   = sub_ctx.real_vars,
                    },
                    .original_indices                  = sub_ctx.original_indices,
                    .needs_original_space_verification = false,
                },
            };
            child.features                  = item.features;
            child.metadata                  = item.metadata;
            child.depth                     = item.depth;
            child.rewrite_gen               = item.rewrite_gen;
            child.attempted_mask            = 0;
            child.signature_recursion_depth = item.signature_recursion_depth + 1;
            child.group_id                  = child_group_id;
            child.history                   = item.history;

            result.next.push_back(std::move(child));
        }

        return Ok(std::move(result));
    }

} // namespace cobra
