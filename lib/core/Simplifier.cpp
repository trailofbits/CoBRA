#include "cobra/core/Simplifier.h"
#include "cobra/core/AtomSimplifier.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/BitPartitioner.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Classification.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/DecompositionEngine.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/MaskedAtomReconstructor.h"
#include "cobra/core/MixedProductRewriter.h"
#include "cobra/core/OperandSimplifier.h"
#include "cobra/core/PatternMatcher.h"
#include "cobra/core/ProductIdentityRecoverer.h"
#include "cobra/core/Result.h"
#include "cobra/core/SelfCheck.h"
#include "cobra/core/SemilinearIR.h"
#include "cobra/core/SemilinearNormalizer.h"
#include "cobra/core/SemilinearSignature.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/SignatureSimplifier.h"
#include "cobra/core/SimplifyOutcome.h"
#include "cobra/core/StructureRecovery.h"
#include "cobra/core/TermRefiner.h"
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

        // Rewrite k + k*(c^x) → (-k)*(~c ^ x).
        // The semilinear XOR recovery produces a negated coefficient
        // and an additive constant that cancel via this identity.
        std::unique_ptr< Expr >
        SimplifyXorConstant(std::unique_ptr< Expr > expr, uint32_t bitwidth) {
            if (expr->kind != Expr::Kind::kAdd) { return expr; }
            if (expr->children.size() != 2) { return expr; }

            // Match Add(Constant(k), Mul(Constant(k), XOR(Constant(c), ...)))
            // or   Add(Mul(Constant(k), XOR(Constant(c), ...)), Constant(k))
            const Expr *const_node = nullptr;
            const Expr *mul_node   = nullptr;
            int const_idx          = -1;

            for (int i = 0; i < 2; ++i) {
                if (expr->children[i]->kind == Expr::Kind::kConstant
                    && expr->children[1 - i]->kind == Expr::Kind::kMul)
                {
                    const_node = expr->children[i].get();
                    mul_node   = expr->children[1 - i].get();
                    const_idx  = i;
                    break;
                }
            }
            if (const_node == nullptr) { return expr; }

            // Mul must have Constant(k) as first child and XOR as second.
            if (mul_node->children.size() != 2) { return expr; }
            const auto &mul_lhs = *mul_node->children[0];
            const auto &mul_rhs = *mul_node->children[1];
            if (mul_lhs.kind != Expr::Kind::kConstant) { return expr; }
            if (mul_rhs.kind != Expr::Kind::kXor) { return expr; }

            // Constants must match: k == k
            if (const_node->constant_val != mul_lhs.constant_val) { return expr; }

            // XOR must have a constant child.
            const auto &xor_node = mul_rhs;
            if (xor_node.children.size() != 2) { return expr; }

            int xor_const_idx = -1;
            if (xor_node.children[0]->kind == Expr::Kind::kConstant) {
                xor_const_idx = 0;
            } else if (xor_node.children[1]->kind == Expr::Kind::kConstant) {
                xor_const_idx = 1;
            }
            if (xor_const_idx < 0) { return expr; }

            // k + k*(c^x) = (-k)*(~c^x)
            const uint64_t kMask = Bitmask(bitwidth);
            const uint64_t kK    = const_node->constant_val;
            const uint64_t kNegK = ModNeg(kK, bitwidth);
            const uint64_t kC    = xor_node.children[xor_const_idx]->constant_val;
            const uint64_t kNotC = (~kC) & kMask;

            auto var_child = CloneExpr(*xor_node.children[1 - xor_const_idx]);
            auto new_xor   = Expr::BitwiseXor(Expr::Constant(kNotC), std::move(var_child));
            return ApplyCoefficient(std::move(new_xor), kNegK, bitwidth);
        }

        std::optional< SimplifyOutcome > TrySemilinearPipeline(
            const Expr &ast, const std::vector< std::string > &vars, const Options &opts,
            const Classification &cls
        ) {
            const auto kNumVars = static_cast< uint32_t >(vars.size());
            COBRA_TRACE(
                "Simplifier", "TrySemilinearPipeline: vars={} bitwidth={}", vars.size(),
                opts.bitwidth
            );

            if (kNumVars > opts.max_vars) { return std::nullopt; }

            auto ir_result = NormalizeToSemilinear(ast, vars, opts.bitwidth);
            if (!ir_result.has_value()) {
                COBRA_TRACE(
                    "Simplifier", "TrySemilinearPipeline: normalization failed: {}",
                    ir_result.error().message
                );
                return std::nullopt;
            }
            auto &ir = ir_result.value();

            SimplifyStructure(ir);

            auto plain = ReconstructMaskedAtoms(ir, {});
            auto check = SelfCheckSemilinear(ir, *plain, vars, opts.bitwidth);
            if (!check.passed) {
                COBRA_TRACE(
                    "Simplifier", "TrySemilinearPipeline: self-check failed: {}",
                    check.mismatch_detail
                );
                return std::nullopt;
            }

            if (FlattenComplexAtoms(ir)) { CoalesceTerms(ir); }
            RecoverStructure(ir);
            RefineTerms(ir);
            CoalesceTerms(ir);

            if (opts.evaluator) {
                auto probe_expr = ReconstructMaskedAtoms(ir, {});
                auto probe      = FullWidthCheckEval(
                    opts.evaluator, kNumVars, *probe_expr, opts.bitwidth, 16
                );
                if (!probe.passed) {
                    COBRA_TRACE(
                        "Simplifier", "TrySemilinearPipeline: post-rewrite probe failed"
                    );
                    return std::nullopt;
                }
            }

            CompactAtomTable(ir);
            auto partitions = ComputePartitions(ir);
            auto simplified = ReconstructMaskedAtoms(ir, partitions);
            simplified      = SimplifyXorConstant(std::move(simplified), opts.bitwidth);

            if (opts.evaluator) {
                auto final_check =
                    FullWidthCheckEval(opts.evaluator, kNumVars, *simplified, opts.bitwidth);
                if (!final_check.passed) {
                    COBRA_TRACE(
                        "Simplifier", "TrySemilinearPipeline: final verification failed"
                    );
                    return std::nullopt;
                }
            }

            SimplifyOutcome outcome;
            outcome.kind                 = SimplifyOutcome::Kind::kSimplified;
            outcome.expr                 = std::move(simplified);
            outcome.real_vars            = vars;
            outcome.verified             = true;
            outcome.diag.classification  = cls;
            outcome.diag.attempted_route = cls.route;
            COBRA_TRACE("Simplifier", "TrySemilinearPipeline: success");
            return outcome;
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

        ctx.original_indices = BuildVarSupport(vars, elim.real_vars);

        if (opts.evaluator) {
            if (elim.real_vars.size() == vars.size()) {
                ctx.eval = opts.evaluator;
            } else {
                ctx.eval = [eval = opts.evaluator, idx_map = ctx.original_indices,
                            original_vals = std::vector< uint64_t >(vars.size(), 0)](
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

        // Delegate to SignatureSimplifier
        auto sub = SimplifyFromSignature(elim.reduced_sig, ctx, opts, 0);
        COBRA_TRACE(
            "Simplifier", "RunSupportedPipeline: SignatureSimplifier returned succeeded={}",
            sub.Succeeded()
        );

        if (sub.Succeeded()) {
            auto payload = sub.TakePayload();
            SimplifyOutcome outcome;
            outcome.kind       = SimplifyOutcome::Kind::kSimplified;
            outcome.expr       = std::move(payload.expr);
            outcome.sig_vector = elim.reduced_sig;
            outcome.real_vars  = std::move(elim.real_vars);
            outcome.verified   = payload.verification == VerificationState::kVerified;
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

        // Semilinear expressions need the AST-based pipeline, not
        // boolean-signature simplification. Try it before routing.
        // Use input_expr (pre-lowering) — LowerNotOverArith converts
        // ~(const) into Add(Neg(const), mask) which inflates the IR.
        if (cls.semantic == SemanticClass::kSemilinear
            && !IsLinearShortcut(
                *working_expr, static_cast< uint32_t >(vars.size()), opts.bitwidth
            ))
        {
            auto semi = TrySemilinearPipeline(*input_expr, vars, opts, cls);
            if (semi.has_value()) { return Ok(std::move(*semi)); }
            COBRA_TRACE(
                "Simplifier", "Semilinear pipeline failed, falling back to supported pipeline"
            );
        }

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

                // Verify in original variable space. The internal
                // FullWidthCheckEval uses a remapped evaluator that
                // zeros eliminated variables, so it cannot catch wrong
                // aux-var elimination (x*y == x&y on {0,1}).
                // This mirrors kMixedRewrite Step 1's unconditional
                // VerifyInOriginalSpace call.
                if (opts.evaluator && result.value().kind == SimplifyOutcome::Kind::kSimplified)
                {
                    auto check = VerifyInOriginalSpace(
                        opts.evaluator, vars, result.value().real_vars, *result.value().expr,
                        opts.bitwidth
                    );
                    if (!check.passed) {
                        return Ok(MakeUnchanged(
                            working_expr, cls,
                            "Supported pipeline result failed"
                            " full-width verification"
                        ));
                    }
                    result.value().verified = true;
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
