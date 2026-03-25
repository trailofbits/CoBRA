#include "DecompositionPassHelpers.h"
#include "OrchestratorPasses.h"
#include "cobra/core/DecompositionEngine.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/SignatureChecker.h"

namespace cobra {

    namespace {

        bool IsAstKind(const WorkItem &item) {
            return std::holds_alternative< AstPayload >(item.payload);
        }

        template< typename ExtractorFn >
        Result< PassResult > RunExtractor(
            const WorkItem &item, OrchestratorContext &ctx, PassId pass_id,
            ExtractorKind expected_kind, ExtractorFn extractor_fn
        ) {
            if (!IsAstKind(item)) {
                return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
            }

            const auto &ast = std::get< AstPayload >(item.payload);
            if (!ast.classification) {
                return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
            }
            if (!ctx.evaluator) {
                return Ok(
                    PassResult{
                        .decision = PassDecision::kBlocked,
                        .reason =
                            ReasonDetail{
                                         .top = { .code    = { ReasonCategory::kGuardFailed,
                                                      ReasonDomain::kDecomposition },
                                         .message = "Decomposition requires evaluator" },
                                         },
                }
                );
            }

            auto decomp_sig = ComputeDecompositionSignature(ast, ctx, item.rewrite_gen);

            DecompositionContext dctx{
                .opts         = ctx.opts,
                .vars         = ctx.original_vars,
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
            const auto num_vars = static_cast< uint32_t >(ctx.original_vars.size());

            // Direct check: does the core alone solve the function?
            auto direct_check =
                FullWidthCheckEval(*ctx.evaluator, num_vars, *core_payload.expr, ctx.bitwidth);

            if (direct_check.passed) {
                auto cost_info = ComputeCost(*core_payload.expr);
                WorkItem cand_item;
                cand_item.payload = CandidatePayload{
                    .expr                              = std::move(core_payload.expr),
                    .real_vars                         = ctx.original_vars,
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
            };
            core_item.features       = item.features;
            core_item.metadata       = item.metadata;
            core_item.depth          = item.depth;
            core_item.rewrite_gen    = item.rewrite_gen;
            core_item.attempted_mask = item.attempted_mask;
            core_item.history        = item.history;

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

} // namespace cobra
