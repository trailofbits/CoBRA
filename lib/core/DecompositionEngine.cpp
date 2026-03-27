#include "cobra/core/DecompositionEngine.h"
#include "DecompositionPassHelpers.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/MultivarPolyRecovery.h"
#include "cobra/core/PassContract.h"
#include "cobra/core/PolyExprBuilder.h"
#include "cobra/core/Profile.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/TemplateDecomposer.h"
#include "cobra/core/Trace.h"
#include <cstdint>
#include <memory>
#include <numeric>
#include <random>

namespace cobra {

    // DecompositionMeta stores enum values as uint8_t to avoid pulling
    // DecompositionEngine.h types into PassContract.h. Guard against
    // enum growth silently overflowing the uint8_t storage.
    static_assert(
        static_cast< int >(ExtractorKind::kBooleanNullDirect) <= 255,
        "ExtractorKind exceeds uint8_t range in DecompositionMeta"
    );
    static_assert(
        static_cast< int >(ResidualSolverKind::kTemplateDecomposition) <= 255,
        "ResidualSolverKind exceeds uint8_t range in DecompositionMeta"
    );

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
    BuildRemainderEvaluator(const Evaluator &original, const Expr &core, uint32_t bitwidth) {
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

    SolverResult< CoreCandidate > ExtractProductCore(const DecompositionContext &ctx) {
        COBRA_ZONE_N("ExtractProductCore");
        if (ctx.current_expr == nullptr) {
            return SolverResult< CoreCandidate >::Inapplicable(
                ReasonDetail{
                    .top = { .code = { ReasonCategory::kGuardFailed,
                                       ReasonDomain::kDecomposition, decomposition::kNoExpr },
                            .message = "no expression provided" }
            }
            );
        }

        std::vector< const Expr * > products;
        std::vector< std::unique_ptr< Expr > > residual;
        SplitAddTree(*ctx.current_expr, products, residual);

        if (products.empty()) {
            return SolverResult< CoreCandidate >::Blocked(
                ReasonDetail{
                    .top = { .code    = { ReasonCategory::kSearchExhausted,
                                          ReasonDomain::kDecomposition,
                                          decomposition::kNoProducts },
                            .message = "no product terms found in AST" }
            }
            );
        }

        auto core_expr = CloneExpr(*products[0]);
        for (size_t i = 1; i < products.size(); ++i) {
            core_expr = Expr::Add(std::move(core_expr), CloneExpr(*products[i]));
        }

        CoreCandidate core;
        core.expr = std::move(core_expr);
        core.kind = ExtractorKind::kProductAST;
        return SolverResult< CoreCandidate >::Success(std::move(core));
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

        auto residual_eval = BuildRemainderEvaluator(ctx.opts.evaluator, *core.expr, kBw);

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

    SolverResult< CoreCandidate >
    ExtractPolyCore(const DecompositionContext &ctx, uint8_t degree) {
        COBRA_ZONE_N("ExtractPolyCore");
        if (!ctx.opts.evaluator) {
            return SolverResult< CoreCandidate >::Inapplicable(
                ReasonDetail{
                    .top = { .code    = { ReasonCategory::kGuardFailed,
                                          ReasonDomain::kDecomposition,
                                          decomposition::kNoEvaluator },
                            .message = "polynomial extraction requires evaluator" }
            }
            );
        }

        const auto kNv     = static_cast< uint32_t >(ctx.vars.size());
        const uint32_t kBw = ctx.opts.bitwidth;

        // Support discovery: full-width EliminateAuxVars
        auto fw_elim          = EliminateAuxVars(ctx.sig, ctx.vars, ctx.opts.evaluator, kBw);
        const auto kRealCount = static_cast< uint32_t >(fw_elim.real_vars.size());

        if (kRealCount > 6) {
            return SolverResult< CoreCandidate >::Inapplicable(
                ReasonDetail{
                    .top = { .code    = { ReasonCategory::kGuardFailed,
                                          ReasonDomain::kDecomposition,
                                          decomposition::kTooManyVars },
                            .message = "too many real variables for polynomial extraction" }
            }
            );
        }

        auto support = BuildVarSupport(ctx.vars, fw_elim.real_vars);

        auto poly = RecoverMultivarPoly(ctx.opts.evaluator, support, kNv, kBw, degree);
        if (!poly.Succeeded()) {
            return SolverResult< CoreCandidate >::Blocked(
                ReasonDetail{
                    .top = { .code    = { ReasonCategory::kSearchExhausted,
                                          ReasonDomain::kDecomposition,
                                          decomposition::kPolyRecovFailed },
                            .message = "polynomial recovery failed" }
            }
            );
        }

        auto expr = BuildPolyExpr(poly.TakePayload());
        if (!expr.has_value()) {
            return SolverResult< CoreCandidate >::Blocked(
                ReasonDetail{
                    .top = { .code    = { ReasonCategory::kSearchExhausted,
                                          ReasonDomain::kDecomposition,
                                          decomposition::kExprBuildFailed },
                            .message = "expression build from polynomial failed" }
            }
            );
        }

        CoreCandidate core;
        core.expr        = std::move(expr.value());
        core.kind        = ExtractorKind::kPolynomial;
        core.degree_used = degree;
        return SolverResult< CoreCandidate >::Success(std::move(core));
    }

    SolverResult< CoreCandidate > ExtractTemplateCore(const DecompositionContext &ctx) {
        COBRA_ZONE_N("ExtractTemplateCore");
        if (!ctx.opts.evaluator) {
            return SolverResult< CoreCandidate >::Inapplicable(
                ReasonDetail{
                    .top = { .code    = { ReasonCategory::kGuardFailed,
                                          ReasonDomain::kDecomposition,
                                          decomposition::kNoEvaluator },
                            .message = "template extraction requires evaluator" }
            }
            );
        }

        const auto kNv     = static_cast< uint32_t >(ctx.vars.size());
        const uint32_t kBw = ctx.opts.bitwidth;

        // Reduced-variable attempt first
        auto elim           = EliminateAuxVars(ctx.sig, ctx.vars);
        const auto kRvCount = static_cast< uint32_t >(elim.real_vars.size());
        COBRA_PLOT("TemplateNumVars", static_cast< int64_t >(kNv));
        COBRA_PLOT("TemplateReducedVars", static_cast< int64_t >(kRvCount));

        {
            COBRA_ZONE_N("TemplateReducedVar");
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
                if (elim.real_vars.size() < ctx.vars.size()) {
                    RemapVarIndices(*td_payload.expr, sig_ctx.original_indices);
                }
                auto check = FullWidthCheckEval(ctx.opts.evaluator, kNv, *td_payload.expr, kBw);
                if (check.passed) {
                    CoreCandidate core;
                    core.expr = std::move(td_payload.expr);
                    core.kind = ExtractorKind::kTemplate;
                    return SolverResult< CoreCandidate >::Success(std::move(core));
                }
            }
        }

        // Full-variable fallback (only when reduced != full)
        if (elim.real_vars.size() < ctx.vars.size()) {
            COBRA_ZONE_N("TemplateFullVarFallback");
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
                return SolverResult< CoreCandidate >::Success(std::move(core));
            }
        }

        return SolverResult< CoreCandidate >::Blocked(
            ReasonDetail{
                .top = { .code    = { ReasonCategory::kSearchExhausted,
                                      ReasonDomain::kDecomposition,
                                      decomposition::kNoTemplateMatch },
                        .message = "no template match found" }
        }
        );
    }

} // namespace cobra
