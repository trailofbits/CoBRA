#include "SignaturePasses.h"
#include "CompetitionGroup.h"
#include "ContinuationTypes.h"
#include "JoinState.h"
#include "OrchestratorPasses.h"
#include "cobra/core/AnfTransform.h"
#include "cobra/core/ArithmeticLowering.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/BitwiseDecomposer.h"
#include "cobra/core/Classification.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/CoBExprBuilder.h"
#include "cobra/core/CoeffInterpolator.h"
#include "cobra/core/CoefficientSplitter.h"
#include "cobra/core/DecompositionEngine.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/HybridDecomposer.h"
#include "cobra/core/MultivarPolyRecovery.h"
#include "cobra/core/PatternMatcher.h"
#include "cobra/core/PolyExprBuilder.h"
#include "cobra/core/PolyNormalizer.h"
#include "cobra/core/Profile.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
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
#include <ranges>
#include <utility>
#include <vector>

namespace cobra {

    namespace {

        // Build a mapped evaluator for the reduced variable space.
        // Prefers item-level evaluator_override (set for residual
        // children), falling back to the run-global ctx.evaluator.
        // When the reduced space matches the evaluator's space,
        // returns the evaluator directly; otherwise builds a
        // remapping lambda.
        std::optional< Evaluator > BuildMappedEvaluator(
            const OrchestratorContext &ctx, const SignatureSubproblemContext &sub_ctx,
            const WorkItem &item
        ) {
            // Use item-level evaluator override if present (residual
            // or lifted-outer children).  The override's arity may
            // exceed the reduced var count when aux-var elimination
            // removed variables, so we must remap through a buffer
            // sized to the full override arity.
            if (item.evaluator_override) {
                const auto arity        = item.evaluator_override_arity;
                const bool identity_map = sub_ctx.original_indices.size() == arity
                    && std::ranges::equal(sub_ctx.original_indices,
                                          std::views::iota(uint32_t{ 0 }, arity));
                if (identity_map) { return *item.evaluator_override; }

                return item.evaluator_override->Remap(
                    sub_ctx.original_indices, arity, EvaluatorTraceKind::kMappedOverride
                );
            }
            // Fall back to run-global evaluator.
            if (!ctx.evaluator) { return std::nullopt; }
            if (sub_ctx.real_vars.size() == ctx.original_vars.size()) { return *ctx.evaluator; }
            return ctx.evaluator->Remap(
                sub_ctx.original_indices, static_cast< uint32_t >(ctx.original_vars.size()),
                EvaluatorTraceKind::kMappedGlobal
            );
        }

        template< typename JoinT >
        std::optional< AstSolveContext > RebuildSolveContext(const JoinT &join) {
            if (!join.has_solve_ctx) { return std::nullopt; }
            return AstSolveContext{
                .vars      = join.solve_ctx_vars,
                .evaluator = join.solve_ctx_evaluator,
                .input_sig = join.solve_ctx_input_sig,
            };
        }

        template< typename JoinT >
        void EmitJoinRewrite(
            const JoinT &join, const WorkItem &item, std::unique_ptr< Expr > replacement,
            PassResult &pr
        ) {
            auto rebuilt_ast = CloneExpr(*join.full_ast);
            bool replaced    = false;
            rebuilt_ast =
                ReplaceByHash(std::move(rebuilt_ast), join.target_hash, replacement, replaced);

            auto new_cls = ClassifyStructural(*rebuilt_ast);

            WorkItem rewritten;
            rewritten.payload = AstPayload{
                .expr           = std::move(rebuilt_ast),
                .classification = new_cls,
                .provenance     = Provenance::kRewritten,
                .solve_ctx      = RebuildSolveContext(join),
            };
            rewritten.features                = item.features;
            rewritten.features.classification = new_cls;
            rewritten.features.provenance     = Provenance::kRewritten;
            rewritten.metadata                = item.metadata;
            rewritten.depth                   = join.parent_depth;
            rewritten.rewrite_gen             = join.rewrite_gen + 1;
            rewritten.attempted_mask          = 0;
            rewritten.group_id                = join.parent_group_id;
            rewritten.history                 = join.parent_history;

            pr.next.push_back(std::move(rewritten));
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

                bool fw_ok = true;
                if (cont.parent_eval) {
                    auto check = FullWidthCheckEval(
                        *cont.parent_eval, cont.parent_num_vars, *composed, ctx.bitwidth
                    );
                    fw_ok = check.passed;
                }

                if (fw_ok) {
                    auto cost_info = ComputeCost(*composed);
                    SubmitCandidate(
                        ctx.competition_groups, cont.parent_group_id,
                        CandidateRecord{
                            .expr         = std::move(composed),
                            .cost         = cost_info.cost,
                            .verification = cont.parent_eval ? VerificationState::kVerified
                                                             : VerificationState::kUnverified,
                            .real_vars    = cont.parent_real_vars,
                            .source_pass  = PassId::kSignatureBitwiseDecompose,
                            .needs_original_space_verification =
                                cont.parent_needs_original_space_verification,
                        }
                    );
                }
            }

