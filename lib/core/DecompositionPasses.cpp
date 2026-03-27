#include "CompetitionGroup.h"
#include "ContinuationTypes.h"
#include "DecompositionPassHelpers.h"
#include "OrchestratorPasses.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/DecompositionEngine.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/GhostResidualSolver.h"
#include "cobra/core/MultivarPolyRecovery.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/SignatureSimplifier.h"
#include "cobra/core/Simplifier.h"
#include "cobra/core/TemplateDecomposer.h"

namespace cobra {

    namespace {

        bool IsAstKind(const WorkItem &item) {
            return std::holds_alternative< AstPayload >(item.payload);
        }

        bool IsResidualKind(const WorkItem &item) {
            return std::holds_alternative< ResidualStatePayload >(item.payload);
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
                case ResidualOrigin::kSignatureLowering:
                    return ExtractorKind::kBooleanNullDirect;
                case ResidualOrigin::kLiftedOuter:
                    return ExtractorKind::kBooleanNullDirect;
            }
            return ExtractorKind::kBooleanNullDirect;
        }

        std::optional< PassResult > TryRecombineAndEmit(
            const ResidualStatePayload &residual, std::unique_ptr< Expr > solved_expr,
            const std::vector< std::string > &solved_expr_vars, const WorkItem &parent,
            OrchestratorContext &ctx, PassId producing_pass, ResidualSolverKind solver_kind
        ) {
            // Resolve target-local variable space: prefer the residual's
            // target context when populated, fall back to the global context.
            const auto &target_vars =
                residual.target.vars.empty() ? ctx.original_vars : residual.target.vars;
            const auto &target_eval =
                residual.target.vars.empty() ? *ctx.evaluator : residual.target.eval;
            const auto &remap = residual.target.remap_support.empty()
                ? residual.residual_support
                : residual.target.remap_support;

            // Some residual solvers return expressions already embedded in the
            // target variable universe. Callers pass target_vars in that case
            // to suppress remapping.
            if (solved_expr_vars.size() < target_vars.size()) {
                RemapVarIndices(*solved_expr, remap);
            }

            auto combined = residual.core_expr
                ? Expr::Add(CloneExpr(*residual.core_expr), std::move(solved_expr))
                : std::move(solved_expr);

            const auto num_vars = static_cast< uint32_t >(target_vars.size());
            auto check = FullWidthCheckEval(target_eval, num_vars, *combined, ctx.bitwidth);

            if (!check.passed) { return std::nullopt; }

            auto cost_info = ComputeCost(*combined);
            WorkItem cand_item;
            cand_item.payload = CandidatePayload{
                .expr                              = std::move(combined),
                .real_vars                         = target_vars,
                .cost                              = cost_info.cost,
                .producing_pass                    = producing_pass,
                .needs_original_space_verification = false,
            };
            cand_item.features                    = parent.features;
            cand_item.metadata                    = parent.metadata;
            cand_item.metadata.verification       = VerificationState::kVerified;
            cand_item.metadata.sig_vector         = residual.source_sig;
            cand_item.metadata.decomposition_meta = DecompositionMeta{
                .extractor_kind = static_cast< uint8_t >(ProjectExtractorKind(residual.origin)),
                .solver_kind    = static_cast< uint8_t >(solver_kind),
                .has_solver     = true,
                .core_degree    = residual.core_degree,
            };
            cand_item.depth          = parent.depth;
            cand_item.rewrite_gen    = parent.rewrite_gen;
            cand_item.attempted_mask = parent.attempted_mask;
            cand_item.history        = parent.history;
            cand_item.group_id       = parent.group_id;

            PassResult result;
            result.decision    = PassDecision::kSolvedCandidate;
            result.disposition = ItemDisposition::kConsumeCurrent;
            result.next.push_back(std::move(cand_item));
            return result;
        }

