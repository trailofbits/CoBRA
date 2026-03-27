#include "OrchestratorPasses.h"
#include "CompetitionGroup.h"
#include "ContinuationTypes.h"
#include "DecompositionPassHelpers.h"
#include "JoinState.h"
#include "LiftingPasses.h"
#include "SemilinearPasses.h"
#include "SignaturePasses.h"
#include "SimplifierInternal.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/DecompositionEngine.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/MixedProductRewriter.h"
#include "cobra/core/PatternMatcher.h"
#include "cobra/core/SignatureEval.h"

#include <functional>
#include <numeric>
#include <optional>

namespace cobra {

    namespace {

        bool IsAstKind(const WorkItem &item) {
            return std::holds_alternative< AstPayload >(item.payload);
        }

        std::optional< AstSolveContext >
        CloneSolveContext(const std::optional< AstSolveContext > &solve_ctx) {
            if (!solve_ctx.has_value()) { return std::nullopt; }
            return AstSolveContext{
                .vars      = solve_ctx->vars,
                .evaluator = solve_ctx->evaluator,
                .input_sig = solve_ctx->input_sig,
            };
        }

        bool IsCandidateKind(const WorkItem &item) {
            return std::holds_alternative< CandidatePayload >(item.payload);
        }

        struct OperandSite
        {
            const Expr *mul;
            size_t mul_hash;
            bool lhs_bitwise;
            bool rhs_bitwise;
        };

        std::optional< OperandSite > FindFirstOperandSite(const Expr &root) {
            if (root.kind == Expr::Kind::kMul && root.children.size() == 2) {
                bool lhs_vd = HasVarDep(*root.children[0]);
                bool rhs_vd = HasVarDep(*root.children[1]);
                if (lhs_vd && rhs_vd) {
                    bool lhs_bw = HasNonleafBitwise(*root.children[0]);
                    bool rhs_bw = HasNonleafBitwise(*root.children[1]);
                    if (lhs_bw || rhs_bw) {
                        return OperandSite{
                            .mul         = &root,
                            .mul_hash    = std::hash< Expr >{}(root),
                            .lhs_bitwise = lhs_bw,
                            .rhs_bitwise = rhs_bw,
                        };
                    }
                }
            }
            for (const auto &child : root.children) {
                auto result = FindFirstOperandSite(*child);
                if (result.has_value()) { return result; }
            }
            return std::nullopt;
        }

        struct ProductSite
        {
            const Expr *add_node;
            size_t add_hash;
        };

        std::optional< ProductSite > FindFirstProductSite(const Expr &root) {
            if (root.kind == Expr::Kind::kAdd && root.children.size() == 2) {
                bool lhs_mul2 = root.children[0]->kind == Expr::Kind::kMul
                    && root.children[0]->children.size() == 2;
                bool rhs_mul2 = root.children[1]->kind == Expr::Kind::kMul
                    && root.children[1]->children.size() == 2;
                if (lhs_mul2 && rhs_mul2) {
                    return ProductSite{
                        .add_node = &root,
                        .add_hash = std::hash< Expr >{}(root),
                    };
                }
            }
            for (const auto &child : root.children) {
                auto result = FindFirstProductSite(*child);
                if (result.has_value()) { return result; }
            }
            return std::nullopt;
        }

        struct ProductAssignment
        {
            std::vector< uint64_t > sig_x;
            std::vector< uint64_t > sig_y;
        };

