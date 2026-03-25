#include "OrchestratorPasses.h"
#include "SimplifierInternal.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/DecompositionEngine.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/MixedProductRewriter.h"
#include "cobra/core/OperandSimplifier.h"
#include "cobra/core/PatternMatcher.h"
#include "cobra/core/ProductIdentityRecoverer.h"
#include "cobra/core/SemilinearSignature.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/SignatureSimplifier.h"

namespace cobra {

    namespace {

        bool IsAstKind(const WorkItem &item) {
            return std::holds_alternative< AstPayload >(item.payload);
        }

        bool IsSignatureKind(const WorkItem &item) {
            return std::holds_alternative< SignatureStatePayload >(item.payload);
        }

        bool IsCandidateKind(const WorkItem &item) {
            return std::holds_alternative< CandidatePayload >(item.payload);
        }

    } // namespace

    // ---------------------------------------------------------------
    // Analysis passes (Task 9)
    // ---------------------------------------------------------------

    Result< PassResult > RunLowerNotOverArith(const WorkItem &item, OrchestratorContext &ctx) {
        if (!IsAstKind(item)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        const auto &ast = std::get< AstPayload >(item.payload);
        if (ast.provenance != Provenance::kOriginal) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        if (!internal::HasNotOverArith(*ast.expr)) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNoProgress,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        auto lowered = internal::LowerNotOverArith(CloneExpr(*ast.expr), ctx.bitwidth);

        const auto num_vars = static_cast< uint32_t >(ctx.original_vars.size());
        auto new_sig        = EvaluateBooleanSignature(*lowered, num_vars, ctx.bitwidth);

        WorkItem new_item;
        new_item.payload = AstPayload{
            .expr           = std::move(lowered),
            .classification = ast.classification,
            .provenance     = Provenance::kLowered,
        };
        new_item.features            = item.features;
        new_item.features.provenance = Provenance::kLowered;
        new_item.metadata            = item.metadata;
        new_item.metadata.sig_vector = std::move(new_sig);
        new_item.depth               = item.depth;
        new_item.rewrite_gen         = item.rewrite_gen;
        new_item.attempted_mask      = item.attempted_mask;
        new_item.history             = item.history;

        PassResult result;
        result.decision    = PassDecision::kAdvance;
        result.disposition = ItemDisposition::kRetainCurrent;
        result.next.push_back(std::move(new_item));
        return Ok(std::move(result));
    }

    Result< PassResult > RunClassifyAst(const WorkItem &item, OrchestratorContext &ctx) {
        if (!IsAstKind(item)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        const auto &ast = std::get< AstPayload >(item.payload);
        auto cls        = ClassifyStructural(*ast.expr);

        WorkItem new_item;
        new_item.payload = AstPayload{
            .expr           = CloneExpr(*ast.expr),
            .classification = cls,
            .provenance     = ast.provenance,
        };
        new_item.features                = item.features;
        new_item.features.classification = cls;
        new_item.metadata                = item.metadata;
        new_item.depth                   = item.depth;
        new_item.rewrite_gen             = item.rewrite_gen;
        new_item.attempted_mask          = item.attempted_mask;
        new_item.history                 = item.history;

        ctx.run_metadata.input_classification = cls;

        PassResult result;
        result.decision    = PassDecision::kAdvance;
        result.disposition = ItemDisposition::kConsumeCurrent;
        result.next.push_back(std::move(new_item));
        return Ok(std::move(result));
    }

    Result< PassResult >
    RunBuildSignatureState(const WorkItem &item, OrchestratorContext &ctx) {
        if (!IsAstKind(item)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        const auto &ast     = std::get< AstPayload >(item.payload);
        const auto num_vars = static_cast< uint32_t >(ctx.original_vars.size());

        // Step 1: Compute signature.
        // For the initial (non-rewritten) item, use the parser's
        // input signature when NOT-over-arith lowering was a no-op.
        // This matches the legacy's working_sig exactly.  For
        // lowered or rewritten items, recompute from the AST.
        bool use_input_sig =
            !ctx.input_sig.empty() && !ctx.lowering_fired && item.rewrite_gen == 0;
        auto sig = use_input_sig ? ctx.input_sig
                                 : EvaluateBooleanSignature(*ast.expr, num_vars, ctx.bitwidth);

        // Step 2: Constant match
        {
            auto pm = MatchPattern(sig, num_vars, ctx.bitwidth);
            if (pm && (*pm)->kind == Expr::Kind::kConstant) {
                bool needs_verify = ctx.evaluator.has_value();
                WorkItem cand_item;
                cand_item.payload = CandidatePayload{
                    .expr                              = std::move(*pm),
                    .real_vars                         = {},
                    .cost                              = ExprCost{ .weighted_size = 1 },
                    .producing_pass                    = PassId::kBuildSignatureState,
                    .needs_original_space_verification = needs_verify,
                };
                cand_item.features              = item.features;
                cand_item.metadata              = item.metadata;
                cand_item.metadata.sig_vector   = sig;
                cand_item.metadata.verification = needs_verify ? VerificationState::kUnverified
                                                               : VerificationState::kVerified;
                cand_item.depth                 = item.depth;
                cand_item.rewrite_gen           = item.rewrite_gen;
                cand_item.attempted_mask        = item.attempted_mask;
                cand_item.history               = item.history;

                PassResult result;
                result.decision    = PassDecision::kSolvedCandidate;
                result.disposition = ItemDisposition::kRetainCurrent;
                result.next.push_back(std::move(cand_item));
                return Ok(std::move(result));
            }
        }

        // Step 3: Eliminate auxiliary variables
        auto elim                 = EliminateAuxVars(sig, ctx.original_vars);
        const auto real_var_count = static_cast< uint32_t >(elim.real_vars.size());

        // Step 4: Check max_vars
        if (real_var_count > ctx.opts.max_vars) {
            return Err< PassResult >(
                CobraError::kTooManyVariables,
                "Variable count after elimination (" + std::to_string(real_var_count)
                    + ") exceeds max_vars (" + std::to_string(ctx.opts.max_vars) + ")"
            );
        }

        // Step 5: Build original_indices
        auto original_indices = BuildVarSupport(ctx.original_vars, elim.real_vars);

        // Step 6: Determine verification needs
        bool needs_verification = ctx.evaluator.has_value();

        // Step 7: Emit SignatureStatePayload
        WorkItem sig_item;
        sig_item.payload = SignatureStatePayload{
            .sig                               = std::move(sig),
            .real_vars                         = elim.real_vars,
            .elimination                       = std::move(elim),
            .original_indices                  = std::move(original_indices),
            .needs_original_space_verification = needs_verification,
        };
        sig_item.features       = item.features;
        sig_item.metadata       = item.metadata;
        sig_item.depth          = item.depth;
        sig_item.rewrite_gen    = item.rewrite_gen;
        sig_item.attempted_mask = item.attempted_mask;
        sig_item.history        = item.history;

        PassResult result;
        result.decision    = PassDecision::kAdvance;
        result.disposition = ItemDisposition::kRetainCurrent;
        result.next.push_back(std::move(sig_item));
        return Ok(std::move(result));
    }

    // ---------------------------------------------------------------
    // Solver passes (Task 10)
    // ---------------------------------------------------------------

    Result< PassResult > RunSupportedSolve(const WorkItem &item, OrchestratorContext &ctx) {
        if (!IsSignatureKind(item)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        const auto &sig_payload = std::get< SignatureStatePayload >(item.payload);

        // Build SignatureContext with mapped evaluator
        SignatureContext sig_ctx;
        sig_ctx.vars             = sig_payload.real_vars;
        sig_ctx.original_indices = sig_payload.original_indices;

        if (ctx.evaluator) {
            if (sig_payload.real_vars.size() == ctx.original_vars.size()) {
                sig_ctx.eval = *ctx.evaluator;
            } else {
                sig_ctx.eval = [eval = *ctx.evaluator, idx_map = sig_payload.original_indices,
                                original_vals =
                                    std::vector< uint64_t >(ctx.original_vars.size(), 0)](
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
                };
            }
        }

        // For non-MixedRewrite routes, propagate structural_flags
        // from the classification to match the legacy's
        //   pipeline_opts.structural_flags = cls.flags
        // in the kBitwiseOnly/kMultilinear/kPowerRecovery branch.
        Options solve_opts = ctx.opts;
        if (item.features.classification
            && item.features.classification->route != Route::kMixedRewrite)
        {
            solve_opts.structural_flags = item.features.classification->flags;
        }

        auto sub =
            SimplifyFromSignature(sig_payload.elimination.reduced_sig, sig_ctx, solve_opts, 0);

        if (sub.Succeeded()) {
            auto payload = sub.TakePayload();

            WorkItem cand_item;
            cand_item.payload = CandidatePayload{
                .expr           = std::move(payload.expr),
                .real_vars      = sig_payload.real_vars,
                .cost           = payload.cost,
                .producing_pass = PassId::kSupportedSolve,
                .needs_original_space_verification =
                    sig_payload.needs_original_space_verification,
            };
            cand_item.features              = item.features;
            cand_item.metadata              = item.metadata;
            cand_item.metadata.sig_vector   = sig_payload.elimination.reduced_sig;
            cand_item.metadata.verification = payload.verification;
            cand_item.depth                 = item.depth;
            cand_item.rewrite_gen           = item.rewrite_gen;
            cand_item.attempted_mask        = item.attempted_mask;
            cand_item.history               = item.history;

            PassResult result;
            result.decision    = PassDecision::kSolvedCandidate;
            result.disposition = ItemDisposition::kConsumeCurrent;
            result.next.push_back(std::move(cand_item));
            return Ok(std::move(result));
        }

        // Translate solver failure
        ReasonDetail reason = sub.Reason();
        PassDecision decision;
        switch (sub.Kind()) {
            case OutcomeKind::kInapplicable:
                decision = PassDecision::kNoProgress;
                break;
            case OutcomeKind::kBlocked:
                decision = PassDecision::kBlocked;
                break;
            case OutcomeKind::kVerifyFailed:
                decision = PassDecision::kNoProgress;
                break;
            default:
                decision = PassDecision::kBlocked;
                break;
        }

        return Ok(
            PassResult{
                .decision    = decision,
                .disposition = ItemDisposition::kRetainCurrent,
                .reason      = std::move(reason),
            }
        );
    }

    Result< PassResult > RunTrySemilinearPass(const WorkItem &item, OrchestratorContext &ctx) {
        if (!IsAstKind(item)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        const auto &ast = std::get< AstPayload >(item.payload);
        if (ast.provenance == Provenance::kLowered) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        if (!ast.classification || ast.classification->semantic != SemanticClass::kSemilinear) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        const auto num_vars = static_cast< uint32_t >(ctx.original_vars.size());
        if (IsLinearShortcut(*ast.expr, num_vars, ctx.bitwidth)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        auto semi = internal::TrySemilinearPipeline(*ast.expr, ctx.original_vars, ctx.opts);

        if (semi.Succeeded()) {
            auto cost_info = ComputeCost(semi.GetExpr());

            WorkItem cand_item;
            cand_item.payload = CandidatePayload{
                .expr                              = semi.TakeExpr(),
                .real_vars                         = semi.RealVars(),
                .cost                              = cost_info.cost,
                .producing_pass                    = PassId::kTrySemilinearPass,
                .needs_original_space_verification = false,
            };
            cand_item.features              = item.features;
            cand_item.metadata              = item.metadata;
            cand_item.metadata.verification = VerificationState::kVerified;
            cand_item.depth                 = item.depth;
            cand_item.rewrite_gen           = item.rewrite_gen;
            cand_item.attempted_mask        = item.attempted_mask;
            cand_item.history               = item.history;

            PassResult result;
            result.decision    = PassDecision::kSolvedCandidate;
            result.disposition = ItemDisposition::kRetainCurrent;
            result.next.push_back(std::move(cand_item));
            return Ok(std::move(result));
        }

        auto failure_reason                 = semi.Reason();
        ctx.run_metadata.semilinear_failure = failure_reason;
        return Ok(
            PassResult{
                .decision    = PassDecision::kBlocked,
                .disposition = ItemDisposition::kRetainCurrent,
                .reason      = std::move(failure_reason),
            }
        );
    }

    Result< PassResult > RunDecompose(const WorkItem &item, OrchestratorContext &ctx) {
        if (!IsAstKind(item)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        const auto &ast = std::get< AstPayload >(item.payload);
        if (!ast.classification || ast.classification->route != Route::kMixedRewrite) {
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

        const auto num_vars = static_cast< uint32_t >(ctx.original_vars.size());
        // Use the parser's input signature for the initial
        // (non-rewritten) AST to match legacy's working_sig exactly.
        bool use_input_sig =
            !ctx.input_sig.empty() && !ctx.lowering_fired && item.rewrite_gen == 0;
        auto decomp_sig = use_input_sig
            ? ctx.input_sig
            : EvaluateBooleanSignature(*ast.expr, num_vars, ctx.bitwidth);

        DecompositionContext dctx{
            .opts         = ctx.opts,
            .vars         = ctx.original_vars,
            .sig          = decomp_sig,
            .current_expr = ast.expr.get(),
            .cls          = *ast.classification,
        };

        auto decomp = TryDecomposition(dctx);

        if (decomp.Succeeded()) {
            auto cost_info = ComputeCost(decomp.GetExpr());

            WorkItem cand_item;
            cand_item.payload = CandidatePayload{
                .expr                              = decomp.TakeExpr(),
                .real_vars                         = decomp.RealVars(),
                .cost                              = cost_info.cost,
                .producing_pass                    = PassId::kExtractProductCore,
                .needs_original_space_verification = false,
            };
            cand_item.features              = item.features;
            cand_item.metadata              = item.metadata;
            cand_item.metadata.sig_vector   = decomp_sig;
            cand_item.metadata.verification = VerificationState::kVerified;
            if (decomp.DecompositionMetadata()) {
                cand_item.metadata.decomposition_meta = *decomp.DecompositionMetadata();
            }
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

        ReasonDetail reason = decomp.Reason();
        PassResult fail_result;
        fail_result.decision    = PassDecision::kBlocked;
        fail_result.disposition = ItemDisposition::kRetainCurrent;
        fail_result.reason      = std::move(reason);
        return Ok(std::move(fail_result));
    }

    // ---------------------------------------------------------------
    // Rewrite passes and verification (Task 11)
    // ---------------------------------------------------------------

    Result< PassResult > RunOperandSimplify(const WorkItem &item, OrchestratorContext &ctx) {
        if (!IsAstKind(item)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        const auto &ast = std::get< AstPayload >(item.payload);
        auto opsimpl = SimplifyMixedOperands(CloneExpr(*ast.expr), ctx.original_vars, ctx.opts);

        if (!opsimpl.changed) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNoProgress,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        auto new_cls = ClassifyStructural(*opsimpl.expr);

        WorkItem rewritten;
        rewritten.payload = AstPayload{
            .expr           = std::move(opsimpl.expr),
            .classification = new_cls,
            .provenance     = Provenance::kRewritten,
        };
        rewritten.features                = item.features;
        rewritten.features.classification = new_cls;
        rewritten.features.provenance     = Provenance::kRewritten;
        rewritten.metadata                = item.metadata;
        rewritten.depth                   = item.depth;
        rewritten.rewrite_gen             = item.rewrite_gen + 1;
        rewritten.attempted_mask          = 0;
        rewritten.history                 = item.history;

        PassResult result;
        result.decision    = PassDecision::kAdvance;
        result.disposition = ItemDisposition::kReplaceCurrent;
        result.next.push_back(std::move(rewritten));
        return Ok(std::move(result));
    }

    Result< PassResult >
    RunProductIdentityCollapse(const WorkItem &item, OrchestratorContext &ctx) {
        if (!IsAstKind(item)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        const auto &ast = std::get< AstPayload >(item.payload);
        auto collapse =
            CollapseProductIdentities(CloneExpr(*ast.expr), ctx.original_vars, ctx.opts);

        if (!collapse.changed) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNoProgress,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        auto new_cls = ClassifyStructural(*collapse.expr);

        WorkItem rewritten;
        rewritten.payload = AstPayload{
            .expr           = std::move(collapse.expr),
            .classification = new_cls,
            .provenance     = Provenance::kRewritten,
        };
        rewritten.features                = item.features;
        rewritten.features.classification = new_cls;
        rewritten.features.provenance     = Provenance::kRewritten;
        rewritten.metadata                = item.metadata;
        rewritten.depth                   = item.depth;
        rewritten.rewrite_gen             = item.rewrite_gen + 1;
        rewritten.attempted_mask          = 0;
        rewritten.history                 = item.history;

        PassResult result;
        result.decision    = PassDecision::kAdvance;
        result.disposition = ItemDisposition::kReplaceCurrent;
        result.next.push_back(std::move(rewritten));
        return Ok(std::move(result));
    }

    Result< PassResult > RunXorLowering(const WorkItem &item, OrchestratorContext &ctx) {
        if (!IsAstKind(item)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        const auto &ast = std::get< AstPayload >(item.payload);

        RewriteOptions rw_opts;
        rw_opts.max_rounds      = 2;
        rw_opts.max_node_growth = 3;
        rw_opts.bitwidth        = ctx.bitwidth;

        auto rw = RewriteMixedProducts(CloneExpr(*ast.expr), rw_opts);

        if (!rw.structure_changed) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kBlocked,
                    .disposition = ItemDisposition::kRetainCurrent,
                    .reason =
                        ReasonDetail{
                                     .top = { .code    = { ReasonCategory::kSearchExhausted,
                                                  ReasonDomain::kMixedRewrite },
                                     .message = "No rewrite applied" },
                                     },
            }
            );
        }

        auto new_cls = ClassifyStructural(*rw.expr);

        // Still mixed or unsupported after rewrite: short-circuit
        // with kRepresentationGap so parity is preserved.
        if (new_cls.route == Route::kMixedRewrite || new_cls.route == Route::kUnsupported) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kBlocked,
                    .disposition = ItemDisposition::kRetainCurrent,
                    .reason =
                        ReasonDetail{
                                     .top = { .code = { ReasonCategory::kRepresentationGap,
                                               ReasonDomain::kMixedRewrite },
                                     .message =
                                         "Rewrite did not reduce to supported structure" },
                                     },
            }
            );
        }

        WorkItem rewritten;
        rewritten.payload = AstPayload{
            .expr           = std::move(rw.expr),
            .classification = new_cls,
            .provenance     = Provenance::kRewritten,
        };
        rewritten.features                             = item.features;
        rewritten.features.classification              = new_cls;
        rewritten.features.provenance                  = Provenance::kRewritten;
        rewritten.metadata                             = item.metadata;
        rewritten.metadata.structural_transform_rounds = rw.rounds_applied;
        rewritten.depth                                = item.depth;
        rewritten.rewrite_gen                          = item.rewrite_gen + 1;
        rewritten.attempted_mask                       = 0;
        rewritten.history                              = item.history;

        PassResult result;
        result.decision    = PassDecision::kAdvance;
        result.disposition = ItemDisposition::kReplaceCurrent;
        result.next.push_back(std::move(rewritten));
        return Ok(std::move(result));
    }

    // ---------------------------------------------------------------
    // Pass registry
    // ---------------------------------------------------------------

    const std::vector< PassDescriptor > &GetPassRegistry() {
        static const std::vector< PassDescriptor > kRegistry = {
            {
             .id         = PassId::kLowerNotOverArith,
             .consumes   = StateKind::kFoldedAst,
             .tag        = PassTag::kAnalysis,
             .applicable = [](const WorkItem &item,       const OrchestratorContext & /*ctx*/)
       -> bool { return IsAstKind(item); },
             .run = RunLowerNotOverArith,
             },
            {
             .id         = PassId::kClassifyAst,
             .consumes   = StateKind::kFoldedAst,
             .tag        = PassTag::kAnalysis,
             .applicable = [](const WorkItem &item,       const OrchestratorContext & /*ctx*/)
       -> bool { return IsAstKind(item); },
             .run = RunClassifyAst,
             },
            {
             .id         = PassId::kBuildSignatureState,
             .consumes   = StateKind::kFoldedAst,
             .tag        = PassTag::kAnalysis,
             .applicable = [](const WorkItem &item,       const OrchestratorContext & /*ctx*/)
       -> bool { return IsAstKind(item); },
             .run = RunBuildSignatureState,
             },
            {
             .id         = PassId::kSupportedSolve,
             .consumes   = StateKind::kSignatureState,
             .tag        = PassTag::kSolver,
             .applicable = [](const WorkItem &item, const OrchestratorContext & /*ctx*/)
 -> bool { return IsSignatureKind(item); },
             .run = RunSupportedSolve,
             },
            {
             .id         = PassId::kTrySemilinearPass,
             .consumes   = StateKind::kFoldedAst,
             .tag        = PassTag::kSolver,
             .applicable = [](const WorkItem &item,       const OrchestratorContext & /*ctx*/)
       -> bool { return IsAstKind(item); },
             .run = RunTrySemilinearPass,
             },
            {
             .id         = PassId::kExtractProductCore,
             .consumes   = StateKind::kFoldedAst,
             .tag        = PassTag::kSolver,
             .applicable = [](const WorkItem &item,       const OrchestratorContext & /*ctx*/)
       -> bool { return IsAstKind(item); },
             .run = RunDecompose,
             },
            {
             .id         = PassId::kOperandSimplify,
             .consumes   = StateKind::kFoldedAst,
             .tag        = PassTag::kRewrite,
             .applicable = [](const WorkItem &item,       const OrchestratorContext & /*ctx*/)
       -> bool { return IsAstKind(item); },
             .run = RunOperandSimplify,
             },
            {
             .id         = PassId::kProductIdentityCollapse,
             .consumes   = StateKind::kFoldedAst,
             .tag        = PassTag::kRewrite,
             .applicable = [](const WorkItem &item,       const OrchestratorContext & /*ctx*/)
       -> bool { return IsAstKind(item); },
             .run = RunProductIdentityCollapse,
             },
            {
             .id         = PassId::kXorLowering,
             .consumes   = StateKind::kFoldedAst,
             .tag        = PassTag::kRewrite,
             .applicable = [](const WorkItem &item,       const OrchestratorContext & /*ctx*/)
       -> bool { return IsAstKind(item); },
             .run = RunXorLowering,
             },
            {
             .id         = PassId::kVerifyCandidate,
             .consumes   = StateKind::kCandidateExpr,
             .tag        = PassTag::kVerifier,
             .applicable = [](const WorkItem &item, const OrchestratorContext & /*ctx*/)
 -> bool { return IsCandidateKind(item); },
             .run = RunVerifyCandidate,
             },
        };
        return kRegistry;
    }

} // namespace cobra