        template< typename ExtractorFn >
        Result< PassResult > RunExtractor(
            const WorkItem &item, OrchestratorContext &ctx, PassId pass_id,
            ExtractorKind expected_kind, ExtractorFn extractor_fn
        ) {
            if (!IsAstKind(item)) {
                return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
            }

            const auto &ast         = std::get< AstPayload >(item.payload);
            const auto &active_vars = ActiveAstVars(item, ctx);
            const auto &active_eval = ActiveAstEvaluator(item, ctx);
            if (!ast.classification) {
                return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
            }
            if (!active_eval) {
                return Ok(
                    PassResult{
                        .decision = PassDecision::kBlocked,
                        .reason =
                            ReasonDetail{
                                         .top = { .code    = { ReasonCategory::kGuardFailed,
                                                      ReasonDomain::kDecomposition,
                                                      decomposition::kNoEvaluator },
                                         .message = "Decomposition requires evaluator" },
                                         },
                }
                );
            }

            auto decomp_sig = ComputeDecompositionSignature(item, ctx, item.rewrite_gen);

            DecompositionContext dctx{
                .opts         = ctx.opts,
                .vars         = active_vars,
                .sig          = decomp_sig,
                .current_expr = ast.expr.get(),
                .cls          = *ast.classification,
            };

            auto core = extractor_fn(dctx);

            if (!core.Succeeded()) {
                PassDecision decision;
                switch (core.Kind()) {
                    case OutcomeKind::kInapplicable:
                        decision = PassDecision::kNotApplicable;
                        break;
                    default:
                        decision = PassDecision::kBlocked;
                        break;
                }
                return Ok(
                    PassResult{
                        .decision    = decision,
                        .disposition = ItemDisposition::kRetainCurrent,
                        .reason      = core.Reason(),
                    }
                );
            }

            auto core_payload   = core.TakePayload();
            const auto num_vars = static_cast< uint32_t >(active_vars.size());

            // Direct check: does the core alone solve the function?
            auto direct_check =
                FullWidthCheckEval(*active_eval, num_vars, *core_payload.expr, ctx.bitwidth);

            if (direct_check.passed) {
                auto cost_info = ComputeCost(*core_payload.expr);
                WorkItem cand_item;
                cand_item.payload = CandidatePayload{
                    .expr                              = std::move(core_payload.expr),
                    .real_vars                         = active_vars,
                    .cost                              = cost_info.cost,
                    .producing_pass                    = pass_id,
                    .needs_original_space_verification = false,
                };
                cand_item.features                    = item.features;
                cand_item.metadata                    = item.metadata;
                cand_item.metadata.verification       = VerificationState::kVerified;
                cand_item.metadata.sig_vector         = decomp_sig;
                cand_item.metadata.decomposition_meta = DecompositionMeta{
                    .extractor_kind = static_cast< uint8_t >(expected_kind),
                    .core_degree    = core_payload.degree_used,
                };
                cand_item.depth          = item.depth;
                cand_item.rewrite_gen    = item.rewrite_gen;
                cand_item.attempted_mask = item.attempted_mask;
                cand_item.history        = item.history;
                cand_item.group_id       = item.group_id;

                PassResult result;
                result.decision    = PassDecision::kSolvedCandidate;
                result.disposition = ItemDisposition::kRetainCurrent;
                result.next.push_back(std::move(cand_item));
                return Ok(std::move(result));
            }

            // AcceptCore gate for polynomial extractors
            if (expected_kind == ExtractorKind::kPolynomial) {
                if (!AcceptCore(dctx, core_payload)) {
                    return Ok(
                        PassResult{
                            .decision    = PassDecision::kBlocked,
                            .disposition = ItemDisposition::kRetainCurrent,
                            .reason =
                                ReasonDetail{
                                             .top = { .code    = { ReasonCategory::kInapplicable,
                                                          ReasonDomain::kDecomposition,
                                                          decomposition::kCoreRejected },
                                             .message = "polynomial core rejected by "
                                                        "acceptance gate" },
                                             },
                    }
                    );
                }
            }

            // Emit CoreCandidatePayload for residual pipeline
            WorkItem core_item;
            core_item.payload = CoreCandidatePayload{
                .core_expr      = std::move(core_payload.expr),
                .extractor_kind = core_payload.kind,
                .degree_used    = core_payload.degree_used,
                .source_sig     = std::move(decomp_sig),
                .target =
                    ResidualTargetContext{
                                          .eval = *active_eval,
                                          .vars = active_vars,
                                          },
            };
            core_item.features       = item.features;
            core_item.metadata       = item.metadata;
            core_item.depth          = item.depth;
            core_item.rewrite_gen    = item.rewrite_gen;
            core_item.attempted_mask = item.attempted_mask;
            core_item.history        = item.history;
            core_item.group_id       = item.group_id;

            PassResult result;
            result.decision    = PassDecision::kAdvance;
            result.disposition = ItemDisposition::kRetainCurrent;
            result.next.push_back(std::move(core_item));
            return Ok(std::move(result));
        }

    } // namespace

    Result< PassResult > RunExtractProductCore(const WorkItem &item, OrchestratorContext &ctx) {
        return RunExtractor(
            item, ctx, PassId::kExtractProductCore, ExtractorKind::kProductAST,
            [](const DecompositionContext &dctx) { return ExtractProductCore(dctx); }
        );
    }

    Result< PassResult > RunExtractPolyCoreD2(const WorkItem &item, OrchestratorContext &ctx) {
        return RunExtractor(
            item, ctx, PassId::kExtractPolyCoreD2, ExtractorKind::kPolynomial,
            [](const DecompositionContext &dctx) { return ExtractPolyCore(dctx, 2); }
        );
    }

    Result< PassResult >
    RunExtractTemplateCore(const WorkItem &item, OrchestratorContext &ctx) {
        return RunExtractor(
            item, ctx, PassId::kExtractTemplateCore, ExtractorKind::kTemplate,
            [](const DecompositionContext &dctx) { return ExtractTemplateCore(dctx); }
        );
    }

    Result< PassResult > RunExtractPolyCoreD3(const WorkItem &item, OrchestratorContext &ctx) {
        return RunExtractor(
            item, ctx, PassId::kExtractPolyCoreD3, ExtractorKind::kPolynomial,
            [](const DecompositionContext &dctx) { return ExtractPolyCore(dctx, 3); }
        );
    }

    Result< PassResult > RunExtractPolyCoreD4(const WorkItem &item, OrchestratorContext &ctx) {
        return RunExtractor(
            item, ctx, PassId::kExtractPolyCoreD4, ExtractorKind::kPolynomial,
            [](const DecompositionContext &dctx) { return ExtractPolyCore(dctx, 4); }
        );
    }

    Result< PassResult >
    RunPrepareDirectResidual(const WorkItem &item, OrchestratorContext &ctx) {
        if (!IsAstKind(item)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }
        const auto &active_vars = ActiveAstVars(item, ctx);
        const auto &active_eval = ActiveAstEvaluator(item, ctx);
        if (!active_eval) {
            return Ok(
                PassResult{
                    .decision = PassDecision::kBlocked,
                    .reason =
                        ReasonDetail{
                                     .top = { .code    = { ReasonCategory::kGuardFailed,
                                                  ReasonDomain::kDecomposition,
                                                  decomposition::kNoEvaluator },
                                     .message = "Decomposition requires evaluator" },
                                     },
            }
            );
        }

        auto decomp_sig     = ComputeDecompositionSignature(item, ctx, item.rewrite_gen);
        const auto num_vars = static_cast< uint32_t >(active_vars.size());

        auto elim    = EliminateAuxVars(decomp_sig, active_vars, *active_eval, ctx.bitwidth);
        auto support = BuildVarSupport(active_vars, elim.real_vars);

        bool is_bn =
            IsBooleanNullResidual(*active_eval, support, num_vars, ctx.bitwidth, decomp_sig);

        if (!is_bn) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNotApplicable,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        WorkItem residual_item;
        residual_item.payload = ResidualStatePayload{
            .origin           = ResidualOrigin::kDirectBooleanNull,
            .core_expr        = nullptr,
            .core_degree      = 0,
            .residual_eval    = *active_eval,
            .source_sig       = decomp_sig,
            .residual_sig     = decomp_sig,
            .residual_elim    = std::move(elim),
            .residual_support = std::move(support),
            .is_boolean_null  = true,
            .degree_floor     = 2,
            .target =
                ResidualTargetContext{
                                      .eval = *active_eval,
                                      .vars = active_vars,
                                      },
        };
        residual_item.features       = item.features;
        residual_item.metadata       = item.metadata;
        residual_item.depth          = item.depth;
        residual_item.rewrite_gen    = item.rewrite_gen;
        residual_item.attempted_mask = item.attempted_mask;
        residual_item.history        = item.history;
        residual_item.group_id       = item.group_id;

        PassResult result;
        result.decision    = PassDecision::kAdvance;
        result.disposition = ItemDisposition::kRetainCurrent;
        result.next.push_back(std::move(residual_item));
        return Ok(std::move(result));
    }

    Result< PassResult >
    RunPrepareResidualFromCore(const WorkItem &item, OrchestratorContext &ctx) {
        if (!std::holds_alternative< CoreCandidatePayload >(item.payload)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }
        const auto &core = std::get< CoreCandidatePayload >(item.payload);
        if (core.target.vars.empty() && !ctx.evaluator) {
            return Ok(
                PassResult{
                    .decision = PassDecision::kBlocked,
                    .reason =
                        ReasonDetail{
                                     .top = { .code    = { ReasonCategory::kGuardFailed,
                                                  ReasonDomain::kDecomposition,
                                                  decomposition::kNoEvaluator },
                                     .message = "Decomposition requires evaluator" },
                                     },
            }
            );
        }

        // Resolve target-local variable space from the core's target context.
        const auto &target_vars =
            core.target.vars.empty() ? ctx.original_vars : core.target.vars;
        const auto &target_eval = core.target.vars.empty() ? *ctx.evaluator : core.target.eval;
        const auto num_vars     = static_cast< uint32_t >(target_vars.size());

        auto residual_eval = BuildResidualEvaluator(target_eval, *core.core_expr, ctx.bitwidth);

        auto residual_sig = EvaluateBooleanSignature(residual_eval, num_vars, ctx.bitwidth);

        auto elim    = EliminateAuxVars(residual_sig, target_vars, residual_eval, ctx.bitwidth);
        auto support = BuildVarSupport(target_vars, elim.real_vars);

        bool is_bn =
            IsBooleanNullResidual(residual_eval, support, num_vars, ctx.bitwidth, residual_sig);

        ResidualOrigin origin;
        switch (core.extractor_kind) {
            case ExtractorKind::kProductAST:
                origin = ResidualOrigin::kProductCore;
                break;
            case ExtractorKind::kPolynomial:
                origin = ResidualOrigin::kPolynomialCore;
                break;
            case ExtractorKind::kTemplate:
                origin = ResidualOrigin::kTemplateCore;
                break;
            case ExtractorKind::kBooleanNullDirect:
                origin = ResidualOrigin::kDirectBooleanNull;
                break;
        }

        uint8_t degree_floor = (core.extractor_kind == ExtractorKind::kPolynomial)
            ? static_cast< uint8_t >(core.degree_used + 1)
            : 2;

        WorkItem residual_item;
        residual_item.payload = ResidualStatePayload{
            .origin           = origin,
            .core_expr        = CloneExpr(*core.core_expr),
            .core_degree      = core.degree_used,
            .residual_eval    = std::move(residual_eval),
            .source_sig       = core.source_sig,
            .residual_sig     = std::move(residual_sig),
            .residual_elim    = std::move(elim),
            .residual_support = std::move(support),
            .is_boolean_null  = is_bn,
            .degree_floor     = degree_floor,
            .target =
                ResidualTargetContext{
                                      .eval          = target_eval,
                                      .vars          = target_vars,
                                      .remap_support = core.target.remap_support,
                                      },
        };
        residual_item.features       = item.features;
        residual_item.metadata       = item.metadata;
        residual_item.depth          = item.depth;
        residual_item.rewrite_gen    = item.rewrite_gen;
        residual_item.attempted_mask = item.attempted_mask;
        residual_item.history        = item.history;
        residual_item.group_id       = item.group_id;

        PassResult result;
        result.decision    = PassDecision::kAdvance;
        result.disposition = ItemDisposition::kConsumeCurrent;
        result.next.push_back(std::move(residual_item));
        return Ok(std::move(result));
    }

    // ---------------------------------------------------------------
    // Residual solver passes
    // ---------------------------------------------------------------

    Result< PassResult > RunResidualGhost(const WorkItem &item, OrchestratorContext &ctx) {
        if (!IsResidualKind(item)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }
        const auto &residual = std::get< ResidualStatePayload >(item.payload);
        if (!residual.is_boolean_null) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }
        const auto res_real_count =
            static_cast< uint32_t >(residual.residual_elim.real_vars.size());
        if (res_real_count > 6) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNotApplicable,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        const auto &target_vars =
            residual.target.vars.empty() ? ctx.original_vars : residual.target.vars;
        const auto num_vars = static_cast< uint32_t >(target_vars.size());
        auto ghost          = SolveGhostResidual(
            residual.residual_eval, residual.residual_support, num_vars, ctx.bitwidth
        );
        if (!ghost.Succeeded()) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kBlocked,
                    .disposition = ItemDisposition::kRetainCurrent,
                    .reason      = ghost.Reason(),
                }
            );
        }

        auto ghost_payload = ghost.TakePayload();
        auto recombined    = TryRecombineAndEmit(
            residual, std::move(ghost_payload.expr), target_vars, item, ctx,
            PassId::kResidualGhost, ResidualSolverKind::kGhostResidual
        );
        if (recombined) { return Ok(std::move(*recombined)); }

        return Ok(
            PassResult{
                .decision    = PassDecision::kBlocked,
                .disposition = ItemDisposition::kRetainCurrent,
                .reason =
                    ReasonDetail{
                                 .top = { .code    = { ReasonCategory::kVerifyFailed,
                                              ReasonDomain::kGhostResidual,
                                              decomposition::kResidualFailed },
                                 .message = "ghost residual recombination failed "
                                            "full-width verification" },
                                 },
        }
        );
    }

    Result< PassResult >
    RunResidualFactoredGhost(const WorkItem &item, OrchestratorContext &ctx) {
        if (!IsResidualKind(item)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }
        const auto &residual = std::get< ResidualStatePayload >(item.payload);
        if (!residual.is_boolean_null) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }
        const auto res_real_count =
            static_cast< uint32_t >(residual.residual_elim.real_vars.size());
        if (res_real_count > 6) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNotApplicable,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        const auto &target_vars =
            residual.target.vars.empty() ? ctx.original_vars : residual.target.vars;
        const auto num_vars = static_cast< uint32_t >(target_vars.size());
        auto factored       = SolveFactoredGhostResidual(
            residual.residual_eval, residual.residual_support, num_vars, ctx.bitwidth
        );
        if (!factored.Succeeded()) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kBlocked,
                    .disposition = ItemDisposition::kRetainCurrent,
                    .reason      = factored.Reason(),
                }
            );
        }

        auto factored_payload = factored.TakePayload();
        auto recombined       = TryRecombineAndEmit(
            residual, std::move(factored_payload.expr), target_vars, item, ctx,
            PassId::kResidualFactoredGhost, ResidualSolverKind::kGhostResidual
        );
        if (recombined) { return Ok(std::move(*recombined)); }

        return Ok(
            PassResult{
                .decision    = PassDecision::kBlocked,
                .disposition = ItemDisposition::kRetainCurrent,
                .reason =
                    ReasonDetail{
                                 .top = { .code    = { ReasonCategory::kVerifyFailed,
                                              ReasonDomain::kGhostResidual,
                                              decomposition::kResidualFailed },
                                 .message = "factored ghost recombination failed "
                                            "full-width verification" },
                                 },
        }
        );
    }

    Result< PassResult >
    RunResidualFactoredGhostEscalated(const WorkItem &item, OrchestratorContext &ctx) {
        if (!IsResidualKind(item)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }
        const auto &residual = std::get< ResidualStatePayload >(item.payload);
        if (!residual.is_boolean_null) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }
        const auto res_real_count =
            static_cast< uint32_t >(residual.residual_elim.real_vars.size());
        if (res_real_count > 6) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNotApplicable,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        const auto &target_vars =
            residual.target.vars.empty() ? ctx.original_vars : residual.target.vars;
        const auto num_vars = static_cast< uint32_t >(target_vars.size());
        uint8_t grid        = (res_real_count <= 2) ? 3 : 2;
        auto factored       = SolveFactoredGhostResidual(
            residual.residual_eval, residual.residual_support, num_vars, ctx.bitwidth, 2, grid
        );
        if (!factored.Succeeded()) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kBlocked,
                    .disposition = ItemDisposition::kRetainCurrent,
                    .reason      = factored.Reason(),
                }
            );
        }

        auto factored_payload = factored.TakePayload();
        auto recombined       = TryRecombineAndEmit(
            residual, std::move(factored_payload.expr), target_vars, item, ctx,
            PassId::kResidualFactoredGhostEscalated, ResidualSolverKind::kGhostResidual
        );
        if (recombined) { return Ok(std::move(*recombined)); }

        return Ok(
            PassResult{
                .decision    = PassDecision::kBlocked,
                .disposition = ItemDisposition::kRetainCurrent,
                .reason =
                    ReasonDetail{
                                 .top = { .code    = { ReasonCategory::kVerifyFailed,
                                              ReasonDomain::kGhostResidual,
                                              decomposition::kResidualFailed },
                                 .message = "escalated factored ghost recombination "
                                            "failed full-width verification" },
                                 },
        }
        );
    }

    Result< PassResult >
    RunResidualPolyRecovery(const WorkItem &item, OrchestratorContext &ctx) {
        if (!IsResidualKind(item)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }
        const auto &residual = std::get< ResidualStatePayload >(item.payload);
        const auto res_real_count =
            static_cast< uint32_t >(residual.residual_elim.real_vars.size());
        if (res_real_count > 6) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        const auto &target_vars =
            residual.target.vars.empty() ? ctx.original_vars : residual.target.vars;
        const auto num_vars = static_cast< uint32_t >(target_vars.size());
        auto res_poly       = RecoverAndVerifyPoly(
            residual.residual_eval, residual.residual_support, num_vars, ctx.bitwidth, 4,
            residual.degree_floor
        );
        if (!res_poly.Succeeded()) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kBlocked,
                    .disposition = ItemDisposition::kRetainCurrent,
                    .reason      = res_poly.Reason(),
                }
            );
        }

        auto poly_payload = res_poly.TakePayload();
        auto recombined   = TryRecombineAndEmit(
            residual, std::move(poly_payload.expr), target_vars, item, ctx,
            PassId::kResidualPolyRecovery, ResidualSolverKind::kPolynomialRecovery
        );
        if (recombined) { return Ok(std::move(*recombined)); }

        return Ok(
            PassResult{
                .decision    = PassDecision::kBlocked,
                .disposition = ItemDisposition::kRetainCurrent,
                .reason =
                    ReasonDetail{
                                 .top = { .code    = { ReasonCategory::kVerifyFailed,
                                              ReasonDomain::kPolynomialRecovery,
                                              decomposition::kResidualFailed },
                                 .message = "polynomial residual recombination "
                                            "failed full-width verification" },
                                 },
        }
        );
    }

    Result< PassResult > RunResidualSupported(const WorkItem &item, OrchestratorContext &ctx) {
        if (!IsResidualKind(item)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }
        const auto &residual = std::get< ResidualStatePayload >(item.payload);

        // Resolve target-local variable space.
        const auto &target_vars =
            residual.target.vars.empty() ? ctx.original_vars : residual.target.vars;

        // Aux-var elimination on residual signature in the target space.
        auto elim                 = EliminateAuxVars(residual.residual_sig, target_vars);
        const auto real_var_count = static_cast< uint32_t >(elim.real_vars.size());
        if (real_var_count == 0 || real_var_count > ctx.opts.max_vars) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kBlocked,
                    .disposition = ItemDisposition::kRetainCurrent,
                    .reason =
                        ReasonDetail{
                                     .top = { .code    = { ReasonCategory::kNoSolution,
                                                  ReasonDomain::kDecomposition,
                                                  decomposition::kResidualFailed },
                                     .message = "residual variable count out of range" },
                                     },
            }
            );
        }

        auto original_indices = BuildVarSupport(target_vars, elim.real_vars);

        // Propagate parent group_id from the incoming item.
        // Acquire a handle so the parent stays alive until this
        // child's residual-signature solve completes.
        std::optional< GroupId > parent_group_id =
            item.group_id.has_value() ? item.group_id : std::nullopt;
        if (parent_group_id.has_value()) {
            AcquireHandle(ctx.competition_groups, *parent_group_id);
        }

        // Resolve target-local evaluator for verification.
        const auto &target_eval =
            residual.target.vars.empty() ? *ctx.evaluator : residual.target.eval;

        // Create competition group with ResidualRecombineCont.
        auto group_id      = CreateGroup(ctx.competition_groups, ctx.next_group_id);
        auto &group        = ctx.competition_groups.at(group_id);
        group.continuation = ResidualRecombineCont{
            .core_expr        = residual.core_expr ? CloneExpr(*residual.core_expr) : nullptr,
            .origin           = residual.origin,
            .residual_eval    = residual.residual_eval,
            .source_sig       = residual.source_sig,
            .residual_support = residual.residual_support,
            .core_degree      = residual.core_degree,
            .parent_group_id  = parent_group_id,
            .target_eval      = target_eval,
            .target_vars      = target_vars,
        };

        // Emit kSignatureState child with the residual evaluator override.
        WorkItem child;
        child.payload = SignatureStatePayload{
            .ctx = {
                .sig              = elim.reduced_sig,
                .real_vars        = elim.real_vars,
                .elimination      = std::move(elim),
                .original_indices = std::move(original_indices),
                .needs_original_space_verification = true,
            },
        };
        child.features           = item.features;
        child.metadata           = item.metadata;
        child.group_id           = group_id;
        child.evaluator_override = residual.residual_eval;

        PassResult result;
        result.decision    = PassDecision::kAdvance;
        result.disposition = ItemDisposition::kRetainCurrent;
        result.next.push_back(std::move(child));
        return Ok(std::move(result));
    }

    Result< PassResult > RunResidualTemplate(const WorkItem &item, OrchestratorContext &ctx) {
        if (!IsResidualKind(item)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }
        const auto &residual = std::get< ResidualStatePayload >(item.payload);
        const auto res_real_count =
            static_cast< uint32_t >(residual.residual_elim.real_vars.size());

        const auto &target_vars =
            residual.target.vars.empty() ? ctx.original_vars : residual.target.vars;

        SignatureContext res_sig_ctx;
        res_sig_ctx.vars             = residual.residual_elim.real_vars;
        res_sig_ctx.original_indices = residual.residual_support;

        if (residual.residual_elim.real_vars.size() == target_vars.size()) {
            res_sig_ctx.eval = residual.residual_eval;
        } else {
            res_sig_ctx.eval = [residual_eval = residual.residual_eval,
                                idx           = residual.residual_support,
                                full          = std::vector< uint64_t >(target_vars.size(), 0)](
                                   const std::vector< uint64_t > &rv
                               ) mutable -> uint64_t {
                for (size_t i = 0; i < idx.size(); ++i) { full[idx[i]] = rv[i]; }
                uint64_t result = residual_eval(full);
                for (size_t i = 0; i < idx.size(); ++i) { full[idx[i]] = 0; }
                return result;
            };
        }

        Options residual_opts   = ctx.opts;
        residual_opts.evaluator = residual.residual_eval;

        auto td = TryTemplateDecomposition(res_sig_ctx, residual_opts, res_real_count, nullptr);
        if (!td.Succeeded()) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kBlocked,
                    .disposition = ItemDisposition::kRetainCurrent,
                    .reason      = td.Reason(),
                }
            );
        }

        auto solved_expr = std::move(td.TakePayload().expr);
        auto recombined  = TryRecombineAndEmit(
            residual, std::move(solved_expr), residual.residual_elim.real_vars, item, ctx,
            PassId::kResidualTemplate, ResidualSolverKind::kTemplateDecomposition
        );
        if (recombined) { return Ok(std::move(*recombined)); }

        return Ok(
            PassResult{
                .decision    = PassDecision::kBlocked,
                .disposition = ItemDisposition::kRetainCurrent,
                .reason =
                    ReasonDetail{
                                 .top = { .code    = { ReasonCategory::kVerifyFailed,
                                              ReasonDomain::kTemplateDecomposer,
                                              decomposition::kResidualFailed },
                                 .message = "template residual recombination "
                                            "failed full-width verification" },
                                 },
        }
        );
    }

} // namespace cobra