        std::vector< ProductAssignment > EnumerateValidAssignments(
            const Expr &add_node, uint32_t num_vars, uint32_t bitwidth,
            uint32_t max_assignments = 4
        ) {
            const auto sig_len  = size_t{ 1 } << num_vars;
            const uint64_t mask = Bitmask(bitwidth);

            const Expr *factors[4] = {
                add_node.children[0]->children[0].get(),
                add_node.children[0]->children[1].get(),
                add_node.children[1]->children[0].get(),
                add_node.children[1]->children[1].get(),
            };

            std::array< std::vector< uint64_t >, 4 > sigs;
            for (int i = 0; i < 4; ++i) {
                sigs[i] = EvaluateBooleanSignature(*factors[i], num_vars, bitwidth);
            }

            struct Roles
            {
                int i, o, l, r;
            };

            static constexpr Roles kAssignments[8] = {
                { .i = 0, .o = 1, .l = 2, .r = 3 },
                { .i = 1, .o = 0, .l = 2, .r = 3 },
                { .i = 0, .o = 1, .l = 3, .r = 2 },
                { .i = 1, .o = 0, .l = 3, .r = 2 },
                { .i = 2, .o = 3, .l = 0, .r = 1 },
                { .i = 3, .o = 2, .l = 0, .r = 1 },
                { .i = 2, .o = 3, .l = 1, .r = 0 },
                { .i = 3, .o = 2, .l = 1, .r = 0 },
            };

            std::vector< ProductAssignment > out;
            std::vector< uint64_t > sig_x(sig_len);
            std::vector< uint64_t > sig_y(sig_len);

            for (const auto &a : kAssignments) {
                const auto &sig_i = sigs[a.i];
                const auto &sig_o = sigs[a.o];
                const auto &sig_l = sigs[a.l];
                const auto &sig_r = sigs[a.r];

                bool ok = true;
                for (size_t j = 0; j < sig_len; ++j) {
                    const uint64_t mi = sig_i[j] & mask;
                    const uint64_t mo = sig_o[j] & mask;
                    const uint64_t ml = sig_l[j] & mask;
                    const uint64_t mr = sig_r[j] & mask;
                    if (((mi & ml) | (mi & mr) | (ml & mr)) != 0u) {
                        ok = false;
                        break;
                    }
                    if (mo != (mi | ml | mr)) {
                        ok = false;
                        break;
                    }
                }
                if (!ok) { continue; }

                for (size_t j = 0; j < sig_len; ++j) {
                    sig_x[j] = (sig_i[j] | sig_l[j]) & mask;
                    sig_y[j] = (sig_i[j] | sig_r[j]) & mask;
                }

                out.push_back(ProductAssignment{ .sig_x = sig_x, .sig_y = sig_y });
                if (out.size() >= max_assignments) { break; }
            }

            return out;
        }

    } // namespace

    // ---------------------------------------------------------------
    // Analysis passes (Task 9)
    // ---------------------------------------------------------------

    Result< PassResult > RunLowerNotOverArith(const WorkItem &item, OrchestratorContext &ctx) {
        if (!IsAstKind(item)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        const auto &ast         = std::get< AstPayload >(item.payload);
        const auto &active_vars = ActiveAstVars(item, ctx);
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

        const auto num_vars = static_cast< uint32_t >(active_vars.size());
        auto new_sig        = EvaluateBooleanSignature(*lowered, num_vars, ctx.bitwidth);
        auto solve_ctx      = CloneSolveContext(ast.solve_ctx);
        if (solve_ctx.has_value()) { solve_ctx->input_sig = new_sig; }

        WorkItem new_item;
        new_item.payload = AstPayload{
            .expr           = std::move(lowered),
            .classification = ast.classification,
            .provenance     = Provenance::kLowered,
            .solve_ctx      = std::move(solve_ctx),
        };
        new_item.features            = item.features;
        new_item.features.provenance = Provenance::kLowered;
        new_item.metadata            = item.metadata;
        new_item.metadata.sig_vector = std::move(new_sig);
        new_item.depth               = item.depth;
        new_item.rewrite_gen         = item.rewrite_gen;
        new_item.attempted_mask      = item.attempted_mask;
        new_item.group_id            = item.group_id;
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
            .solve_ctx      = CloneSolveContext(ast.solve_ctx),
        };
        new_item.features                = item.features;
        new_item.features.classification = cls;
        new_item.metadata                = item.metadata;
        new_item.depth                   = item.depth;
        new_item.rewrite_gen             = item.rewrite_gen;
        new_item.attempted_mask          = item.attempted_mask;
        new_item.group_id                = item.group_id;
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

        const auto &ast         = std::get< AstPayload >(item.payload);
        const auto &active_vars = ActiveAstVars(item, ctx);
        const auto &active_eval = ActiveAstEvaluator(item, ctx);
        const auto *active_sig  = ActiveAstInputSig(item, ctx);
        const auto num_vars     = static_cast< uint32_t >(active_vars.size());

        // Step 1: Compute signature.
        // For the initial (non-rewritten) item, use the parser's
        // input signature when NOT-over-arith lowering was a no-op.
        // This matches the legacy's working_sig exactly.  For
        // lowered or rewritten items, recompute from the AST.
        bool use_input_sig =
            active_sig != nullptr && !ctx.lowering_fired && item.rewrite_gen == 0;
        auto sig = use_input_sig ? *active_sig
                                 : EvaluateBooleanSignature(*ast.expr, num_vars, ctx.bitwidth);

        // Step 2: Constant match
        {
            auto pm = MatchPattern(sig, num_vars, ctx.bitwidth);
            if (pm && (*pm)->kind == Expr::Kind::kConstant) {
                bool needs_verify = active_eval.has_value();
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
                cand_item.group_id              = item.group_id;

                PassResult result;
                result.decision    = PassDecision::kSolvedCandidate;
                result.disposition = ItemDisposition::kRetainCurrent;
                result.next.push_back(std::move(cand_item));
                return Ok(std::move(result));
            }
        }

        // Step 3: Eliminate auxiliary variables
        auto elim                 = EliminateAuxVars(sig, active_vars);
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
        auto original_indices = BuildVarSupport(active_vars, elim.real_vars);

        // Step 6: Determine verification needs
        bool needs_verification = active_eval.has_value();

        // Step 7: Reuse incoming group or create a fresh one
        GroupId group_id;
        if (item.group_id.has_value()) {
            group_id = *item.group_id;
            AcquireHandle(ctx.competition_groups, group_id);
        } else {
            group_id = CreateGroup(ctx.competition_groups, ctx.next_group_id);
        }

        // Step 8: Emit SignatureStatePayload
        WorkItem sig_item;
        sig_item.payload = SignatureStatePayload{
            .ctx = {
                .sig                               = std::move(sig),
                .real_vars                         = elim.real_vars,
                .elimination                       = std::move(elim),
                .original_indices                  = std::move(original_indices),
                .needs_original_space_verification = needs_verification,
            },
        };
        sig_item.features       = item.features;
        sig_item.metadata       = item.metadata;
        sig_item.depth          = item.depth;
        sig_item.rewrite_gen    = item.rewrite_gen;
        sig_item.attempted_mask = item.attempted_mask;
        sig_item.group_id       = group_id;
        sig_item.history        = item.history;

        // Thread subproblem-local evaluator so BuildMappedEvaluator
        // verifies against the reduced outer function, not the
        // top-level ctx.evaluator.
        if (auto *a = std::get_if< AstPayload >(&item.payload)) {
            if (a->solve_ctx.has_value() && a->solve_ctx->evaluator.has_value()) {
                sig_item.evaluator_override = *a->solve_ctx->evaluator;
            }
        }

        PassResult result;
        result.decision    = PassDecision::kAdvance;
        result.disposition = ItemDisposition::kRetainCurrent;
        result.next.push_back(std::move(sig_item));
        return Ok(std::move(result));
    }