            // Release parent handle — may trigger parent resolution.
            auto parent_resolved = ReleaseHandle(ctx.competition_groups, cont.parent_group_id);
            if (parent_resolved.has_value()) { pr.next.push_back(std::move(*parent_resolved)); }

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

                bool fw_ok = true;
                if (cont.parent_eval) {
                    auto check = FullWidthCheckEval(
                        *cont.parent_eval, cont.parent_num_vars, *composed, ctx.bitwidth
                    );
                    fw_ok = check.passed;
                }

                if (fw_ok) {
                    auto cost_info = ComputeCost(*composed);
                    SubmitCandidate(
                        ctx.competition_groups, cont.parent_group_id,
                        CandidateRecord{
                            .expr         = std::move(composed),
                            .cost         = cost_info.cost,
                            .verification = cont.parent_eval ? VerificationState::kVerified
                                                             : VerificationState::kUnverified,
                            .real_vars    = cont.parent_real_vars,
                            .source_pass  = PassId::kSignatureHybridDecompose,
                            .needs_original_space_verification =
                                cont.parent_needs_original_space_verification,
                        }
                    );
                }
            }

            // Release parent handle — may trigger parent resolution.
            auto parent_resolved = ReleaseHandle(ctx.competition_groups, cont.parent_group_id);
            if (parent_resolved.has_value()) { pr.next.push_back(std::move(*parent_resolved)); }

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
                EmitJoinRewrite(*join, item, std::move(best->expr), pr);
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
                        EmitJoinRewrite(*join, item, std::move(candidate), pr);
                    }
                }
            }

            ctx.join_states.erase(join_it);
            return pr;
        }

        std::unique_ptr< Expr > SubstituteBindings(
            const Expr &expr, const std::vector< LiftedBinding > &bindings,
            uint32_t original_var_count
        ) {
            if (expr.kind == Expr::Kind::kVariable && expr.var_index >= original_var_count) {
                for (const auto &b : bindings) {
                    if (b.outer_var_index == expr.var_index) { return CloneExpr(*b.subtree); }
                }
                return Expr::Variable(expr.var_index);
            }
            auto result = CloneExpr(expr);
            for (auto &child : result->children) {
                child = SubstituteBindings(*child, bindings, original_var_count);
            }
            return result;
        }

        PassResult ResolveLiftedSubstitute(
            const LiftedSubstituteCont &cont, CompetitionGroup &group, const WorkItem &item,
            OrchestratorContext &ctx
        ) {
            PassResult pr;
            pr.disposition = ItemDisposition::kConsumeCurrent;
            pr.decision    = PassDecision::kAdvance;

            if (!group.best.has_value()) {
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
                    reason.top.message = "Lifted substitute: no winner in group";
                }
                pr.decision = PassDecision::kBlocked;
                pr.reason   = std::move(reason);
                return pr;
            }

            auto &winner = *group.best;

            // Step 1: Remap from reduced outer space back to
            // full outer space.  winner.real_vars names the
            // variables that survived aux-var elimination;
            // cont.outer_vars is the full outer variable list.
            auto remapped = CloneExpr(*winner.expr);
            if (winner.real_vars.size() < cont.outer_vars.size()) {
                auto remap = BuildVarSupport(cont.outer_vars, winner.real_vars);
                // remap maps reduced_idx -> outer_idx
                // We need the inverse: for each reduced var at
                // position i, set its index to remap[i].
                RemapVarIndices(*remapped, remap);
            }

            // Step 2: Substitute lifted bindings (virtual vars
            // → original subtrees) in the full outer space.
            auto substituted =
                SubstituteBindings(*remapped, cont.bindings, cont.original_var_count);

            // Full-width verify against original evaluator.
            const auto num_vars = static_cast< uint32_t >(cont.original_vars.size());
            auto check =
                FullWidthCheckEval(cont.original_eval, num_vars, *substituted, ctx.bitwidth);
            if (!check.passed) {
                pr.decision = PassDecision::kBlocked;
                pr.reason   = ReasonDetail{
                    .top = {
                        .code = { ReasonCategory::kVerifyFailed,
                                  ReasonDomain::kOrchestrator },
                        .message = "Lifted substitute failed "
                                   "full-width verification",
                    },
                };
                return pr;
            }

            auto cost_info = ComputeCost(*substituted);

            WorkItem cand_item;
            cand_item.payload = CandidatePayload{
                .expr                              = std::move(substituted),
                .real_vars                         = cont.original_vars,
                .cost                              = cost_info.cost,
                .producing_pass                    = winner.source_pass,
                .needs_original_space_verification = false,
            };
            cand_item.features              = item.features;
            cand_item.metadata              = item.metadata;
            cand_item.metadata.verification = VerificationState::kVerified;
            cand_item.metadata.sig_vector   = cont.source_sig;
            cand_item.depth                 = item.depth;
            cand_item.rewrite_gen           = item.rewrite_gen;
            cand_item.attempted_mask        = item.attempted_mask;
            cand_item.history               = item.history;

            pr.decision = PassDecision::kSolvedCandidate;
            pr.next.push_back(std::move(cand_item));
            return pr;
        }

        ExtractorKind ProjectExtractorKind(RemainderOrigin origin) {
            switch (origin) {
                case RemainderOrigin::kDirectBooleanNull:
                    return ExtractorKind::kBooleanNullDirect;
                case RemainderOrigin::kProductCore:
                    return ExtractorKind::kProductAST;
                case RemainderOrigin::kPolynomialCore:
                    return ExtractorKind::kPolynomial;
                case RemainderOrigin::kTemplateCore:
                    return ExtractorKind::kTemplate;
                case RemainderOrigin::kSignatureLowering:
                    return ExtractorKind::kBooleanNullDirect;
                case RemainderOrigin::kLiftedOuter:
                    return ExtractorKind::kBooleanNullDirect;
            }
            return ExtractorKind::kBooleanNullDirect;
        }

        PassResult ResolveResidualRecombine(
            const RemainderRecombineCont &cont, CompetitionGroup &group, const WorkItem &item,
            OrchestratorContext &ctx
        ) {
            PassResult pr;
            pr.disposition = ItemDisposition::kConsumeCurrent;
            pr.decision    = PassDecision::kAdvance;

            bool parent_released = false;
            auto release_parent  = [&]() {
                if (!cont.parent_group_id.has_value() || parent_released) { return; }
                auto parent_resolved =
                    ReleaseHandle(ctx.competition_groups, *cont.parent_group_id);
                parent_released = true;
                if (parent_resolved.has_value()) {
                    pr.next.push_back(std::move(*parent_resolved));
                }
            };

            if (!group.best.has_value()) {
                release_parent();
                return pr;
            }
            if (!ctx.evaluator && cont.target_vars.empty()) {
                release_parent();
                return pr;
            }

            // Resolve target-local context for verification.
            const auto &target_vars =
                cont.target_vars.empty() ? ctx.original_vars : cont.target_vars;
            const auto &target_eval =
                cont.target_vars.empty() ? *ctx.evaluator : cont.target_eval;

            auto solved = CloneExpr(*group.best->expr);

            // Remap winner from reduced variable space to target.
            // The winner's real_vars may be a subset of target_vars
            // when aux-var elimination reduced the variable set.
            if (!cont.remainder_support.empty()
                && group.best->real_vars.size() < target_vars.size())
            {
                RemapVarIndices(*solved, cont.remainder_support);
            }

            // Strengthened 64-probe FW check of solved expr against
            // residual_eval, matching RunResidualSupported's check.
            const auto num_vars = static_cast< uint32_t >(target_vars.size());
            auto res_check =
                FullWidthCheckEval(cont.remainder_eval, num_vars, *solved, ctx.bitwidth, 64);
            if (!res_check.passed) {
                release_parent();
                return pr;
            }

            // Recombine: core_expr + solved_residual, or just solved
            // for direct boolean-null (core_expr is null).
            auto combined = cont.prefix_expr
                ? Expr::Add(CloneExpr(*cont.prefix_expr), std::move(solved))
                : std::move(solved);

            // Verify the recombined expression against the target
            // evaluator.
            auto orig_check =
                FullWidthCheckEval(target_eval, num_vars, *combined, ctx.bitwidth);
            if (!orig_check.passed) {
                release_parent();
                return pr;
            }

            auto cost_info = ComputeCost(*combined);

            if (cont.parent_group_id.has_value()) {
                SubmitCandidate(
                    ctx.competition_groups, *cont.parent_group_id,
                    CandidateRecord{
                        .expr                              = std::move(combined),
                        .cost                              = cost_info.cost,
                        .verification                      = VerificationState::kVerified,
                        .real_vars                         = target_vars,
                        .source_pass                       = PassId::kResidualSupported,
                        .needs_original_space_verification = false,
                    }
                );
                release_parent();
                pr.decision = PassDecision::kAdvance;
            } else {
                // No parent group — emit as a candidate directly.
                WorkItem cand_item;
                cand_item.payload = CandidatePayload{
                    .expr                              = std::move(combined),
                    .real_vars                         = target_vars,
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
                    .core_degree = cont.prefix_degree,
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
                } else if constexpr (std::is_same_v< T, RemainderRecombineCont >) {
                    return ResolveResidualRecombine(c, group, item, ctx);
                } else if constexpr (std::is_same_v< T, LiftedSubstituteCont >) {
                    return ResolveLiftedSubstitute(c, group, item, ctx);
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
        auto mapped_eval = BuildMappedEvaluator(ctx, sub_ctx, item);
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
        auto mapped_eval = BuildMappedEvaluator(ctx, sub_ctx, item);
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
        child.signature_recursion_depth = item.signature_recursion_depth + 1;
        child.group_id                  = item.group_id;
        child.evaluator_override        = item.evaluator_override;
        child.evaluator_override_arity  = item.evaluator_override_arity;
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
        auto mapped_eval = BuildMappedEvaluator(ctx, sub_ctx, item);
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

        auto mapped_eval = BuildMappedEvaluator(ctx, sub_ctx, item);
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

        // Build CoB expression from residual coefficients.
        auto bit_expr = BuildCobExpr(residual, num_vars, ctx.bitwidth);

        const bool bit_is_zero =
            bit_expr && bit_expr->kind == Expr::Kind::kConstant && bit_expr->constant_val == 0;

        // Build the prefix expression (singleton + poly, excluding CoB).
        std::unique_ptr< Expr > prefix;
        if (poly_expr) { prefix = std::move(poly_expr); }
        if (singleton_expr) {
            prefix = prefix ? Expr::Add(std::move(prefix), std::move(singleton_expr))
                            : std::move(singleton_expr);
        }

        assert(item.group_id.has_value());

        if (bit_is_zero) {
            // Zero residual: prefix alone is the full answer.
            if (!prefix) { prefix = Expr::Constant(0); }

            auto fw = FullWidthCheckEval(*mapped_eval, num_vars, *prefix, ctx.bitwidth);
            if (!fw.passed) {
                return Ok(
                    PassResult{
                        .decision    = PassDecision::kNoProgress,
                        .disposition = ItemDisposition::kRetainCurrent,
                    }
                );
            }

            auto cost_info = ComputeCost(*prefix);
            SubmitCandidate(
                ctx.competition_groups, *item.group_id,
                CandidateRecord{
                    .expr         = std::move(prefix),
                    .cost         = cost_info.cost,
                    .verification = VerificationState::kVerified,
                    .real_vars    = sub_ctx.real_vars,
                    .source_pass  = PassId::kSignatureSingletonPolyRecovery,
                    .needs_original_space_verification =
                        sub_ctx.needs_original_space_verification,
                    .sig_vector = sub_ctx.elimination.reduced_sig,
                }
            );

            return Ok(
                PassResult{
                    .decision    = PassDecision::kAdvance,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        // When invoked from a residual solver's recursive signature
        // chain (evaluator_override set), fall back to inline
        // combination to avoid residual -> signature -> residual
        // cycles. Only emit kRemainderState for top-level invocations.
        if (item.evaluator_override.has_value()) {
            std::unique_ptr< Expr > combined;
            if (bit_expr && !bit_is_zero) { combined = std::move(bit_expr); }
            if (prefix) {
                combined = combined ? Expr::Add(std::move(combined), std::move(prefix))
                                    : std::move(prefix);
            }
            if (!combined) { combined = Expr::Constant(0); }

            auto fw = FullWidthCheckEval(*mapped_eval, num_vars, *combined, ctx.bitwidth);
            if (!fw.passed) {
                return Ok(
                    PassResult{
                        .decision    = PassDecision::kNoProgress,
                        .disposition = ItemDisposition::kRetainCurrent,
                    }
                );
            }

            auto cost_info = ComputeCost(*combined);
            SubmitCandidate(
                ctx.competition_groups, *item.group_id,
                CandidateRecord{
                    .expr         = std::move(combined),
                    .cost         = cost_info.cost,
                    .verification = VerificationState::kVerified,
                    .real_vars    = sub_ctx.real_vars,
                    .source_pass  = PassId::kSignatureSingletonPolyRecovery,
                    .needs_original_space_verification =
                        sub_ctx.needs_original_space_verification,
                    .sig_vector = sub_ctx.elimination.reduced_sig,
                }
            );

            return Ok(
                PassResult{
                    .decision    = PassDecision::kAdvance,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        // Nonzero residual: emit kRemainderState for shared solver table.
        // Acquire a handle so the group stays open for the residual child.
        AcquireHandle(ctx.competition_groups, *item.group_id);

        if (!prefix) { prefix = Expr::Constant(0); }

        auto residual_eval    = BuildRemainderEvaluator(*mapped_eval, *prefix, ctx.bitwidth);
        auto residual_sig     = EvaluateBooleanSignature(residual_eval, num_vars, ctx.bitwidth);
        auto residual_elim    = EliminateAuxVars(residual_sig, sub_ctx.real_vars);
        auto residual_support = BuildVarSupport(sub_ctx.real_vars, residual_elim.real_vars);

        bool is_bn = std::all_of(residual_sig.begin(), residual_sig.end(), [](uint64_t s) {
            return s == 0;
        });

        WorkItem residual_item;
        residual_item.payload = RemainderStatePayload{
            .origin            = RemainderOrigin::kSignatureLowering,
            .prefix_expr       = std::move(prefix),
            .prefix_degree     = 0,
            .remainder_eval    = residual_eval,
            .source_sig        = sub_ctx.elimination.reduced_sig,
            .remainder_sig     = std::move(residual_sig),
            .remainder_elim    = std::move(residual_elim),
            .remainder_support = std::move(residual_support),
            .is_boolean_null   = is_bn,
            .degree_floor      = 2,
            .target =
                RemainderTargetContext{
                                       .eval = *mapped_eval,
                                       .vars = sub_ctx.real_vars,
                                       },
        };
        residual_item.features                  = item.features;
        residual_item.metadata                  = item.metadata;
        residual_item.depth                     = item.depth;
        residual_item.rewrite_gen               = item.rewrite_gen;
        residual_item.attempted_mask            = 0;
        residual_item.signature_recursion_depth = item.signature_recursion_depth;
        residual_item.history                   = item.history;
        residual_item.group_id                  = item.group_id;

        PassResult result;
        result.decision    = PassDecision::kAdvance;
        result.disposition = ItemDisposition::kRetainCurrent;
        result.next.push_back(std::move(residual_item));
        return Ok(std::move(result));
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

        auto mapped_eval = BuildMappedEvaluator(ctx, sub_ctx, item);
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

        if (item.signature_recursion_depth >= 2) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNoProgress,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        const auto &sig_payload = std::get< SignatureStatePayload >(item.payload);
        const auto &sub_ctx     = sig_payload.ctx;
        auto parent_eval        = BuildMappedEvaluator(ctx, sub_ctx, item);
        if (!parent_eval) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNoProgress,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        const auto &sig     = sub_ctx.elimination.reduced_sig;
        const auto num_vars = static_cast< uint32_t >(sub_ctx.real_vars.size());

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

                auto fw = FullWidthCheckEval(*parent_eval, num_vars, *composed, ctx.bitwidth);
                if (!fw.passed) { continue; }

                auto cost_info = ComputeCost(*composed);
                SubmitCandidate(
                    ctx.competition_groups, parent_group_id,
                    CandidateRecord{
                        .expr         = std::move(composed),
                        .cost         = cost_info.cost,
                        .verification = VerificationState::kVerified,
                        .real_vars    = sub_ctx.real_vars,
                        .source_pass  = PassId::kSignatureBitwiseDecompose,
                        .needs_original_space_verification =
                            sub_ctx.needs_original_space_verification,
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

            // Acquire a handle on the parent for this child lineage.
            AcquireHandle(ctx.competition_groups, parent_group_id);

            // Create child competition group with continuation
            auto child_group_id = CreateGroup(ctx.competition_groups, ctx.next_group_id);
            ctx.competition_groups.at(child_group_id).continuation = ContinuationData{
                BitwiseComposeCont{
                                   .var_k                   = cand.var_k,
                                   .gate                    = cand.gate,
                                   .add_coeff               = cand.add_coeff,
                                   .active_context_indices  = active_context_indices,
                                   .parent_group_id         = parent_group_id,
                                   .parent_eval             = parent_eval,
                                   .parent_real_vars        = sub_ctx.real_vars,
                                   .parent_original_indices = sub_ctx.original_indices,
                                   .parent_num_vars         = num_vars,
                                   .parent_needs_original_space_verification =
                        sub_ctx.needs_original_space_verification,
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
            child.evaluator_override        = item.evaluator_override;
            child.evaluator_override_arity  = item.evaluator_override_arity;
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

        if (item.signature_recursion_depth >= 1) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNoProgress,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        const auto &sig_payload = std::get< SignatureStatePayload >(item.payload);
        const auto &sub_ctx     = sig_payload.ctx;
        auto parent_eval        = BuildMappedEvaluator(ctx, sub_ctx, item);
        if (!parent_eval) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNoProgress,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        const auto &sig     = sub_ctx.elimination.reduced_sig;
        const auto num_vars = static_cast< uint32_t >(sub_ctx.real_vars.size());

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
            // Acquire a handle on the parent for this child lineage.
            AcquireHandle(ctx.competition_groups, parent_group_id);

            // Create child competition group with continuation
            auto child_group_id = CreateGroup(ctx.competition_groups, ctx.next_group_id);
            ctx.competition_groups.at(child_group_id).continuation = ContinuationData{
                HybridComposeCont{
                                  .var_k                   = cand.var_k,
                                  .op                      = cand.op,
                                  .parent_group_id         = parent_group_id,
                                  .parent_eval             = parent_eval,
                                  .parent_real_vars        = sub_ctx.real_vars,
                                  .parent_original_indices = sub_ctx.original_indices,
                                  .parent_num_vars         = num_vars,
                                  .parent_needs_original_space_verification =
                        sub_ctx.needs_original_space_verification,
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
            child.evaluator_override        = item.evaluator_override;
            child.evaluator_override_arity  = item.evaluator_override_arity;
            child.history                   = item.history;

            result.next.push_back(std::move(child));
        }

        return Ok(std::move(result));
    }

} // namespace cobra
