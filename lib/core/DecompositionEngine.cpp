#include "cobra/core/DecompositionEngine.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/GhostResidualSolver.h"
#include "cobra/core/MultivarPolyRecovery.h"
#include "cobra/core/PolyExprBuilder.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/SignatureSimplifier.h"
#include "cobra/core/TemplateDecomposer.h"
#include "cobra/core/Trace.h"
#include <cstdint>
#include <memory>
#include <numeric>
#include <random>

namespace cobra {

    namespace {

        bool IsVarProduct(const Expr &e) {
            if (e.kind != Expr::Kind::kMul || e.children.size() != 2) { return false; }
            const bool kLhsConst = (e.children[0]->kind == Expr::Kind::kConstant);
            const bool kRhsConst = (e.children[1]->kind == Expr::Kind::kConstant);
            return !kLhsConst && !kRhsConst;
        }

        // Check if e is a product addend: Mul(non-const,non-const),
        // Neg(Mul(...)), or Not(Mul(...)) (== -(Mul(...))-1).
        bool IsProductAddend(const Expr &e) {
            if (IsVarProduct(e)) { return true; }
            if (e.kind == Expr::Kind::kNeg && e.children.size() == 1
                && IsVarProduct(*e.children[0]))
            {
                return true;
            }
            // ~(Mul(a,b)) = -(Mul(a,b)) - 1: the Mul is
            // extractable; the -1 offset goes to residual.
            if (e.kind == Expr::Kind::kNot && e.children.size() == 1
                && IsVarProduct(*e.children[0]))
            {
                return true;
            }
            return false;
        }

        void SplitAddTree(
            const Expr &e, std::vector< const Expr * > &products,
            std::vector< std::unique_ptr< Expr > > &residual
        ) {
            if (e.kind == Expr::Kind::kAdd && e.children.size() == 2) {
                SplitAddTree(*e.children[0], products, residual);
                const Expr &rhs = *e.children[1];
                if (IsProductAddend(rhs)) {
                    products.push_back(&rhs);
                } else {
                    residual.push_back(CloneExpr(rhs));
                }
                return;
            }
            if (IsProductAddend(e)) {
                products.push_back(&e);
            } else {
                residual.push_back(CloneExpr(e));
            }
        }

    } // namespace

    Evaluator
    BuildResidualEvaluator(const Evaluator &original, const Expr &core, uint32_t bitwidth) {
        // shared_ptr because std::function requires copy-constructible captures.
        std::shared_ptr< Expr > core_shared = CloneExpr(core);
        const uint64_t kMask                = Bitmask(bitwidth);
        return [original, core = std::move(core_shared), kMask,
                bitwidth](const std::vector< uint64_t > &v) -> uint64_t {
            const uint64_t kF = original(v);
            const uint64_t kP = EvalExpr(*core, v, bitwidth);
            return (kF - kP) & kMask;
        };
    }

    std::optional< CoreCandidate > ExtractProductCore(const DecompositionContext &ctx) {
        if (ctx.current_expr == nullptr) { return std::nullopt; }

        std::vector< const Expr * > products;
        std::vector< std::unique_ptr< Expr > > residual;
        SplitAddTree(*ctx.current_expr, products, residual);

        if (products.empty()) { return std::nullopt; }

        auto core_expr = CloneExpr(*products[0]);
        for (size_t i = 1; i < products.size(); ++i) {
            core_expr = Expr::Add(std::move(core_expr), CloneExpr(*products[i]));
        }

        CoreCandidate core;
        core.expr = std::move(core_expr);
        core.kind = ExtractorKind::kProductAST;
        return core;
    }

