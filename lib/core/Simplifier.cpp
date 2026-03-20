#include "cobra/core/Simplifier.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classification.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/DecompositionEngine.h"
#include "cobra/core/Expr.h"
#include "cobra/core/MixedProductRewriter.h"
#include "cobra/core/OperandSimplifier.h"
#include "cobra/core/PatternMatcher.h"
#include "cobra/core/ProductIdentityRecoverer.h"
#include "cobra/core/Result.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/SignatureSimplifier.h"
#include "cobra/core/SimplifyOutcome.h"
#include "cobra/core/Trace.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cobra {

    namespace {

        void RemapVarIndices(Expr &expr, const std::vector< uint32_t > &index_map) {
            if (expr.kind == Expr::Kind::kVariable) {
                expr.var_index = index_map[expr.var_index];
                return;
            }
            for (auto &child : expr.children) { RemapVarIndices(*child, index_map); }
        }

        // Verify a reduced-variable result against the original evaluator
        // with random values for ALL original variables (including eliminated).
        // This catches cases where aux var elimination incorrectly dropped a
        // variable that matters at full width (e.g., x*y == x&y on {0,1}).
        CheckResult VerifyInOriginalSpace(
            const Evaluator &eval, const std::vector< std::string > &all_vars,
            const std::vector< std::string > &real_vars, const Expr &reduced_expr,
            uint32_t bitwidth
        ) {
            const auto kAllCount = static_cast< uint32_t >(all_vars.size());
            if (real_vars.empty() || real_vars.size() == all_vars.size()) {
                return FullWidthCheckEval(eval, kAllCount, reduced_expr, bitwidth);
            }
            std::vector< uint32_t > idx_map;
            idx_map.reserve(real_vars.size());
            for (const auto &rv : real_vars) {
                for (uint32_t j = 0; j < all_vars.size(); ++j) {
                    if (all_vars[j] == rv) {
                        idx_map.push_back(j);
                        break;
                    }
                }
            }
            auto remapped = CloneExpr(reduced_expr);
            RemapVarIndices(*remapped, idx_map);
            return FullWidthCheckEval(eval, kAllCount, *remapped, bitwidth);
        }

        SimplifyOutcome MakeUnchanged(
            const Expr *input_expr, const Classification &cls, const std::string &reason
        ) {
            SimplifyOutcome outcome;
            outcome.kind                 = SimplifyOutcome::Kind::kUnchangedUnsupported;
            outcome.expr                 = CloneExpr(*input_expr);
            outcome.diag.classification  = cls;
            outcome.diag.attempted_route = cls.route;
            outcome.diag.reason          = reason;
            return outcome;
        }

        SimplifyOutcome MakeUnchanged(
            const Expr *input_expr, const Classification &cls, const RewriteResult &rw,
            const std::string &reason
        ) {
            auto outcome                            = MakeUnchanged(input_expr, cls, reason);
            outcome.diag.rewrite_rounds             = rw.rounds_applied;
            outcome.diag.rewrite_produced_candidate = rw.structure_changed;
            return outcome;
        }

        bool IsPurelyArithmetic(const Expr &e) {
            switch (e.kind) {
                case Expr::Kind::kConstant:
                case Expr::Kind::kVariable:
                case Expr::Kind::kAdd:
                case Expr::Kind::kMul:
                case Expr::Kind::kNeg:
                    break;
                default:
                    return false;
            }
            return std::ranges::all_of(e.children, [](const auto &child) {
                return IsPurelyArithmetic(*child);
            });
        }

        bool HasNotOverArith(const Expr &e) {
            if (e.kind == Expr::Kind::kNot && !e.children.empty()
                && IsPurelyArithmetic(*e.children[0]))
            {
                return true;
            }
            return std::ranges::any_of(e.children, [](const auto &child) {
                return HasNotOverArith(*child);
            });
        }

        std::unique_ptr< Expr > LowerNotOverArith(
            std::unique_ptr< Expr > e, uint32_t bitwidth
        ) { // NOLINT(readability-identifier-naming)
            for (auto &child : e->children) {
                child = LowerNotOverArith(std::move(child), bitwidth);
            }

            if (e->kind == Expr::Kind::kNot && e->children.size() == 1
                && IsPurelyArithmetic(*e->children[0]))
            {
                const uint64_t mask = (bitwidth == 64) ? UINT64_MAX : ((1ULL << bitwidth) - 1);
                return Expr::Add(Expr::Negate(std::move(e->children[0])), Expr::Constant(mask));
            }

            return e;
        }

    } // namespace

    Result< SimplifyOutcome > RunSupportedPipeline(
        const std::vector< uint64_t > &sig, const std::vector< std::string > &vars,
        const Options &opts
    ) {
        const auto kNumVars = static_cast< uint32_t >(vars.size());
        COBRA_TRACE(
            "Simplifier", "RunSupportedPipeline: vars={} bitwidth={} max_vars={}", vars.size(),
            opts.bitwidth, opts.max_vars
        );
        COBRA_TRACE_SIG("Simplifier", "RunSupportedPipeline input sig", sig);

        // Step 1: Prune constants
        {
            auto pm = MatchPattern(sig, kNumVars, opts.bitwidth);
            if (pm && (*pm)->kind == Expr::Kind::kConstant) {
                COBRA_TRACE(
                    "Simplifier", "RunSupportedPipeline: constant match val={}",
                    (*pm)->constant_val
                );
                SimplifyOutcome outcome;
                outcome.kind       = SimplifyOutcome::Kind::kSimplified;
                outcome.expr       = std::move(*pm);
                outcome.sig_vector = sig;
                outcome.verified   = true;
                return Ok(std::move(outcome));
            }
        }

        // Step 2: Eliminate auxiliary variables
        auto elim                = EliminateAuxVars(sig, vars);
        const auto kRealVarCount = static_cast< uint32_t >(elim.real_vars.size());
        COBRA_TRACE(
            "Simplifier", "RunSupportedPipeline: after EliminateAuxVars real={} eliminated={}",
            elim.real_vars.size(), elim.spurious_vars.size()
        );

        if (kRealVarCount > opts.max_vars) {
            return Err< SimplifyOutcome >(
                CobraError::kTooManyVariables,
                "Variable count after elimination (" + std::to_string(kRealVarCount)
                    + ") exceeds max_vars (" + std::to_string(opts.max_vars) + ")"
            );
        }

        // Build SignatureContext with mapped evaluator
        SignatureContext ctx; // NOLINT(misc-const-correctness)
        ctx.vars = elim.real_vars;

        ctx.original_indices.reserve(elim.real_vars.size());
        for (const auto &real_var : elim.real_vars) {
            for (size_t j = 0; j < vars.size(); ++j) {
                if (vars[j] == real_var) {
                    ctx.original_indices.push_back(static_cast< uint32_t >(j));
                    break;
                }
            }
        }

        if (opts.evaluator) {
            if (elim.real_vars.size() == vars.size()) {
                ctx.eval = opts.evaluator;
            } else {
                auto idx_map = ctx.original_indices;
                ctx.eval     = [eval = opts.evaluator, idx_map = std::move(idx_map),
                                orig_sz = vars.size()](
                                   const std::vector< uint64_t > &reduced_vals
                               ) -> uint64_t {
                    std::vector< uint64_t > original_vals(orig_sz, 0);
                    for (size_t i = 0; i < idx_map.size(); ++i) {
                        original_vals[idx_map[i]] = reduced_vals[i];
                    }
                    return eval(original_vals);
                };
            }
        }

        // Delegate to SignatureSimplifier
        auto sub = SimplifyFromSignature(elim.reduced_sig, ctx, opts, 0);
        COBRA_TRACE(
            "Simplifier", "RunSupportedPipeline: SignatureSimplifier returned has_value={}",
            sub.has_value()
        );

        if (sub.has_value()) {
            SimplifyOutcome outcome;
            outcome.kind       = SimplifyOutcome::Kind::kSimplified;
            outcome.expr       = std::move(sub->expr);
            outcome.sig_vector = elim.reduced_sig;
            outcome.real_vars  = std::move(elim.real_vars);
            outcome.verified   = sub->verified;
            return Ok(std::move(outcome));
        }

        // Fallback: should not normally reach here since
        // simplify_from_signature always produces a CoB result.
        return Err< SimplifyOutcome >(
            CobraError::kVerificationFailed, "SignatureSimplifier produced no result"
        );
    }

    Result< SimplifyOutcome > Simplify(
        const std::vector< uint64_t > &sig, const std::vector< std::string > &vars,
        const Expr *input_expr, const Options &opts
    ) {
        COBRA_TRACE(
            "Simplifier", "Simplify: vars={} has_ast={}", vars.size(), input_expr != nullptr
        );
        // No AST available: run supported pipeline without classification
        if (input_expr == nullptr) { return RunSupportedPipeline(sig, vars, opts); }

        // Lower ~(arithmetic) to -(arithmetic) - 1 before classification.
        // This converts BitwiseOverArith patterns like ~(b*b) into pure
        // polynomials, enabling a better route (e.g., PowerRecovery).
        std::unique_ptr< Expr > lowered_storage;
        const Expr *working_expr = input_expr;
        auto working_sig         = sig;

        if (HasNotOverArith(*input_expr)) {
            COBRA_TRACE("Simplifier", "Simplify: lowering NOT-over-Arith patterns");
            lowered_storage = LowerNotOverArith(CloneExpr(*input_expr), opts.bitwidth);
            working_expr    = lowered_storage.get();
            working_sig     = EvaluateBooleanSignature(
                *lowered_storage, static_cast< uint32_t >(vars.size()), opts.bitwidth
            );
        }

        auto cls = ClassifyStructural(*working_expr);
        COBRA_TRACE(
            "Simplifier", "Simplify: route={} semantic={}", static_cast< int >(cls.route),
            static_cast< int >(cls.semantic)
        );

        switch (cls.route) {
            case Route::kBitwiseOnly:
            case Route::kMultilinear:
            case Route::kPowerRecovery: {
                Options pipeline_opts          = opts;
                pipeline_opts.structural_flags = cls.flags;
                auto result = RunSupportedPipeline(working_sig, vars, pipeline_opts);
                if (!result.has_value()) {
                    if (result.error().code != CobraError::kVerificationFailed) {
                        return result;
                    }
                    SimplifyOutcome outcome;
                    outcome.kind                 = SimplifyOutcome::Kind::kError;
                    outcome.diag.classification  = cls;
                    outcome.diag.attempted_route = cls.route;
                    outcome.diag.reason          = "Supported fragment failed verification"
                                                   " (BugOrGap): "
                        + result.error().message;
                    return Ok(std::move(outcome));
                }
                result.value().diag.classification  = cls;
                result.value().diag.attempted_route = cls.route;
                return result;
            }

            case Route::kMixedRewrite: {
                COBRA_TRACE("Simplifier", "MixedRewrite: starting multi-step pipeline");
                if (!opts.evaluator) {
                    return Ok(MakeUnchanged(
                        working_expr, cls,
                        "MixedRewrite requires evaluator for "
                        "verification"
                    ));
                }

                // Step 1: Try the standard pipeline opportunistically
                auto result = RunSupportedPipeline(working_sig, vars, opts);
                if (!result.has_value()
                    && result.error().code != CobraError::kVerificationFailed)
                {
                    return result;
                }
                if (result.has_value()
                    && result.value().kind == SimplifyOutcome::Kind::kSimplified)
                {
                    auto check = VerifyInOriginalSpace(
                        opts.evaluator, vars, result.value().real_vars, *result.value().expr,
                        opts.bitwidth
                    );
                    if (check.passed) {
                        result.value().diag.classification = cls;
                        return result;
                    }
                }

                // Step 1.5: Early decomposition on original AST
                // Product cores that directly equal f(x) exist in the
                // original obfuscated form but are destroyed by operand
                // simplification, so try decomposition before preconditioning.
                {
                    DecompositionContext early_dctx{
                        .opts         = opts,
                        .vars         = vars,
                        .sig          = working_sig,
                        .current_expr = working_expr,
                        .cls          = cls,
                    };
                    auto early_decomp = TryDecomposition(early_dctx);
                    COBRA_TRACE(
                        "Simplifier", "MixedRewrite step 1.5: early decomposition found={}",
                        early_decomp.has_value()
                    );
                    if (early_decomp.has_value()) {
                        SimplifyOutcome outcome;
                        outcome.kind                = SimplifyOutcome::Kind::kSimplified;
                        outcome.expr                = std::move(early_decomp->expr);
                        outcome.sig_vector          = working_sig;
                        outcome.real_vars           = { vars.begin(), vars.end() };
                        outcome.verified            = true;
                        outcome.diag.classification = cls;
                        return Ok(std::move(outcome));
                    }
                }

                // State: best current expression for subsequent steps
                auto current_expr = CloneExpr(*working_expr);

                // Step 2: Operand simplification
                auto op_result = SimplifyMixedOperands(std::move(current_expr), vars, opts);
                current_expr   = std::move(op_result.expr);
                COBRA_TRACE(
                    "Simplifier", "MixedRewrite step 2: operand simplification changed={}",
                    op_result.changed
                );

                if (op_result.changed) {
                    auto new_sig = EvaluateBooleanSignature(
                        *current_expr, static_cast< uint32_t >(vars.size()), opts.bitwidth
                    );

                    auto reentry = RunSupportedPipeline(new_sig, vars, opts);
                    if (!reentry.has_value()
                        && reentry.error().code != CobraError::kVerificationFailed)
                    {
                        return reentry;
                    }
                    if (reentry.has_value()
                        && reentry.value().kind == SimplifyOutcome::Kind::kSimplified)
                    {
                        auto check = VerifyInOriginalSpace(
                            opts.evaluator, vars, reentry.value().real_vars,
                            *reentry.value().expr, opts.bitwidth
                        );
                        if (check.passed) {
                            reentry.value().diag.classification             = cls;
                            reentry.value().diag.rewrite_produced_candidate = true;
                            return reentry;
                        }
                    }
                }

                // Step 2.5: Product identity collapse
                auto pi_result = CollapseProductIdentities(std::move(current_expr), vars, opts);
                current_expr   = std::move(pi_result.expr);
                COBRA_TRACE(
                    "Simplifier", "MixedRewrite step 2.5: product identity collapse changed={}",
                    pi_result.changed
                );

                if (pi_result.changed) {
                    auto new_sig = EvaluateBooleanSignature(
                        *current_expr, static_cast< uint32_t >(vars.size()), opts.bitwidth
                    );

                    auto reentry = RunSupportedPipeline(new_sig, vars, opts);
                    if (!reentry.has_value()
                        && reentry.error().code != CobraError::kVerificationFailed)
                    {
                        return reentry;
                    }
                    if (reentry.has_value()
                        && reentry.value().kind == SimplifyOutcome::Kind::kSimplified)
                    {
                        auto check = VerifyInOriginalSpace(
                            opts.evaluator, vars, reentry.value().real_vars,
                            *reentry.value().expr, opts.bitwidth
                        );
                        if (check.passed) {
                            reentry.value().diag.classification             = cls;
                            reentry.value().diag.rewrite_produced_candidate = true;
                            return reentry;
                        }
                    }
                }

                // Phase 2: Decomposition engine
                {
                    auto post_cls   = ClassifyStructural(*current_expr);
                    auto decomp_sig = (op_result.changed || pi_result.changed)
                        ? EvaluateBooleanSignature(
                              *current_expr, static_cast< uint32_t >(vars.size()), opts.bitwidth
                          )
                        : working_sig;
                    DecompositionContext dctx{
                        .opts         = opts,
                        .vars         = vars,
                        .sig          = decomp_sig,
                        .current_expr = current_expr.get(),
                        .cls          = post_cls,
                    };
                    auto decomp = TryDecomposition(dctx);
                    COBRA_TRACE(
                        "Simplifier", "MixedRewrite Phase 2: decomposition found={}",
                        decomp.has_value()
                    );
                    if (decomp.has_value()) {
                        SimplifyOutcome outcome;
                        outcome.kind                = SimplifyOutcome::Kind::kSimplified;
                        outcome.expr                = std::move(decomp->expr);
                        outcome.sig_vector          = decomp_sig;
                        outcome.real_vars           = { vars.begin(), vars.end() };
                        outcome.verified            = true;
                        outcome.diag.classification = cls;
                        return Ok(std::move(outcome));
                    }
                }

                // Step 3: XOR lowering on the best current expression
                RewriteOptions rw_opts; // NOLINT(misc-const-correctness)
                rw_opts.max_rounds      = 2;
                rw_opts.max_node_growth = 3;
                rw_opts.bitwidth        = opts.bitwidth;

                auto rewritten = RewriteMixedProducts(std::move(current_expr), rw_opts);
                COBRA_TRACE(
                    "Simplifier", "MixedRewrite step 3: XOR lowering rounds={} changed={}",
                    rewritten.rounds_applied, rewritten.structure_changed
                );

                if (!rewritten.structure_changed) {
                    return Ok(
                        MakeUnchanged(working_expr, cls, rewritten, "No rewrite applied")
                    );
                }

                auto new_cls = ClassifyStructural(*rewritten.expr);
                COBRA_TRACE(
                    "Simplifier", "MixedRewrite step 3: post-rewrite route={}",
                    static_cast< int >(new_cls.route)
                );
                if (new_cls.route == Route::kMixedRewrite
                    || new_cls.route == Route::kUnsupported)
                {
                    return Ok(MakeUnchanged(
                        working_expr, cls, rewritten,
                        "Rewrite did not reduce to supported"
                        " structure"
                    ));
                }

                auto new_sig = EvaluateBooleanSignature(
                    *rewritten.expr, static_cast< uint32_t >(vars.size()), opts.bitwidth
                );
                auto reentry = RunSupportedPipeline(new_sig, vars, opts);
                if (!reentry.has_value()
                    && reentry.error().code != CobraError::kVerificationFailed)
                {
                    return reentry;
                }

                if (reentry.has_value()
                    && reentry.value().kind == SimplifyOutcome::Kind::kSimplified)
                {
                    auto check = VerifyInOriginalSpace(
                        opts.evaluator, vars, reentry.value().real_vars, *reentry.value().expr,
                        opts.bitwidth
                    );
                    if (check.passed) {
                        reentry.value().diag.classification = cls;
                        reentry.value().diag.rewrite_rounds = rewritten.rounds_applied;
                        reentry.value().diag.rewrite_produced_candidate = true;
                        return reentry;
                    }
                }

                auto outcome = MakeUnchanged(
                    working_expr, cls, rewritten, "Rewritten candidate failed verification"
                );
                outcome.diag.candidate_failed_verification = true;
                return Ok(std::move(outcome));
            }

            case Route::kUnsupported:
                return Ok(MakeUnchanged(
                    working_expr, cls,
                    "Expression structure is outside supported"
                    " scope"
                ));
        }

        return Err< SimplifyOutcome >(CobraError::kVerificationFailed, "Unhandled route");
    }

} // namespace cobra