    // ---------------------------------------------------------------
    // Rewrite passes and verification (Task 11)
    // ---------------------------------------------------------------

    Result< PassResult > RunOperandSimplify(const WorkItem &item, OrchestratorContext &ctx) {
        if (!IsAstKind(item)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }
        const auto &ast = std::get< AstPayload >(item.payload);

        auto site = FindFirstOperandSite(*ast.expr);
        if (!site.has_value()) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNoProgress,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        const auto &active_vars = ActiveAstVars(item, ctx);
        const auto num_vars     = static_cast< uint32_t >(active_vars.size());
        auto baseline_cost      = ComputeCost(*site->mul).cost;

        OperandJoinState join;
        join.full_ast        = CloneExpr(*ast.expr);
        join.original_mul    = CloneExpr(*site->mul);
        join.target_hash     = site->mul_hash;
        join.baseline_cost   = baseline_cost;
        join.vars            = active_vars;
        join.parent_group_id = item.group_id;
        if (ast.solve_ctx.has_value()) {
            join.has_solve_ctx       = true;
            join.solve_ctx_vars      = ast.solve_ctx->vars;
            join.solve_ctx_evaluator = ast.solve_ctx->evaluator;
            join.solve_ctx_input_sig = ast.solve_ctx->input_sig;
        }
        join.bitwidth       = ctx.bitwidth;
        join.parent_depth   = item.depth;
        join.rewrite_gen    = item.rewrite_gen;
        join.parent_history = item.history;

        auto join_id =
            CreateJoin(ctx.join_states, ctx.next_join_id, JoinState{ std::move(join) });

        // Pre-resolve non-bitwise sides so ResolveOperandRewrite
        // does not wait forever for a child that was never emitted.
        auto &stored_join = std::get< OperandJoinState >(ctx.join_states.at(join_id));
        if (!site->lhs_bitwise) { stored_join.lhs_resolved = true; }
        if (!site->rhs_bitwise) { stored_join.rhs_resolved = true; }

        PassResult result;
        result.decision    = PassDecision::kAdvance;
        result.disposition = ItemDisposition::kConsumeCurrent;

        auto emit_child = [&](const Expr &operand, OperandRewriteCont::OperandRole role) {
            auto sig = EvaluateBooleanSignature(operand, num_vars, ctx.bitwidth);

            auto group_id      = CreateGroup(ctx.competition_groups, ctx.next_group_id);
            auto &group        = ctx.competition_groups.at(group_id);
            group.continuation = OperandRewriteCont{
                .join_id = join_id,
                .role    = role,
            };

            std::vector< uint32_t > indices(num_vars);
            std::iota(indices.begin(), indices.end(), 0U);

            WorkItem child;
            child.payload = SignatureStatePayload{
                    .ctx = {
                        .sig              = std::move(sig),
                        .real_vars        = active_vars,
                        .elimination      = { .reduced_sig = {}, .real_vars = active_vars },
                        .original_indices = std::move(indices),
                        .needs_original_space_verification = false,
                    },
                };
            child.features              = item.features;
            child.metadata              = item.metadata;
            child.depth                 = item.depth;
            child.rewrite_gen           = item.rewrite_gen;
            child.attempted_mask        = item.attempted_mask;
            child.group_id              = group_id;
            child.history               = item.history;
            // Copy sig into elimination.reduced_sig for technique passes.
            auto &sub                   = std::get< SignatureStatePayload >(child.payload).ctx;
            sub.elimination.reduced_sig = sub.sig;

            result.next.push_back(std::move(child));
        };

        if (site->lhs_bitwise) {
            emit_child(*site->mul->children[0], OperandRewriteCont::OperandRole::kLhs);
        }
        if (site->rhs_bitwise) {
            emit_child(*site->mul->children[1], OperandRewriteCont::OperandRole::kRhs);
        }

        return Ok(std::move(result));
    }

