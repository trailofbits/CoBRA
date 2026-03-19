#include "cobra/core/ProductIdentityRecoverer.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/SignatureSimplifier.h"
#include "cobra/core/Simplifier.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cobra {
    namespace {

        uint64_t Bitmask(uint32_t bitwidth) {
            return (bitwidth >= 64) ? UINT64_MAX : ((1ULL << bitwidth) - 1);
        }

        // Attempt to collapse Add(Mul(F0,F1), Mul(F2,F3)) into Mul(x,y)
        // using the MBA product identity:
        //   x*y = (x&y)*(x|y) + (x&~y)*(~x&y)
        //
        // Returns the collapsed Mul(x,y) if the identity matches, or
        // nullopt if no valid assignment was found.
        std::optional< std::unique_ptr< Expr > > TryCollapse(
            const Expr &add_node, const std::vector< std::string > &vars, const Options &opts
        ) {
            const auto num_vars = static_cast< uint32_t >(vars.size());
            if (num_vars == 0) {
                return std::nullopt;
            }

            const uint64_t mask  = Bitmask(opts.bitwidth);
            const size_t sig_len = 1ULL << num_vars;

            // Compute boolean signatures for all 4 factors
            const Expr *factors[4] = {
                add_node.children[0]->children[0].get(),
                add_node.children[0]->children[1].get(),
                add_node.children[1]->children[0].get(),
                add_node.children[1]->children[1].get(),
            };

            std::array< std::vector< uint64_t >, 4 > sigs;
            for (int i = 0; i < 4; ++i) {
                sigs[i] = EvaluateBooleanSignature(*factors[i], num_vars, opts.bitwidth);
            }

            // 8 role assignments: which Mul is (I*O) vs (L*R),
            // and within each Mul, which factor takes which role.
            // Pair {0,1} from lhs Mul, pair {2,3} from rhs Mul.
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

            auto baseline = ComputeCost(add_node).cost;

            for (const auto &a : kAssignments) {
                const auto &sig_i = sigs[a.i];
                const auto &sig_o = sigs[a.o];
                const auto &sig_l = sigs[a.l];
                const auto &sig_r = sigs[a.r];

                // Boolean-cube constraints:
                //   I, L, R pairwise disjoint
                //   O == I | L | R
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
                if (!ok) {
                    continue;
                }

                // Reconstruct factor signatures:
                //   x = I | L,  y = I | R
                std::vector< uint64_t > sig_x(sig_len);
                std::vector< uint64_t > sig_y(sig_len);
                for (size_t j = 0; j < sig_len; ++j) {
                    sig_x[j] = (sig_i[j] | sig_l[j]) & mask;
                    sig_y[j] = (sig_i[j] | sig_r[j]) & mask;
                }

                // Simplify reconstructed signatures into expressions
                SignatureContext ctx;
                ctx.vars = vars;
                ctx.original_indices.resize(num_vars);
                for (uint32_t vi = 0; vi < num_vars; ++vi) {
                    ctx.original_indices[vi] = vi;
                }

                Options sub_opts   = opts;
                sub_opts.evaluator = Evaluator{};

                auto x_res = SimplifyFromSignature(sig_x, ctx, sub_opts, 0);
                if (!x_res.has_value()) {
                    continue;
                }

                auto y_res = SimplifyFromSignature(sig_y, ctx, sub_opts, 0);
                if (!y_res.has_value()) {
                    continue;
                }

                auto candidate = Expr::Mul(std::move(x_res->expr), std::move(y_res->expr));

                // Full-width verification against the original
                auto check = FullWidthCheck(add_node, num_vars, *candidate, {}, opts.bitwidth);
                if (!check.passed) {
                    continue;
                }

                // Cost gate: only accept if strictly cheaper
                auto cand_cost = ComputeCost(*candidate).cost;
                if (!IsBetter(cand_cost, baseline)) {
                    continue;
                }

                return std::move(candidate);
            }

            return std::nullopt;
        }

        struct WalkInfo
        {
            std::unique_ptr< Expr > expr;
            bool changed = false;
        };

        WalkInfo Walk(
            std::unique_ptr< Expr > expr, const std::vector< std::string > &vars,
            const Options &opts
        ) {
            bool child_changed = false;

            for (auto &c : expr->children) {
                auto child    = Walk(std::move(c), vars, opts);
                c             = std::move(child.expr);
                child_changed = child_changed || child.changed;
            }

            if (expr->kind == Expr::Kind::kAdd && expr->children.size() == 2
                && expr->children[0]->kind == Expr::Kind::kMul
                && expr->children[0]->children.size() == 2
                && expr->children[1]->kind == Expr::Kind::kMul
                && expr->children[1]->children.size() == 2)
            {
                auto collapsed = TryCollapse(*expr, vars, opts);
                if (collapsed.has_value()) {
                    return { .expr = std::move(*collapsed), .changed = true };
                }
            }

            return { .expr = std::move(expr), .changed = child_changed };
        }

    } // namespace

    ProductCollapseResult CollapseProductIdentities(
        std::unique_ptr< Expr > expr, const std::vector< std::string > &vars,
        const Options &opts
    ) {
        auto result = Walk(std::move(expr), vars, opts);
        return { .expr = std::move(result.expr), .changed = result.changed };
    }

} // namespace cobra
