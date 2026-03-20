#include "cobra/core/MixedProductRewriter.h"
#include "cobra/core/Classification.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/Trace.h"
#include <cstdint>
#include <memory>
#include <utility>

namespace cobra {

    uint32_t NodeCount(const Expr &expr) {
        uint32_t count = 1;
        for (const auto &child : expr.children) { count += NodeCount(*child); }
        return count;
    }

    namespace {

        // Context for top-down walk: are we inside a mixed-product
        // or bitwise-over-arithmetic context?
        struct RewriteContext
        {
            bool in_mixed_product      = false;
            bool in_bitwise_over_arith = false;
        };

        bool IsInUnsupportedContext(const RewriteContext &ctx) {
            return ctx.in_mixed_product || ctx.in_bitwise_over_arith;
        }

        bool HasArithVar(const Expr &expr) {
            if (expr.kind == Expr::Kind::kAdd || expr.kind == Expr::Kind::kMul
                || expr.kind == Expr::Kind::kNeg)
            {
                if (HasVarDep(expr)) { return true; }
            }
            for (const auto &c : expr.children) {
                if (HasArithVar(*c)) { return true; }
            }
            return false;
        }

        uint32_t CountSitesImpl(const Expr &expr, const RewriteContext &ctx) {
            uint32_t count = 0;

            RewriteContext child_ctx = ctx;

            if (expr.kind == Expr::Kind::kMul) {
                if (expr.children.size() == 2) {
                    const bool kLhsBw = HasNonleafBitwise(*expr.children[0]);
                    const bool kRhsBw = HasNonleafBitwise(*expr.children[1]);
                    const bool kLhsVd = HasVarDep(*expr.children[0]);
                    const bool kRhsVd = HasVarDep(*expr.children[1]);
                    if ((kLhsBw || kRhsBw) && kLhsVd && kRhsVd) {
                        child_ctx.in_mixed_product = true;
                    }
                }
            }

            if (expr.kind == Expr::Kind::kAnd || expr.kind == Expr::Kind::kOr
                || expr.kind == Expr::Kind::kXor)
            {
                if (expr.children.size() == 2) {
                    if (HasArithVar(*expr.children[0]) || HasArithVar(*expr.children[1])) {
                        child_ctx.in_bitwise_over_arith = true;
                    }
                }
            }

            if (expr.kind == Expr::Kind::kNot) {
                if (!expr.children.empty() && HasArithVar(*expr.children[0])) {
                    child_ctx.in_bitwise_over_arith = true;
                }
            }

            if (expr.kind == Expr::Kind::kXor && expr.children.size() == 2
                && IsInUnsupportedContext(child_ctx))
            {
                count += 1;
            }

            for (const auto &c : expr.children) { count += CountSitesImpl(*c, child_ctx); }

            return count;
        }

        // Apply XOR lowering: x ^ y -> x + y - 2*(x & y)
        // Only in unsupported contexts.
        // NOLINTNEXTLINE(readability-identifier-naming)
        std::unique_ptr< Expr > ApplyXorLoweringImpl(
            std::unique_ptr< Expr > expr, const RewriteContext &ctx, uint32_t bitwidth
        ) {
            RewriteContext child_ctx = ctx;

            if (expr->kind == Expr::Kind::kMul) {
                if (expr->children.size() == 2) {
                    const bool lhs_bw = HasNonleafBitwise(*expr->children[0]);
                    const bool rhs_bw = HasNonleafBitwise(*expr->children[1]);
                    const bool lhs_vd = HasVarDep(*expr->children[0]);
                    const bool rhs_vd = HasVarDep(*expr->children[1]);
                    if ((lhs_bw || rhs_bw) && lhs_vd && rhs_vd) {
                        child_ctx.in_mixed_product = true;
                    }
                }
            }

            if (expr->kind == Expr::Kind::kAnd || expr->kind == Expr::Kind::kOr
                || expr->kind == Expr::Kind::kXor)
            {
                if (expr->children.size() == 2) {
                    if (HasArithVar(*expr->children[0]) || HasArithVar(*expr->children[1])) {
                        child_ctx.in_bitwise_over_arith = true;
                    }
                }
            }

            if (expr->kind == Expr::Kind::kNot) {
                if (!expr->children.empty() && HasArithVar(*expr->children[0])) {
                    child_ctx.in_bitwise_over_arith = true;
                }
            }

            // Recurse into children first (bottom-up)
            for (auto &c : expr->children) {
                c = ApplyXorLoweringImpl(std::move(c), child_ctx, bitwidth);
            }

            // Apply XOR lowering if in unsupported context
            if (expr->kind == Expr::Kind::kXor && IsInUnsupportedContext(child_ctx)
                && expr->children.size() == 2)
            {
                COBRA_TRACE("MixedRewriter", "  XOR lowering: x^y -> x+y-2*(x&y)");
                // x ^ y -> x + y - 2*(x & y)
                auto lhs  = CloneExpr(*expr->children[0]);
                auto rhs  = CloneExpr(*expr->children[1]);
                auto lhs2 = CloneExpr(*expr->children[0]);
                auto rhs2 = CloneExpr(*expr->children[1]);

                auto sum         = Expr::Add(std::move(lhs), std::move(rhs));
                auto and_term    = Expr::BitwiseAnd(std::move(lhs2), std::move(rhs2));
                auto two_and     = Expr::Mul(Expr::Constant(2), std::move(and_term));
                auto neg_two_and = Expr::Negate(std::move(two_and));
                return Expr::Add(std::move(sum), std::move(neg_two_and));
            }

            return expr;
        }

    } // namespace