    Result< PassResult >
    RunProductIdentityCollapse(const WorkItem &item, OrchestratorContext &ctx) {
        if (!IsAstKind(item)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }
        const auto &ast = std::get< AstPayload >(item.payload);

        auto site = FindFirstProductSite(*ast.expr);
        if (!site.has_value()) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNoProgress,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        const auto &active_vars = ActiveAstVars(item, ctx);
        const auto num_vars     = static_cast< uint32_t >(active_vars.size());

        auto assignments = EnumerateValidAssignments(*site->add_node, num_vars, ctx.bitwidth);
        if (assignments.empty()) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNoProgress,
                    .disposition = ItemDisposition::kRetainCurrent,
                }
            );
        }

        auto baseline_cost = ComputeCost(*site->add_node).cost;

        PassResult result;
        result.decision    = PassDecision::kAdvance;
        result.disposition = ItemDisposition::kConsumeCurrent;

        std::vector< uint32_t > indices(num_vars);
        std::iota(indices.begin(), indices.end(), 0U);

        for (const auto &assign : assignments) {
            ProductJoinState join;
            join.original_expr   = CloneExpr(*site->add_node);
            join.baseline_cost   = baseline_cost;
            join.vars            = active_vars;
            join.parent_group_id = item.group_id;
            if (ast.solve_ctx.has_value()) {
                join.has_solve_ctx       = true;
                join.solve_ctx_vars      = ast.solve_ctx->vars;
                join.solve_ctx_evaluator = ast.solve_ctx->evaluator;
                join.solve_ctx_input_sig = ast.solve_ctx->input_sig;
            }
            join.bitwidth       = ctx.bitwidth;
            join.parent_depth   = item.depth;
            join.rewrite_gen    = item.rewrite_gen;
            join.parent_history = item.history;
            join.full_ast       = CloneExpr(*ast.expr);
            join.target_hash    = site->add_hash;

            auto join_id =
                CreateJoin(ctx.join_states, ctx.next_join_id, JoinState{ std::move(join) });

            auto x_group = CreateGroup(ctx.competition_groups, ctx.next_group_id);
            ctx.competition_groups.at(x_group).continuation = ProductCollapseCont{
                .join_id = join_id,
                .role    = ProductCollapseCont::FactorRole::kX,
            };

            WorkItem x_child;
            x_child.payload = SignatureStatePayload{
                .ctx = {
                    .sig              = assign.sig_x,
                    .real_vars        = active_vars,
                    .elimination      = { .reduced_sig = assign.sig_x,
                                     .real_vars = active_vars },
                    .original_indices = indices,
                    .needs_original_space_verification = false,
                },
            };
            x_child.features       = item.features;
            x_child.metadata       = item.metadata;
            x_child.depth          = item.depth;
            x_child.rewrite_gen    = item.rewrite_gen;
            x_child.attempted_mask = item.attempted_mask;
            x_child.group_id       = x_group;
            x_child.history        = item.history;
            result.next.push_back(std::move(x_child));

            auto y_group = CreateGroup(ctx.competition_groups, ctx.next_group_id);
            ctx.competition_groups.at(y_group).continuation = ProductCollapseCont{
                .join_id = join_id,
                .role    = ProductCollapseCont::FactorRole::kY,
            };

            WorkItem y_child;
            y_child.payload = SignatureStatePayload{
                .ctx = {
                    .sig              = assign.sig_y,
                    .real_vars        = active_vars,
                    .elimination      = { .reduced_sig = assign.sig_y,
                                     .real_vars = active_vars },
                    .original_indices = indices,
                    .needs_original_space_verification = false,
                },
            };
            y_child.features       = item.features;
            y_child.metadata       = item.metadata;
            y_child.depth          = item.depth;
            y_child.rewrite_gen    = item.rewrite_gen;
            y_child.attempted_mask = item.attempted_mask;
            y_child.group_id       = y_group;
            y_child.history        = item.history;
            result.next.push_back(std::move(y_child));
        }

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
                                                  ReasonDomain::kStructuralTransform },
                                     .message = "No rewrite applied" },
                                     },
            }
            );
        }

        auto new_cls = ClassifyStructural(*rw.expr);

        // Still has unrecovered mixed structure after rewrite:
        // short-circuit with kRepresentationGap so parity is preserved.
        if (NeedsStructuralRecovery(new_cls.flags)) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kBlocked,
                    .disposition = ItemDisposition::kRetainCurrent,
                    .reason =
                        ReasonDetail{
                                     .top = { .code = { ReasonCategory::kRepresentationGap,
                                               ReasonDomain::kStructuralTransform },
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
            .solve_ctx      = CloneSolveContext(ast.solve_ctx),
        };
        rewritten.features                             = item.features;
        rewritten.features.classification              = new_cls;
        rewritten.features.provenance                  = Provenance::kRewritten;
        rewritten.metadata                             = item.metadata;
        rewritten.metadata.structural_transform_rounds = rw.rounds_applied;
        rewritten.depth                                = item.depth;
        rewritten.rewrite_gen                          = item.rewrite_gen + 1;
        rewritten.attempted_mask                       = 0;
        rewritten.group_id                             = item.group_id;
        rewritten.history                              = item.history;

        PassResult result;
        result.decision    = PassDecision::kAdvance;
        result.disposition = ItemDisposition::kReplaceCurrent;
        result.next.push_back(std::move(rewritten));
        return Ok(std::move(result));
    }

    // ---------------------------------------------------------------
    // Lifting passes
    // ---------------------------------------------------------------

    Result< PassResult >
    RunPrepareLiftedOuterSolve(const WorkItem &item, OrchestratorContext &ctx) {
        auto *skel = std::get_if< LiftedSkeletonPayload >(&item.payload);
        if (skel == nullptr) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        auto group_id =
            CreateGroup(ctx.competition_groups, ctx.next_group_id, skel->baseline_cost);
        auto &group = ctx.competition_groups.at(group_id);

        LiftedSubstituteCont cont;
        cont.bindings.reserve(skel->bindings.size());
        for (const auto &b : skel->bindings) {
            cont.bindings.push_back(
                LiftedBinding{
                    .kind             = b.kind,
                    .outer_var_index  = b.outer_var_index,
                    .subtree          = CloneExpr(*b.subtree),
                    .structural_hash  = b.structural_hash,
                    .original_support = b.original_support,
                }
            );
        }
        cont.outer_vars         = skel->outer_ctx.vars;
        cont.original_var_count = skel->original_var_count;
        cont.original_vars      = ctx.original_vars;
        cont.source_sig         = skel->source_sig;
        if (ctx.evaluator.has_value()) { cont.original_eval = *ctx.evaluator; }
        group.continuation = std::move(cont);

        auto cls = ClassifyStructural(*skel->outer_expr);

        // Build a proper outer evaluator so downstream families
        // (decomposition, signature) can verify against the
        // reduced outer problem.  Use a shared_ptr so the
        // std::function (Evaluator) is copy-constructible.
        auto solve_ctx      = skel->outer_ctx; // copy before moving outer_expr
        auto shared_oe      = std::shared_ptr< Expr >(CloneExpr(*skel->outer_expr));
        auto outer_bw       = ctx.bitwidth;
        solve_ctx.evaluator = [shared_oe,
                               outer_bw](const std::vector< uint64_t > &vals) -> uint64_t {
            return EvaluateExpr(*shared_oe, vals, outer_bw);
        };

        WorkItem child;
        child.payload = AstPayload{
            .expr           = CloneExpr(*skel->outer_expr),
            .classification = cls,
            .provenance     = Provenance::kRewritten,
            .solve_ctx      = std::move(solve_ctx),
        };
        child.features.classification = cls;
        child.features.provenance     = Provenance::kRewritten;
        child.metadata                = item.metadata;
        child.depth                   = item.depth;
        child.rewrite_gen             = item.rewrite_gen;
        child.group_id                = group_id;
        child.history                 = item.history;

        PassResult result;
        result.decision    = PassDecision::kAdvance;
        result.disposition = ItemDisposition::kConsumeCurrent;
        result.next.push_back(std::move(child));
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
             .applicable = [](const WorkItem &item,const OrchestratorContext & /*ctx*/)
-> bool { return IsAstKind(item); },
             .run = RunLowerNotOverArith,
             },
            {
             .id         = PassId::kClassifyAst,
             .consumes   = StateKind::kFoldedAst,
             .tag        = PassTag::kAnalysis,
             .applicable = [](const WorkItem &item,                                    const OrchestratorContext & /*ctx*/)
                                    -> bool { return IsAstKind(item); },
             .run = RunClassifyAst,
             },
            {
             .id         = PassId::kBuildSignatureState,
             .consumes   = StateKind::kFoldedAst,
             .tag        = PassTag::kAnalysis,
             .applicable = [](const WorkItem &item,                                    const OrchestratorContext & /*ctx*/)
                                    -> bool { return IsAstKind(item); },
             .run = RunBuildSignatureState,
             },
            {
             .id         = PassId::kSemilinearNormalize,
             .consumes   = StateKind::kFoldedAst,
             .tag        = PassTag::kAnalysis,
             .applicable = [](const WorkItem &item, const OrchestratorContext & /*ctx*/)
 -> bool { return std::holds_alternative< AstPayload >(item.payload); },
             .run = RunSemilinearNormalize,
             },
            {
             .id         = PassId::kSemilinearCheck,
             .consumes   = StateKind::kSemilinearNormalizedIr,
             .tag        = PassTag::kAnalysis,
             .applicable = [](const WorkItem &item,
             const OrchestratorContext & /*ctx*/) -> bool {
             return std::holds_alternative< NormalizedSemilinearPayload >(item.payload);
             },                .run = RunSemilinearCheck,
             },
            {
             .id         = PassId::kSemilinearRewrite,
             .consumes   = StateKind::kSemilinearCheckedIr,
             .tag        = PassTag::kRewrite,
             .applicable = [](const WorkItem &item,
             const OrchestratorContext & /*ctx*/) -> bool {
             return std::holds_alternative< CheckedSemilinearPayload >(item.payload);
             },              .run = RunSemilinearRewrite,
             },
            {
             .id         = PassId::kSemilinearReconstruct,
             .consumes   = StateKind::kSemilinearRewrittenIr,
             .tag        = PassTag::kSolver,
             .applicable = [](const WorkItem &item,
             const OrchestratorContext & /*ctx*/) -> bool {
             return std::holds_alternative< RewrittenSemilinearPayload >(item.payload);
             },          .run = RunSemilinearReconstruct,
             },
            {
             .id         = PassId::kResolveCompetition,
             .consumes   = StateKind::kCompetitionResolved,
             .tag        = PassTag::kAnalysis,
             .applicable = [](const WorkItem &item,
             const OrchestratorContext & /*ctx*/) -> bool {
             return std::holds_alternative< CompetitionResolvedPayload >(item.payload);
             },             .run = RunResolveCompetition,
             },
            // Signature technique passes
            {
             .id         = PassId::kSignaturePatternMatch,
             .consumes   = StateKind::kSignatureState,
             .tag        = PassTag::kSolver,
             .applicable = [](const WorkItem &item,
             const OrchestratorContext & /*ctx*/) -> bool {
             return std::holds_alternative< SignatureStatePayload >(item.payload);
             },          .run = RunSignaturePatternMatch,
             },
            {
             .id         = PassId::kSignatureAnf,
             .consumes   = StateKind::kSignatureState,
             .tag        = PassTag::kSolver,
             .applicable = [](const WorkItem &item,
             const OrchestratorContext & /*ctx*/) -> bool {
             return std::holds_alternative< SignatureStatePayload >(item.payload);
             },                   .run = RunSignatureAnf,
             },
            {
             .id         = PassId::kPrepareCoeffModel,
             .consumes   = StateKind::kSignatureState,
             .tag        = PassTag::kAnalysis,
             .applicable = [](const WorkItem &item,
             const OrchestratorContext & /*ctx*/) -> bool {
             return std::holds_alternative< SignatureStatePayload >(item.payload);
             },              .run = RunPrepareCoeffModel,
             },
            {
             .id         = PassId::kSignatureCobCandidate,
             .consumes   = StateKind::kSignatureCoeffState,
             .tag        = PassTag::kSolver,
             .applicable = [](const WorkItem &item,
             const OrchestratorContext & /*ctx*/) -> bool {
             return std::holds_alternative< SignatureCoeffStatePayload >(item.payload);
             },          .run = RunSignatureCobCandidate,
             },
            {
             .id         = PassId::kSignatureSingletonPolyRecovery,
             .consumes   = StateKind::kSignatureCoeffState,
             .tag        = PassTag::kSolver,
             .applicable = [](const WorkItem &item,
             const OrchestratorContext & /*ctx*/) -> bool {
             return std::holds_alternative< SignatureCoeffStatePayload >(item.payload);
             }, .run = RunSignatureSingletonPolyRecovery,
             },
            {
             .id         = PassId::kSignatureMultivarPolyRecovery,
             .consumes   = StateKind::kSignatureState,
             .tag        = PassTag::kSolver,
             .applicable = [](const WorkItem &item,
             const OrchestratorContext & /*ctx*/) -> bool {
             return std::holds_alternative< SignatureStatePayload >(item.payload);
             },  .run = RunSignatureMultivarPolyRecovery,
             },
            {
             .id         = PassId::kSignatureBitwiseDecompose,
             .consumes   = StateKind::kSignatureState,
             .tag        = PassTag::kSolver,
             .applicable = [](const WorkItem &item,
             const OrchestratorContext & /*ctx*/) -> bool {
             return std::holds_alternative< SignatureStatePayload >(item.payload);
             },      .run = RunSignatureBitwiseDecompose,
             },
            {
             .id         = PassId::kSignatureHybridDecompose,
             .consumes   = StateKind::kSignatureState,
             .tag        = PassTag::kSolver,
             .applicable = [](const WorkItem &item,
             const OrchestratorContext & /*ctx*/) -> bool {
             return std::holds_alternative< SignatureStatePayload >(item.payload);
             },       .run = RunSignatureHybridDecompose,
             },
            {
             .id         = PassId::kExtractProductCore,
             .consumes   = StateKind::kFoldedAst,
             .tag        = PassTag::kSolver,
             .applicable = [](const WorkItem &item,                                    const OrchestratorContext & /*ctx*/)
                                    -> bool { return IsAstKind(item); },
             .run = RunExtractProductCore,
             },
            {
             .id         = PassId::kExtractPolyCoreD2,
             .consumes   = StateKind::kFoldedAst,
             .tag        = PassTag::kSolver,
             .applicable = [](const WorkItem &item,                                    const OrchestratorContext & /*ctx*/)
                                    -> bool { return IsAstKind(item); },
             .run = RunExtractPolyCoreD2,
             },
            {
             .id         = PassId::kExtractTemplateCore,
             .consumes   = StateKind::kFoldedAst,
             .tag        = PassTag::kSolver,
             .applicable = [](const WorkItem &item,                                    const OrchestratorContext & /*ctx*/)
                                    -> bool { return IsAstKind(item); },
             .run = RunExtractTemplateCore,
             },
            {
             .id         = PassId::kExtractPolyCoreD3,
             .consumes   = StateKind::kFoldedAst,
             .tag        = PassTag::kSolver,
             .applicable = [](const WorkItem &item,                                    const OrchestratorContext & /*ctx*/)
                                    -> bool { return IsAstKind(item); },
             .run = RunExtractPolyCoreD3,
             },
            {
             .id         = PassId::kExtractPolyCoreD4,
             .consumes   = StateKind::kFoldedAst,
             .tag        = PassTag::kSolver,
             .applicable = [](const WorkItem &item,                                    const OrchestratorContext & /*ctx*/)
                                    -> bool { return IsAstKind(item); },
             .run = RunExtractPolyCoreD4,
             },
            // Decomposition prep passes
            {
             .id         = PassId::kPrepareDirectRemainder,
             .consumes   = StateKind::kFoldedAst,
             .tag        = PassTag::kAnalysis,
             .applicable = [](const WorkItem &item,                                    const OrchestratorContext & /*ctx*/)
                                    -> bool { return IsAstKind(item); },
             .run = RunPrepareDirectRemainder,
             },
            {
             .id         = PassId::kPrepareRemainderFromCore,
             .consumes   = StateKind::kCoreCandidate,
             .tag        = PassTag::kAnalysis,
             .applicable = [](const WorkItem &item,
             const OrchestratorContext & /*ctx*/) -> bool {
             return std::holds_alternative< CoreCandidatePayload >(item.payload);
             },       .run = RunPrepareRemainderFromCore,
             },
            // Decomposition remainder solvers
            {
             .id         = PassId::kResidualSupported,
             .consumes   = StateKind::kRemainderState,
             .tag        = PassTag::kSolver,
             .applicable = [](const WorkItem &item,
             const OrchestratorContext & /*ctx*/) -> bool {
             return std::holds_alternative< RemainderStatePayload >(item.payload);
             },              .run = RunResidualSupported,
             },
            {
             .id         = PassId::kResidualPolyRecovery,
             .consumes   = StateKind::kRemainderState,
             .tag        = PassTag::kSolver,
             .applicable = [](const WorkItem &item,
             const OrchestratorContext & /*ctx*/) -> bool {
             return std::holds_alternative< RemainderStatePayload >(item.payload);
             },           .run = RunResidualPolyRecovery,
             },
            {
             .id         = PassId::kResidualGhost,
             .consumes   = StateKind::kRemainderState,
             .tag        = PassTag::kSolver,
             .applicable = [](const WorkItem &item,
             const OrchestratorContext & /*ctx*/) -> bool {
             return std::holds_alternative< RemainderStatePayload >(item.payload);
             },                  .run = RunResidualGhost,
             },
            {
             .id         = PassId::kResidualFactoredGhost,
             .consumes   = StateKind::kRemainderState,
             .tag        = PassTag::kSolver,
             .applicable = [](const WorkItem &item,
             const OrchestratorContext & /*ctx*/) -> bool {
             return std::holds_alternative< RemainderStatePayload >(item.payload);
             },          .run = RunResidualFactoredGhost,
             },
            {
             .id         = PassId::kResidualFactoredGhostEscalated,
             .consumes   = StateKind::kRemainderState,
             .tag        = PassTag::kSolver,
             .applicable = [](const WorkItem &item,
             const OrchestratorContext & /*ctx*/) -> bool {
             return std::holds_alternative< RemainderStatePayload >(item.payload);
             }, .run = RunResidualFactoredGhostEscalated,
             },
            {
             .id         = PassId::kResidualTemplate,
             .consumes   = StateKind::kRemainderState,
             .tag        = PassTag::kSolver,
             .applicable = [](const WorkItem &item,
             const OrchestratorContext & /*ctx*/) -> bool {
             return std::holds_alternative< RemainderStatePayload >(item.payload);
             },               .run = RunResidualTemplate,
             },
            {
             .id         = PassId::kOperandSimplify,
             .consumes   = StateKind::kFoldedAst,
             .tag        = PassTag::kRewrite,
             .applicable = [](const WorkItem &item,                                    const OrchestratorContext & /*ctx*/)
                                    -> bool { return IsAstKind(item); },
             .run = RunOperandSimplify,
             },
            {
             .id         = PassId::kProductIdentityCollapse,
             .consumes   = StateKind::kFoldedAst,
             .tag        = PassTag::kRewrite,
             .applicable = [](const WorkItem &item,                                    const OrchestratorContext & /*ctx*/)
                                    -> bool { return IsAstKind(item); },
             .run = RunProductIdentityCollapse,
             },
            {
             .id         = PassId::kXorLowering,
             .consumes   = StateKind::kFoldedAst,
             .tag        = PassTag::kRewrite,
             .applicable = [](const WorkItem &item,                                    const OrchestratorContext & /*ctx*/)
                                    -> bool { return IsAstKind(item); },
             .run = RunXorLowering,
             },
            {
             .id         = PassId::kLiftArithmeticAtoms,
             .consumes   = StateKind::kFoldedAst,
             .tag        = PassTag::kRewrite,
             .applicable = [](const WorkItem &item, const OrchestratorContext & /*ctx*/)
 -> bool { return std::holds_alternative< AstPayload >(item.payload); },
             .run = RunLiftArithmeticAtoms,
             },
            {
             .id         = PassId::kLiftRepeatedSubexpressions,
             .consumes   = StateKind::kFoldedAst,
             .tag        = PassTag::kRewrite,
             .applicable = [](const WorkItem &item, const OrchestratorContext & /*ctx*/)
 -> bool { return std::holds_alternative< AstPayload >(item.payload); },
             .run = RunLiftRepeatedSubexpressions,
             },
            {
             .id         = PassId::kPrepareLiftedOuterSolve,
             .consumes   = StateKind::kLiftedSkeleton,
             .tag        = PassTag::kAnalysis,
             .applicable = [](const WorkItem &item,
             const OrchestratorContext & /*ctx*/) -> bool {
             return std::holds_alternative< LiftedSkeletonPayload >(item.payload);
             },        .run = RunPrepareLiftedOuterSolve,
             },
            {
             .id         = PassId::kVerifyCandidate,
             .consumes   = StateKind::kCandidateExpr,
             .tag        = PassTag::kVerifier,
             .applicable = [](const WorkItem &item,                              const OrchestratorContext & /*ctx*/)
                              -> bool { return IsCandidateKind(item); },
             .run = RunVerifyCandidate,
             },
        };
        return kRegistry;
    }

    // ---------------------------------------------------------------
    // ActiveAst helpers
    // ---------------------------------------------------------------

    const std::vector< std::string > &
    ActiveAstVars(const WorkItem &item, const OrchestratorContext &ctx) {
        if (auto *ast = std::get_if< AstPayload >(&item.payload)) {
            if (ast->solve_ctx.has_value()) { return ast->solve_ctx->vars; }
        }
        return ctx.original_vars;
    }

    const std::optional< Evaluator > &
    ActiveAstEvaluator(const WorkItem &item, const OrchestratorContext &ctx) {
        if (auto *ast = std::get_if< AstPayload >(&item.payload)) {
            if (ast->solve_ctx.has_value()) { return ast->solve_ctx->evaluator; }
        }
        return ctx.evaluator;
    }

    const std::vector< uint64_t > *
    ActiveAstInputSig(const WorkItem &item, const OrchestratorContext &ctx) {
        if (auto *ast = std::get_if< AstPayload >(&item.payload)) {
            if (ast->solve_ctx.has_value() && !ast->solve_ctx->input_sig.empty()) {
                return &ast->solve_ctx->input_sig;
            }
        }
        if (!ctx.input_sig.empty()) { return &ctx.input_sig; }
        return nullptr;
    }

} // namespace cobra