    bool AcceptCore(const DecompositionContext &ctx, const CoreCandidate &core) {
        if (!core.expr) {
            COBRA_TRACE("AcceptCore", "rejected: null expression");
            return false;
        }
        if (core.expr->kind == Expr::Kind::kConstant) {
            COBRA_TRACE("AcceptCore", "rejected: constant core");
            return false;
        }

        const auto kNv       = static_cast< uint32_t >(ctx.vars.size());
        const uint32_t kBw   = ctx.opts.bitwidth;
        const uint64_t kMask = Bitmask(kBw);

        auto residual_eval = BuildResidualEvaluator(ctx.opts.evaluator, *core.expr, kBw);

        // Deterministic seed for reproducible acceptance decisions.
        std::mt19937_64 rng(0xDECAF);
        bool all_same_as_orig = true;
        bool all_zero         = true;
        constexpr int kProbes = 5;
        std::vector< uint64_t > point(kNv);
        for (int p = 0; p < kProbes; ++p) {
            for (uint32_t i = 0; i < kNv; ++i) { point[i] = rng() & kMask; }
            const uint64_t kOrig = ctx.opts.evaluator(point) & kMask;
            const uint64_t kRes  = residual_eval(point);
            if (kRes != kOrig) { all_same_as_orig = false; }
            if (kRes != 0) { all_zero = false; }
        }
        if (all_same_as_orig) {
            COBRA_TRACE("AcceptCore", "rejected: core is zero (residual == original)");
            return false;
        }
        if (all_zero) {
            COBRA_TRACE("AcceptCore", "rejected: core equals original (residual == 0)");
            return false;
        }

        // Non-trivial core that changes the function — let residual solvers
        // decide whether the decomposition is useful.
        return true;
    }

    std::optional< CoreCandidate >
    ExtractPolyCore(const DecompositionContext &ctx, uint8_t degree) {
        if (!ctx.opts.evaluator) { return std::nullopt; }

        const auto kNv     = static_cast< uint32_t >(ctx.vars.size());
        const uint32_t kBw = ctx.opts.bitwidth;

        // Support discovery: full-width EliminateAuxVars
        auto fw_elim          = EliminateAuxVars(ctx.sig, ctx.vars, ctx.opts.evaluator, kBw);
        const auto kRealCount = static_cast< uint32_t >(fw_elim.real_vars.size());

        if (kRealCount > 6) { return std::nullopt; }

        auto support = BuildVarSupport(ctx.vars, fw_elim.real_vars);

        auto poly = RecoverMultivarPoly(ctx.opts.evaluator, support, kNv, kBw, degree);
        if (!poly.Succeeded()) { return std::nullopt; }

        auto expr = BuildPolyExpr(poly.TakePayload());
        if (!expr.has_value()) { return std::nullopt; }

        CoreCandidate core;
        core.expr        = std::move(expr.value());
        core.kind        = ExtractorKind::kPolynomial;
        core.degree_used = degree;
        return core;
    }

    std::optional< CoreCandidate > ExtractTemplateCore(const DecompositionContext &ctx) {
        if (!ctx.opts.evaluator) { return std::nullopt; }

        const auto kNv     = static_cast< uint32_t >(ctx.vars.size());
        const uint32_t kBw = ctx.opts.bitwidth;

        // Reduced-variable attempt first
        auto elim           = EliminateAuxVars(ctx.sig, ctx.vars);
        const auto kRvCount = static_cast< uint32_t >(elim.real_vars.size());

        SignatureContext sig_ctx;
        sig_ctx.vars             = elim.real_vars;
        sig_ctx.original_indices = BuildVarSupport(ctx.vars, elim.real_vars);
        if (elim.real_vars.size() == ctx.vars.size()) {
            sig_ctx.eval = ctx.opts.evaluator;
        } else {
            sig_ctx.eval = [eval = ctx.opts.evaluator, idx = sig_ctx.original_indices,
                            full = std::vector< uint64_t >(ctx.vars.size(), 0)](
                               const std::vector< uint64_t > &rv
                           ) mutable -> uint64_t {
                for (size_t i = 0; i < idx.size(); ++i) { full[idx[i]] = rv[i]; }
                uint64_t result = eval(full);
                for (size_t i = 0; i < idx.size(); ++i) { full[idx[i]] = 0; }
                return result;
            };
        }

        auto td = TryTemplateDecomposition(sig_ctx, ctx.opts, kRvCount, nullptr);
        if (td.Succeeded()) {
            auto td_payload = td.TakePayload();
            // Remap to original variable space if reduced
            if (elim.real_vars.size() < ctx.vars.size()) {
                RemapVarIndices(*td_payload.expr, sig_ctx.original_indices);
            }
            // Verify in original variable space — reject false positives from
            // reduced-var template before consuming this extractor slot.
            auto check = FullWidthCheckEval(ctx.opts.evaluator, kNv, *td_payload.expr, kBw);
            if (check.passed) {
                CoreCandidate core;
                core.expr = std::move(td_payload.expr);
                core.kind = ExtractorKind::kTemplate;
                return core;
            }
        }

        // Full-variable fallback (only when reduced != full)
        if (elim.real_vars.size() < ctx.vars.size()) {
            SignatureContext full_ctx;
            full_ctx.vars = ctx.vars;
            full_ctx.original_indices.resize(ctx.vars.size());
            std::iota(full_ctx.original_indices.begin(), full_ctx.original_indices.end(), 0U);
            full_ctx.eval = ctx.opts.evaluator;

            auto td2 = TryTemplateDecomposition(full_ctx, ctx.opts, kNv, nullptr);
            if (td2.Succeeded()) {
                CoreCandidate core;
                core.expr = std::move(td2.TakePayload().expr);
                core.kind = ExtractorKind::kTemplate;
                return core;
            }
        }

        return std::nullopt;
    }

