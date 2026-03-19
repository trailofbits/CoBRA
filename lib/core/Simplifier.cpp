#include "cobra/core/Simplifier.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classification.h"
#include "cobra/core/Classifier.h"
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
#include "cobra/core/TemplateDecomposer.h"
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

        Result< SimplifyOutcome > RunSupportedPipeline(
            const std::vector< uint64_t > &sig, const std::vector< std::string > &vars,
            const Options &opts
        ) {
            const auto kNumVars = static_cast< uint32_t >(vars.size());

            // Step 1: Prune constants
            {
                auto pm = MatchPattern(sig, kNumVars, opts.bitwidth);
                if (pm && (*pm)->kind == Expr::Kind::kConstant) {
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

        Evaluator MapEvaluator(
            const Evaluator &eval, const std::vector< std::string > &vars,
            const std::vector< std::string > &real_vars
        ) {
            if (real_vars.size() == vars.size()) { return eval; }
            if (real_vars.empty()) {
                return [eval, orig_count = static_cast< uint32_t >(vars.size())](
                           const std::vector< uint64_t > &
                       ) -> uint64_t { return eval(std::vector< uint64_t >(orig_count, 0)); };
            }
            std::vector< uint32_t > var_map;
            var_map.reserve(real_vars.size());
            for (const auto &rv : real_vars) {
                for (uint32_t j = 0; j < vars.size(); ++j) {
                    if (vars[j] == rv) {
                        var_map.push_back(j);
                        break;
                    }
                }
            }
            return [eval, var_map, orig_count = static_cast< uint32_t >(vars.size())](
                       const std::vector< uint64_t > &reduced_vals
                   ) -> uint64_t {
                std::vector< uint64_t > original_vals(orig_count, 0);
                for (size_t i = 0; i < var_map.size(); ++i) {
                    original_vals[var_map[i]] = reduced_vals[i];
                }
                return eval(original_vals);
            };
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

        // Check if an expression is Mul(var_or_linear, var_or_linear) that
        // produces a non-affine product (i.e., involves distinct subtrees
        // that are not constants).
        bool IsVarProduct(const Expr &e) {
            if (e.kind != Expr::Kind::kMul || e.children.size() != 2) { return false; }
            // Both children must be either variables or simple expressions
            // (not constants). A Mul(const, expr) is a scaled linear term,
            // not a product we need to extract.
            const bool kLhsConst = (e.children[0]->kind == Expr::Kind::kConstant);
            const bool kRhsConst = (e.children[1]->kind == Expr::Kind::kConstant);
            return !kLhsConst && !kRhsConst;
        }

        // Walk a left-leaning Add tree, collecting product terms and
        // residual (non-product) terms. Handles Add, Neg-wrapped terms.
        void SplitAddTree(
            const Expr &e, std::vector< const Expr * > &products,
            std::vector< std::unique_ptr< Expr > >
                &residual // NOLINT(hicpp-named-parameter,readability-named-parameter)
        ) {
            if (e.kind == Expr::Kind::kAdd && e.children.size() == 2) {
                SplitAddTree(*e.children[0], products, residual);
                // rhs could be direct or negated
                const Expr &rhs = *e.children[1];
                if (IsVarProduct(rhs)) {
                    products.push_back(&rhs);
                } else if (
                    rhs.kind == Expr::Kind::kNeg && rhs.children.size() == 1
                    && IsVarProduct(*rhs.children[0])
                )
                {
                    products.push_back(&rhs);
                } else {
                    residual.push_back(CloneExpr(rhs));
                }
                return;
            }
            // Base case
            if (IsVarProduct(e)) {
                products.push_back(&e);
            } else {
                residual.push_back(CloneExpr(e));
            }
        }

        // After product identity collapse, try splitting the expression
        // into product terms + linear residual, simplify the residual
        // separately via the standard pipeline, then recombine.
        std::optional< SimplifyOutcome > TrySplitProductResidual(
            const Expr &collapsed_expr, const std::vector< std::string > &vars,
            const Options &opts, const Classification &cls
        ) {
            std::vector< const Expr * > products;
            std::vector< std::unique_ptr< Expr > > residual;
            SplitAddTree(collapsed_expr, products, residual);

            if (products.empty() || residual.empty()) { return std::nullopt; }

            // Build residual expression
            auto residual_expr = std::move(residual[0]);
            for (size_t i = 1; i < residual.size(); ++i) {
                residual_expr = Expr::Add(std::move(residual_expr), std::move(residual[i]));
            }

            // Compute residual signature and try the standard pipeline
            const auto kNv = static_cast< uint32_t >(vars.size());
            auto res_sig   = EvaluateBooleanSignature(*residual_expr, kNv, opts.bitwidth);

            auto res_result = RunSupportedPipeline(res_sig, vars, opts);
            if (!res_result.has_value()) { return std::nullopt; }
            if (res_result.value().kind != SimplifyOutcome::Kind::kSimplified) {
                return std::nullopt;
            }

            // Recombine: products + simplified residual
            auto combined = CloneExpr(*products[0]);
            for (size_t i = 1; i < products.size(); ++i) {
                combined = Expr::Add(std::move(combined), CloneExpr(*products[i]));
            }
            combined = Expr::Add(std::move(combined), std::move(res_result.value().expr));

            // Verify the combined expression
            if (opts.evaluator) {
                auto check = FullWidthCheckEval(opts.evaluator, kNv, *combined, opts.bitwidth);
                if (!check.passed) { return std::nullopt; }
            }

            SimplifyOutcome outcome;
            outcome.kind       = SimplifyOutcome::Kind::kSimplified;
            outcome.expr       = std::move(combined);
            outcome.sig_vector = EvaluateBooleanSignature(*outcome.expr, kNv, opts.bitwidth);
            outcome.real_vars  = std::vector< std::string >(vars.begin(), vars.end());
            outcome.verified   = true;
            outcome.diag.classification             = cls;
            outcome.diag.rewrite_produced_candidate = true;
            return outcome;
        }

    } // namespace

    Result< SimplifyOutcome > Simplify(
        const std::vector< uint64_t > &sig, const std::vector< std::string > &vars,
        const Expr *input_expr, const Options &opts
    ) {
        // No AST available: run supported pipeline without classification
        if (input_expr == nullptr) { return RunSupportedPipeline(sig, vars, opts); }

        // Lower ~(arithmetic) to -(arithmetic) - 1 before classification.
        // This converts BitwiseOverArith patterns like ~(b*b) into pure
        // polynomials, enabling a better route (e.g., PowerRecovery).
        std::unique_ptr< Expr > lowered_storage;
        const Expr *working_expr = input_expr;
        auto working_sig         = sig;

        if (HasNotOverArith(*input_expr)) {
            lowered_storage = LowerNotOverArith(CloneExpr(*input_expr), opts.bitwidth);
            working_expr    = lowered_storage.get();
            working_sig     = EvaluateBooleanSignature(
                *lowered_storage, static_cast< uint32_t >(vars.size()), opts.bitwidth
            );
        }

        auto cls = ClassifyStructural(*working_expr);

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
                    const auto kRvCount = static_cast< uint32_t >(
                        result.value().real_vars.empty() ? vars.size()
                                                         : result.value().real_vars.size()
                    );
                    auto mapped = MapEvaluator(opts.evaluator, vars, result.value().real_vars);
                    auto check  = FullWidthCheckEval(
                        mapped, kRvCount, *result.value().expr, opts.bitwidth
                    );
                    if (check.passed) {
                        result.value().diag.classification = cls;
                        return result;
                    }
                }

                // State: best current expression for subsequent steps
                auto current_expr = CloneExpr(*working_expr);

                // Step 2: Operand simplification
                auto op_result = SimplifyMixedOperands(std::move(current_expr), vars, opts);
                current_expr   = std::move(op_result.expr);

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
                        const auto kRvCount = static_cast< uint32_t >(
                            reentry.value().real_vars.empty() ? vars.size()
                                                              : reentry.value().real_vars.size()
                        );
                        auto mapped =
                            MapEvaluator(opts.evaluator, vars, reentry.value().real_vars);
                        auto check = FullWidthCheckEval(
                            mapped, kRvCount, *reentry.value().expr, opts.bitwidth
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
                        const auto kRvCount = static_cast< uint32_t >(
                            reentry.value().real_vars.empty() ? vars.size()
                                                              : reentry.value().real_vars.size()
                        );
                        auto mapped =
                            MapEvaluator(opts.evaluator, vars, reentry.value().real_vars);
                        auto check = FullWidthCheckEval(
                            mapped, kRvCount, *reentry.value().expr, opts.bitwidth
                        );
                        if (check.passed) {
                            reentry.value().diag.classification             = cls;
                            reentry.value().diag.rewrite_produced_candidate = true;
                            return reentry;
                        }
                    }

                    // Step 2.5b: Split product + linear residual.
                    // After collapse, the expression may be
                    // x*y + linear_mba. Split off product terms,
                    // simplify the linear residual separately,
                    // then recombine.
                    auto split = TrySplitProductResidual(*current_expr, vars, opts, cls);
                    if (split.has_value()) { return Ok(std::move(*split)); }
                }

                // Step 2.75: Template decomposition fallback.
                // Evaluator-based; does not depend on AST form.
                {
                    auto elim           = EliminateAuxVars(working_sig, vars);
                    const auto kRvCount = static_cast< uint32_t >(elim.real_vars.size());

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
                    ctx.eval = MapEvaluator(opts.evaluator, vars, elim.real_vars);

                    auto td = TryTemplateDecomposition(ctx, opts, kRvCount, nullptr);
                    if (td.has_value()) {
                        SimplifyOutcome outcome;
                        outcome.kind                = SimplifyOutcome::Kind::kSimplified;
                        outcome.expr                = std::move(td->expr);
                        outcome.sig_vector          = elim.reduced_sig;
                        outcome.real_vars           = std::move(elim.real_vars);
                        outcome.verified            = td->verified;
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

                if (!rewritten.structure_changed) {
                    return Ok(
                        MakeUnchanged(working_expr, cls, rewritten, "No rewrite applied")
                    );
                }

                auto new_cls = ClassifyStructural(*rewritten.expr);
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
                    const auto kRvCount = static_cast< uint32_t >(
                        reentry.value().real_vars.empty() ? vars.size()
                                                          : reentry.value().real_vars.size()
                    );
                    auto mapped = MapEvaluator(opts.evaluator, vars, reentry.value().real_vars);
                    auto check  = FullWidthCheckEval(
                        mapped, kRvCount, *reentry.value().expr, opts.bitwidth
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