    uint32_t CountRewriteableSites(const Expr &expr) {
        const RewriteContext kCtx;
        return CountSitesImpl(expr, kCtx);
    }

    RewriteResult RewriteMixedProducts(
        std::unique_ptr< Expr > expr, const RewriteOptions &opts
    ) { // NOLINT(readability-identifier-naming)
        auto cls = ClassifyStructural(*expr);
        if (cls.route != Route::kMixedRewrite) {
            return { .expr = std::move(expr), .rounds_applied = 0, .structure_changed = false };
        }

        const uint32_t initial_count = NodeCount(*expr);
        auto old_flags               = cls.flags & kUnsupportedFlagMask;
        auto old_sites               = CountRewriteableSites(*expr);
        COBRA_TRACE(
            "MixedRewriter",
            "RewriteMixedProducts: max_rounds={} max_growth={} initial_nodes={} "
            "initial_sites={}",
            opts.max_rounds, opts.max_node_growth, initial_count, old_sites
        );

        RewriteResult result;
        result.expr              = std::move(expr);
        result.rounds_applied    = 0;
        result.structure_changed = false;

        for (uint32_t round = 1; round <= opts.max_rounds; ++round) {
            const RewriteContext ctx;
            auto new_expr = ApplyXorLoweringImpl(CloneExpr(*result.expr), ctx, opts.bitwidth);

            const uint32_t new_count = NodeCount(*new_expr);
            if (new_count > initial_count * opts.max_node_growth) {
                COBRA_TRACE(
                    "MixedRewriter", "Round {}: ABORT — node growth exceeded ({}>{})", round,
                    new_count, initial_count * opts.max_node_growth
                );
                break;
            }

            auto new_cls   = ClassifyStructural(*new_expr);
            auto new_flags = new_cls.flags & kUnsupportedFlagMask;
            auto new_sites = CountRewriteableSites(*new_expr);
            COBRA_TRACE(
                "MixedRewriter", "Round {}: nodes={} sites={} flags=0x{:x}", round, new_count,
                new_sites, static_cast< uint32_t >(new_flags)
            );

            // Safety: reject if new unsupported flags appeared
            if (static_cast< uint32_t >(new_flags & ~old_flags) != 0) {
                COBRA_TRACE(
                    "MixedRewriter", "Round {}: ABORT — new unsupported flags appeared", round
                );
                break;
            }

            // Progress check
            const bool coarse_progress = (new_flags != old_flags)
                && ((static_cast< uint32_t >(new_flags) & static_cast< uint32_t >(old_flags))
                    == static_cast< uint32_t >(new_flags));
            const bool fine_progress = (new_sites < old_sites);
            COBRA_TRACE(
                "MixedRewriter", "Round {}: coarse_progress={} fine_progress={}", round,
                coarse_progress, fine_progress
            );

            if (!coarse_progress && !fine_progress) {
                COBRA_TRACE("MixedRewriter", "Round {}: STOP — no progress", round);
                break;
            }

            result.expr              = std::move(new_expr);
            old_flags                = new_flags;
            old_sites                = new_sites;
            result.structure_changed = true;
            result.rounds_applied    = round;

            if (DeriveRoute(new_flags) != Route::kMixedRewrite) {
                COBRA_TRACE("MixedRewriter", "Round {}: route changed — done", round);
                break;
            }
        }

        COBRA_TRACE(
            "MixedRewriter", "RewriteMixedProducts: total_rounds={} changed={}",
            result.rounds_applied, result.structure_changed
        );
        return result;
    }

} // namespace cobra