    std::optional< DecompositionResult > TryDecomposition(const DecompositionContext &ctx) {
        if (!ctx.opts.evaluator) { return std::nullopt; }

        const auto kNv     = static_cast< uint32_t >(ctx.vars.size());
        const uint32_t kBw = ctx.opts.bitwidth;

        COBRA_TRACE("DecompEngine", "TryDecomposition: vars={} bitwidth={}", kNv, kBw);

        // Boolean-null direct path: when f(x) is zero on all {0,1}^k
        // but non-zero at full width, try ghost solvers directly without
        // requiring a non-trivial polynomial core first.
        {
            auto fw_elim  = EliminateAuxVars(ctx.sig, ctx.vars, ctx.opts.evaluator, kBw);
            auto fw_count = static_cast< uint32_t >(fw_elim.real_vars.size());

            auto fw_support = BuildVarSupport(ctx.vars, fw_elim.real_vars);

            bool is_bn =
                IsBooleanNullResidual(ctx.opts.evaluator, fw_support, kNv, kBw, ctx.sig);

            if (is_bn && fw_count <= 6) {
                COBRA_TRACE(
                    "DecompEngine",
                    "Boolean-null function detected (fw_support={}), "
                    "trying ghost solvers directly",
                    fw_count
                );

                auto ghost = SolveGhostResidual(ctx.opts.evaluator, fw_support, kNv, kBw);
                if (ghost.Succeeded()) {
                    auto ghost_payload = ghost.TakePayload();
                    auto check =
                        FullWidthCheckEval(ctx.opts.evaluator, kNv, *ghost_payload.expr, kBw);
                    if (check.passed) {
                        COBRA_TRACE("DecompEngine", "Boolean-null: GhostResidual succeeded");
                        DecompositionResult result;
                        result.expr           = std::move(ghost_payload.expr);
                        result.extractor_kind = ExtractorKind::kBooleanNullDirect;
                        result.solver_kind    = ResidualSolverKind::kGhostResidual;
                        return result;
                    }
                }

                auto factored =
                    SolveFactoredGhostResidual(ctx.opts.evaluator, fw_support, kNv, kBw);
                if (factored.Succeeded()) {
                    auto factored_payload = factored.TakePayload();
                    auto check            = FullWidthCheckEval(
                        ctx.opts.evaluator, kNv, *factored_payload.expr, kBw
                    );
                    if (check.passed) {
                        COBRA_TRACE(
                            "DecompEngine", "Boolean-null: FactoredGhost(d=0) succeeded"
                        );
                        DecompositionResult result;
                        result.expr           = std::move(factored_payload.expr);
                        result.extractor_kind = ExtractorKind::kBooleanNullDirect;
                        result.solver_kind    = ResidualSolverKind::kGhostResidual;
                        return result;
                    }
                }

                // Escalate: higher-degree polynomial weights
                uint8_t grid   = (fw_count <= 2) ? 3 : 2;
                auto factored2 = SolveFactoredGhostResidual(
                    ctx.opts.evaluator, fw_support, kNv, kBw, 2, grid
                );
                if (factored2.Succeeded()) {
                    auto factored2_payload = factored2.TakePayload();
                    auto check             = FullWidthCheckEval(
                        ctx.opts.evaluator, kNv, *factored2_payload.expr, kBw
                    );
                    if (check.passed) {
                        COBRA_TRACE(
                            "DecompEngine", "Boolean-null: FactoredGhost(d=2) succeeded"
                        );
                        DecompositionResult result;
                        result.expr           = std::move(factored2_payload.expr);
                        result.extractor_kind = ExtractorKind::kBooleanNullDirect;
                        result.solver_kind    = ResidualSolverKind::kGhostResidual;
                        return result;
                    }
                }

                COBRA_TRACE(
                    "DecompEngine",
                    "Boolean-null: all ghost solvers failed, falling through to extractors"
                );
            }
        }

        // Helper: try a core candidate through the decomposition pipeline
        auto TryCore = [&](CoreCandidate &core) -> std::optional< DecompositionResult > {
            // Direct-success path
            auto direct_check = FullWidthCheckEval(ctx.opts.evaluator, kNv, *core.expr, kBw);
            if (direct_check.passed) {
                COBRA_TRACE(
                    "DecompEngine", "Direct success: kind={} degree={}",
                    static_cast< int >(core.kind), core.degree_used
                );
                DecompositionResult result;
                result.expr           = std::move(core.expr);
                result.extractor_kind = core.kind;
                result.core_degree    = core.degree_used;
                return result;
            }

            COBRA_TRACE(
                "DecompEngine", "Direct check failed for kind={}, entering residual pipeline",
                static_cast< int >(core.kind)
            );

            // Accept screen (polynomial extractors only)
            if (core.kind == ExtractorKind::kPolynomial && !AcceptCore(ctx, core)) {
                COBRA_TRACE("DecompEngine", "AcceptCore rejected polynomial core");
                return std::nullopt;
            }

            auto residual_eval = BuildResidualEvaluator(ctx.opts.evaluator, *core.expr, kBw);

            uint8_t degree_floor =
                (core.kind == ExtractorKind::kPolynomial) ? core.degree_used + 1 : 2;

            auto residual_sig = EvaluateBooleanSignature(residual_eval, kNv, kBw);

            Options residual_opts   = ctx.opts;
            residual_opts.evaluator = residual_eval;

            // Shared: full-width support discovery for residual
            auto res_fw_elim = EliminateAuxVars(residual_sig, ctx.vars, residual_eval, kBw);
            const auto kResRealCount = static_cast< uint32_t >(res_fw_elim.real_vars.size());

            auto res_support = BuildVarSupport(ctx.vars, res_fw_elim.real_vars);

            COBRA_TRACE(
                "DecompEngine", "Residual: support={} degree_floor={}", kResRealCount,
                degree_floor
            );

            bool is_ghost =
                IsBooleanNullResidual(residual_eval, res_support, kNv, kBw, residual_sig);

            if (is_ghost) {
                COBRA_TRACE(
                    "DecompEngine",
                    "Boolean-null residual detected, "
                    "routing: poly -> ghost -> template"
                );

                // Ghost solver 1: Polynomial recovery
                if (kResRealCount <= 6) {
                    auto res_poly = RecoverAndVerifyPoly(
                        residual_eval, res_support, kNv, kBw, 4, degree_floor
                    );
                    if (res_poly.Succeeded()) {
                        auto combined = Expr::Add(
                            CloneExpr(*core.expr), std::move(res_poly.TakePayload().expr)
                        );
                        auto check =
                            FullWidthCheckEval(ctx.opts.evaluator, kNv, *combined, kBw);
                        if (check.passed) {
                            COBRA_TRACE("DecompEngine", "Ghost path: PolyRecovery succeeded");
                            DecompositionResult result;
                            result.expr           = std::move(combined);
                            result.extractor_kind = core.kind;
                            result.solver_kind    = ResidualSolverKind::kPolynomialRecovery;
                            result.core_degree    = core.degree_used;
                            return result;
                        }
                        COBRA_TRACE("DecompEngine", "Ghost PolyRecovery: recombination failed");
                    } else {
                        COBRA_TRACE(
                            "DecompEngine",
                            "Ghost PolyRecovery: no polynomial recovered (dfloor={})",
                            degree_floor
                        );
                    }
                }

                // Ghost solver 2: Ghost residual solver (6-var cap)
                if (kResRealCount <= 6) {
                    auto ghost = SolveGhostResidual(residual_eval, res_support, kNv, kBw);
                    if (ghost.Succeeded()) {
                        auto ghost_payload = ghost.TakePayload();
                        auto combined =
                            Expr::Add(CloneExpr(*core.expr), std::move(ghost_payload.expr));
                        auto check =
                            FullWidthCheckEval(ctx.opts.evaluator, kNv, *combined, kBw);
                        if (check.passed) {
                            COBRA_TRACE("DecompEngine", "Ghost path: GhostResidual succeeded");
                            DecompositionResult result;
                            result.expr           = std::move(combined);
                            result.extractor_kind = core.kind;
                            result.solver_kind    = ResidualSolverKind::kGhostResidual;
                            result.core_degree    = core.degree_used;
                            return result;
                        }
                        COBRA_TRACE(
                            "DecompEngine", "Ghost GhostResidual: recombination failed"
                        );
                    } else {
                        COBRA_TRACE("DecompEngine", "Ghost GhostResidual: no solution");
                    }
                }

                // Ghost solver 2.5: Factored ghost residual (q(x) * g(tuple))
                if (kResRealCount <= 6) {
                    auto factored =
                        SolveFactoredGhostResidual(residual_eval, res_support, kNv, kBw);
                    if (factored.Succeeded()) {
                        auto factored_payload = factored.TakePayload();
                        auto combined =
                            Expr::Add(CloneExpr(*core.expr), std::move(factored_payload.expr));
                        auto check =
                            FullWidthCheckEval(ctx.opts.evaluator, kNv, *combined, kBw);
                        if (check.passed) {
                            COBRA_TRACE(
                                "DecompEngine", "Ghost path: FactoredGhostResidual succeeded"
                            );
                            DecompositionResult result;
                            result.expr           = std::move(combined);
                            result.extractor_kind = core.kind;
                            result.solver_kind    = ResidualSolverKind::kGhostResidual;
                            result.core_degree    = core.degree_used;
                            return result;
                        }
                        COBRA_TRACE(
                            "DecompEngine", "Ghost FactoredGhost: recombination failed"
                        );
                    } else {
                        COBRA_TRACE("DecompEngine", "Ghost FactoredGhost: no solution");
                    }
                }

                // Ghost solver 3: Generic template fallback
                {
                    SignatureContext res_sig_ctx;
                    res_sig_ctx.vars             = res_fw_elim.real_vars;
                    res_sig_ctx.original_indices = res_support;

                    if (res_fw_elim.real_vars.size() == ctx.vars.size()) {
                        res_sig_ctx.eval = residual_eval;
                    } else {
                        res_sig_ctx.eval = [residual_eval, idx = res_support,
                                            full = std::vector< uint64_t >(ctx.vars.size(), 0)](
                                               const std::vector< uint64_t > &rv
                                           ) mutable -> uint64_t {
                            for (size_t i = 0; i < idx.size(); ++i) { full[idx[i]] = rv[i]; }
                            uint64_t result = residual_eval(full);
                            for (size_t i = 0; i < idx.size(); ++i) { full[idx[i]] = 0; }
                            return result;
                        };
                    }

                    auto td = TryTemplateDecomposition(
                        res_sig_ctx, residual_opts, kResRealCount, nullptr
                    );
                    if (td.Succeeded()) {
                        auto solved_expr = std::move(td.TakePayload().expr);
                        if (res_fw_elim.real_vars.size() < ctx.vars.size()) {
                            RemapVarIndices(*solved_expr, res_support);
                        }
                        auto combined =
                            Expr::Add(CloneExpr(*core.expr), std::move(solved_expr));
                        auto check =
                            FullWidthCheckEval(ctx.opts.evaluator, kNv, *combined, kBw);
                        if (check.passed) {
                            COBRA_TRACE(
                                "DecompEngine", "Ghost path: TemplateDecomp fallback succeeded"
                            );
                            DecompositionResult result;
                            result.expr           = std::move(combined);
                            result.extractor_kind = core.kind;
                            result.solver_kind    = ResidualSolverKind::kTemplateDecomposition;
                            result.core_degree    = core.degree_used;
                            return result;
                        }
                        COBRA_TRACE(
                            "DecompEngine", "Ghost TemplateDecomp: recombination failed"
                        );
                    } else {
                        COBRA_TRACE("DecompEngine", "Ghost TemplateDecomp: no solution");
                    }
                }
                COBRA_TRACE(
                    "DecompEngine", "Ghost path: all solvers failed for kind={}",
                    static_cast< int >(core.kind)
                );
            } else {
                // Standard routing (non-ghost residual)
                COBRA_TRACE("DecompEngine", "Standard residual routing");

                // Residual solver 1: Supported pipeline
                {
                    auto res_result =
                        RunSupportedPipeline(residual_sig, ctx.vars, residual_opts);
                    if (res_result.has_value()
                        && res_result.value().kind == SimplifyOutcome::Kind::kSimplified)
                    {
                        auto solved_expr = std::move(res_result.value().expr);
                        if (!res_result.value().real_vars.empty()
                            && res_result.value().real_vars.size() < ctx.vars.size())
                        {
                            auto idx_map =
                                BuildVarSupport(ctx.vars, res_result.value().real_vars);
                            RemapVarIndices(*solved_expr, idx_map);
                        }

                        // Verify solved_expr against residual evaluator
                        // with stronger probing before recombination.
                        // RunSupportedPipeline's internal FW check uses
                        // only 8 deterministic probes, which can false-
                        // positive on boolean-correct but FW-incorrect
                        // expressions.
                        auto res_check =
                            FullWidthCheckEval(residual_eval, kNv, *solved_expr, kBw, 64);
                        if (!res_check.passed) {
                            COBRA_TRACE(
                                "DecompEngine",
                                "SupportedPipeline: residual FW "
                                "recheck failed (false positive)"
                            );
                        } else {
                            auto combined =
                                Expr::Add(CloneExpr(*core.expr), std::move(solved_expr));
                            auto check =
                                FullWidthCheckEval(ctx.opts.evaluator, kNv, *combined, kBw);
                            if (check.passed) {
                                COBRA_TRACE(
                                    "DecompEngine",
                                    "Decomposed: kind={} + "
                                    "SupportedPipeline",
                                    static_cast< int >(core.kind)
                                );
                                DecompositionResult result;
                                result.expr           = std::move(combined);
                                result.extractor_kind = core.kind;
                                result.solver_kind    = ResidualSolverKind::kSupportedPipeline;
                                result.core_degree    = core.degree_used;
                                return result;
                            }
                            COBRA_TRACE(
                                "DecompEngine",
                                "SupportedPipeline: solved but "
                                "recombination failed"
                            );
                        }
                    } else {
                        COBRA_TRACE("DecompEngine", "SupportedPipeline: residual unsupported");
                    }
                }

                // Residual solver 2: Polynomial recovery
                if (kResRealCount <= 6) {
                    auto res_poly = RecoverAndVerifyPoly(
                        residual_eval, res_support, kNv, kBw, 4, degree_floor
                    );
                    if (res_poly.Succeeded()) {
                        auto poly_payload = res_poly.TakePayload();
                        auto combined =
                            Expr::Add(CloneExpr(*core.expr), std::move(poly_payload.expr));
                        auto check =
                            FullWidthCheckEval(ctx.opts.evaluator, kNv, *combined, kBw);
                        if (check.passed) {
                            COBRA_TRACE(
                                "DecompEngine", "Decomposed: kind={} + PolyRecovery(d={})",
                                static_cast< int >(core.kind), poly_payload.degree_used
                            );
                            DecompositionResult result;
                            result.expr           = std::move(combined);
                            result.extractor_kind = core.kind;
                            result.solver_kind    = ResidualSolverKind::kPolynomialRecovery;
                            result.core_degree    = core.degree_used;
                            return result;
                        }
                        COBRA_TRACE("DecompEngine", "PolyRecovery: recombination failed");
                    } else {
                        COBRA_TRACE(
                            "DecompEngine", "PolyRecovery: no polynomial recovered (dfloor={})",
                            degree_floor
                        );
                    }
                } else {
                    COBRA_TRACE(
                        "DecompEngine", "PolyRecovery: skipped (support={} > 6)", kResRealCount
                    );
                }

                // Residual solver 3: Template decomposition
                {
                    SignatureContext res_sig_ctx;
                    res_sig_ctx.vars             = res_fw_elim.real_vars;
                    res_sig_ctx.original_indices = res_support;

                    if (res_fw_elim.real_vars.size() == ctx.vars.size()) {
                        res_sig_ctx.eval = residual_eval;
                    } else {
                        res_sig_ctx.eval = [residual_eval, idx = res_support,
                                            full = std::vector< uint64_t >(ctx.vars.size(), 0)](
                                               const std::vector< uint64_t > &rv
                                           ) mutable -> uint64_t {
                            for (size_t i = 0; i < idx.size(); ++i) { full[idx[i]] = rv[i]; }
                            uint64_t result = residual_eval(full);
                            for (size_t i = 0; i < idx.size(); ++i) { full[idx[i]] = 0; }
                            return result;
                        };
                    }

                    auto td = TryTemplateDecomposition(
                        res_sig_ctx, residual_opts, kResRealCount, nullptr
                    );
                    if (td.Succeeded()) {
                        auto solved_expr = std::move(td.TakePayload().expr);
                        if (res_fw_elim.real_vars.size() < ctx.vars.size()) {
                            RemapVarIndices(*solved_expr, res_support);
                        }
                        auto combined =
                            Expr::Add(CloneExpr(*core.expr), std::move(solved_expr));
                        auto check =
                            FullWidthCheckEval(ctx.opts.evaluator, kNv, *combined, kBw);
                        if (check.passed) {
                            COBRA_TRACE(
                                "DecompEngine", "Decomposed: kind={} + TemplateDecomp",
                                static_cast< int >(core.kind)
                            );
                            DecompositionResult result;
                            result.expr           = std::move(combined);
                            result.extractor_kind = core.kind;
                            result.solver_kind    = ResidualSolverKind::kTemplateDecomposition;
                            result.core_degree    = core.degree_used;
                            return result;
                        }
                        COBRA_TRACE("DecompEngine", "TemplateDecomp: recombination failed");
                    } else {
                        COBRA_TRACE("DecompEngine", "TemplateDecomp: no solution");
                    }
                }
                COBRA_TRACE(
                    "DecompEngine", "Standard path: all solvers failed for kind={}",
                    static_cast< int >(core.kind)
                );
            }

            return std::nullopt;
        };

        // Extractor 1: Product/AST
        {
            auto core = ExtractProductCore(ctx);
            if (core.has_value()) {
                COBRA_TRACE("DecompEngine", "Trying ProductAST core");
                auto result = TryCore(*core);
                if (result.has_value()) { return result; }
                COBRA_TRACE("DecompEngine", "ProductAST core: all solvers exhausted");
            } else {
                COBRA_TRACE("DecompEngine", "ProductAST: no core extracted");
            }
        }

        // Extractor 2: Polynomial D=2
        {
            auto core = ExtractPolyCore(ctx, 2);
            if (core.has_value()) {
                COBRA_TRACE("DecompEngine", "Trying Polynomial D=2 core");
                auto result = TryCore(*core);
                if (result.has_value()) { return result; }
                COBRA_TRACE("DecompEngine", "Polynomial D=2 core: all solvers exhausted");
            } else {
                COBRA_TRACE("DecompEngine", "Polynomial D=2: no core extracted");
            }
        }

        // Extractor 3: Template
        {
            auto core = ExtractTemplateCore(ctx);
            if (core.has_value()) {
                COBRA_TRACE("DecompEngine", "Trying Template core");
                auto result = TryCore(*core);
                if (result.has_value()) { return result; }
                COBRA_TRACE("DecompEngine", "Template core: all solvers exhausted");
            } else {
                COBRA_TRACE("DecompEngine", "Template: no core extracted");
            }
        }

        // Extractor 4: Polynomial D=3
        {
            auto core = ExtractPolyCore(ctx, 3);
            if (core.has_value()) {
                COBRA_TRACE("DecompEngine", "Trying Polynomial D=3 core");
                auto result = TryCore(*core);
                if (result.has_value()) { return result; }
                COBRA_TRACE("DecompEngine", "Polynomial D=3 core: all solvers exhausted");
            } else {
                COBRA_TRACE("DecompEngine", "Polynomial D=3: no core extracted");
            }
        }

        // Extractor 5: Polynomial D=4
        {
            auto core = ExtractPolyCore(ctx, 4);
            if (core.has_value()) {
                COBRA_TRACE("DecompEngine", "Trying Polynomial D=4 core");
                auto result = TryCore(*core);
                if (result.has_value()) { return result; }
                COBRA_TRACE("DecompEngine", "Polynomial D=4 core: all solvers exhausted");
            } else {
                COBRA_TRACE("DecompEngine", "Polynomial D=4: no core extracted");
            }
        }

        COBRA_TRACE("DecompEngine", "All extractors exhausted, returning nullopt");
        return std::nullopt;
    }

} // namespace cobra
