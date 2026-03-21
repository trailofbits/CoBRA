#include "cobra/core/OperandSimplifier.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/SignatureSimplifier.h"
#include "cobra/core/Simplifier.h"
#include "cobra/core/Trace.h"
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace cobra {
    namespace {

        bool IsConstant(const Expr &e, uint64_t val) {
            return e.kind == Expr::Kind::kConstant && e.constant_val == val;
        }

        // --- Local rebuild cleanup helpers ---

        std::unique_ptr< Expr > MakeMulSimplified(
            std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs
        ) { // NOLINT(readability-identifier-naming)
            if (IsConstant(*lhs, 0) || IsConstant(*rhs, 0)) { return Expr::Constant(0); }
            if (IsConstant(*lhs, 1)) { return rhs; }
            if (IsConstant(*rhs, 1)) { return lhs; }
            return Expr::Mul(std::move(lhs), std::move(rhs));
        }

        std::unique_ptr< Expr > MakeAddSimplified(
            std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs
        ) { // NOLINT(readability-identifier-naming)
            if (IsConstant(*lhs, 0)) { return rhs; }
            if (IsConstant(*rhs, 0)) { return lhs; }
            return Expr::Add(std::move(lhs), std::move(rhs));
        }

        // NOLINTNEXTLINE(readability-identifier-naming)
        std::unique_ptr< Expr > MakeAndSimplified(
            std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs, uint32_t bitwidth
        ) {
            const uint64_t mask = Bitmask(bitwidth);
            if (IsConstant(*lhs, 0) || IsConstant(*rhs, 0)) { return Expr::Constant(0); }
            if (IsConstant(*lhs, mask)) { return rhs; }
            if (IsConstant(*rhs, mask)) { return lhs; }
            return Expr::BitwiseAnd(std::move(lhs), std::move(rhs));
        }

        // NOLINTNEXTLINE(readability-identifier-naming)
        std::unique_ptr< Expr > MakeOrSimplified(
            std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs, uint32_t bitwidth
        ) {
            const uint64_t mask = Bitmask(bitwidth);
            if (IsConstant(*lhs, 0)) { return rhs; }
            if (IsConstant(*rhs, 0)) { return lhs; }
            if (IsConstant(*lhs, mask) || IsConstant(*rhs, mask)) {
                return Expr::Constant(mask);
            }
            return Expr::BitwiseOr(std::move(lhs), std::move(rhs));
        }

        std::unique_ptr< Expr > MakeXorSimplified(
            std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs
        ) { // NOLINT(readability-identifier-naming)
            if (IsConstant(*lhs, 0)) { return rhs; }
            if (IsConstant(*rhs, 0)) { return lhs; }
            return Expr::BitwiseXor(std::move(lhs), std::move(rhs));
        }

        // NOLINTNEXTLINE(readability-identifier-naming)
        std::unique_ptr< Expr > MakeNegSimplified(std::unique_ptr< Expr > operand) {
            if (operand->kind == Expr::Kind::kNeg) { return std::move(operand->children[0]); }
            return Expr::Negate(std::move(operand));
        }

        // NOLINTNEXTLINE(readability-identifier-naming)
        std::unique_ptr< Expr > MakeNotSimplified(std::unique_ptr< Expr > operand) {
            if (operand->kind == Expr::Kind::kNot) { return std::move(operand->children[0]); }
            return Expr::BitwiseNot(std::move(operand));
        }

        // --- Post-order walk ---

        struct WalkInfo
        {
            std::unique_ptr< Expr > expr;
            bool changed = false;
        };

        std::unique_ptr< Expr > RebuildNode(
            Expr::Kind kind,
            std::vector< std::unique_ptr< Expr > >
                children, // NOLINT(hicpp-named-parameter,readability-named-parameter)
            uint64_t constant_val, uint32_t var_index, uint32_t bitwidth
        ) {
            switch (kind) {
                case Expr::Kind::kAdd:
                    return MakeAddSimplified(std::move(children[0]), std::move(children[1]));
                case Expr::Kind::kMul:
                    return MakeMulSimplified(std::move(children[0]), std::move(children[1]));
                case Expr::Kind::kAnd:
                    return MakeAndSimplified(
                        std::move(children[0]), std::move(children[1]), bitwidth
                    );
                case Expr::Kind::kOr:
                    return MakeOrSimplified(
                        std::move(children[0]), std::move(children[1]), bitwidth
                    );
                case Expr::Kind::kXor:
                    return MakeXorSimplified(std::move(children[0]), std::move(children[1]));
                case Expr::Kind::kNot:
                    return MakeNotSimplified(std::move(children[0]));
                case Expr::Kind::kNeg:
                    return MakeNegSimplified(std::move(children[0]));
                case Expr::Kind::kConstant:
                    return Expr::Constant(constant_val);
                case Expr::Kind::kVariable:
                    return Expr::Variable(var_index);
                case Expr::Kind::kShr:
                    return Expr::LogicalShr(std::move(children[0]), constant_val);
            }
            __builtin_unreachable();
        }

        std::optional< std::unique_ptr< Expr > > TrySimplifyOperand(
            const Expr &operand, const std::vector< std::string > &vars, const Options &opts
        ) {
            const auto num_vars = static_cast< uint32_t >(vars.size());

            auto sig = EvaluateBooleanSignature(operand, num_vars, opts.bitwidth);

            // Build context without evaluator — subtree signature
            // from the AST is exact on the Boolean cube.
            SignatureContext ctx;
            ctx.vars = vars;
            ctx.original_indices.resize(num_vars);
            std::iota(ctx.original_indices.begin(), ctx.original_indices.end(), 0U);
            // No evaluator: ctx.eval stays nullopt

            auto operand_cost = ComputeCost(operand).cost;

            // Clear evaluator: subtree signature is ground truth,
            // no full-width check needed for operand-level calls.
            Options sub_opts   = opts;
            sub_opts.evaluator = Evaluator{};

            auto result = SimplifyFromSignature(sig, ctx, sub_opts, 0, &operand_cost);

            if (!result.has_value()) { return std::nullopt; }
            if (!IsBetter(result->cost, operand_cost)) { return std::nullopt; }

            // Verify the simplified operand is FW-equivalent to
            // the original. Boolean-equivalent replacement can
            // differ on full-width inputs when the operand
            // contains arithmetic (Mul, Add) — propagated
            // through an outer Mul, this breaks semantics.
            const uint64_t kMask = Bitmask(opts.bitwidth);
            std::mt19937_64 rng(0xCA5E + num_vars);
            constexpr int kProbes = 8;
            for (int p = 0; p < kProbes; ++p) {
                std::vector< uint64_t > pt(num_vars);
                for (uint32_t i = 0; i < num_vars; ++i) { pt[i] = rng() & kMask; }
                uint64_t orig_val = EvalExpr(operand, pt, opts.bitwidth) & kMask;
                uint64_t simp_val = EvalExpr(*result->expr, pt, opts.bitwidth) & kMask;
                if (orig_val != simp_val) { return std::nullopt; }
            }

            return std::move(result->expr);
        }

        // Try to simplify operands of a mixed Mul node. Returns the
        // improved Mul if any operand simplified to a strictly cheaper
        // result, or nullopt if no improvement was found.
        std::optional< std::unique_ptr< Expr > > TrySimplifyMul(
            const Expr &mul, const std::vector< std::string > &vars, const Options &opts
        ) {
            const bool lhs_vd = HasVarDep(*mul.children[0]);
            const bool rhs_vd = HasVarDep(*mul.children[1]);
            if (!lhs_vd || !rhs_vd) { return std::nullopt; }

            const bool lhs_bw = HasNonleafBitwise(*mul.children[0]);
            const bool rhs_bw = HasNonleafBitwise(*mul.children[1]);
            if (!lhs_bw && !rhs_bw) { return std::nullopt; }

            auto orig_cost = ComputeCost(mul).cost;

            // Only attempt simplification on sides with bitwise structure.
            auto lhs_simp =
                lhs_bw ? TrySimplifyOperand(*mul.children[0], vars, opts) : std::nullopt;
            auto rhs_simp =
                rhs_bw ? TrySimplifyOperand(*mul.children[1], vars, opts) : std::nullopt;

            struct Candidate
            {
                std::unique_ptr< Expr > expr;
                ExprCost cost;
            };

            std::optional< Candidate > best;

            auto try_candidate = [&](std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs) {
                auto mul = MakeMulSimplified(std::move(lhs), std::move(rhs));
                auto c   = ComputeCost(*mul).cost;
                if (IsBetter(c, orig_cost) && (!best.has_value() || IsBetter(c, best->cost))) {
                    best = Candidate{ .expr = std::move(mul), .cost = c };
                }
            };

            if (lhs_simp.has_value()) {
                try_candidate(CloneExpr(**lhs_simp), CloneExpr(*mul.children[1]));
            }
            if (rhs_simp.has_value()) {
                try_candidate(CloneExpr(*mul.children[0]), CloneExpr(**rhs_simp));
            }
            if (lhs_simp.has_value() && rhs_simp.has_value()) {
                try_candidate(CloneExpr(**lhs_simp), CloneExpr(**rhs_simp));
            }

            if (!best.has_value()) { return std::nullopt; }
            return std::move(best->expr);
        }

        // NOLINTNEXTLINE(readability-identifier-naming)
        WalkInfo Walk(
            std::unique_ptr< Expr > expr, const std::vector< std::string > &vars,
            const Options &opts
        ) {
            WalkInfo info;
            bool child_changed = false;

            // Recurse into children (post-order)
            for (auto &c : expr->children) {
                auto child    = Walk(std::move(c), vars, opts);
                c             = std::move(child.expr);
                child_changed = child_changed || child.changed;
            }

            // Rebuild through cleanup helpers
            auto rebuilt = RebuildNode(
                expr->kind, std::move(expr->children), expr->constant_val, expr->var_index,
                opts.bitwidth
            );

            // Check for mixed Mul eligibility
            if (rebuilt->kind == Expr::Kind::kMul && rebuilt->children.size() == 2) {
                auto improved = TrySimplifyMul(*rebuilt, vars, opts);
                if (improved.has_value()) {
                    rebuilt       = std::move(*improved);
                    child_changed = true;
                }
            }

            info.expr    = std::move(rebuilt);
            info.changed = child_changed;
            return info;
        }

    } // namespace

    // NOLINTNEXTLINE(readability-identifier-naming)
    OperandSimplifyResult SimplifyMixedOperands(
        std::unique_ptr< Expr > expr, const std::vector< std::string > &vars,
        const Options &opts
    ) {
        COBRA_TRACE("OperandSimp", "SimplifyMixedOperands: starting");
        // Single pass only in v1. The spec allows a second pass
        // if the first changed something, but one pass is sufficient
        // for the target expressions.
        auto walk_result = Walk(std::move(expr), vars, opts);
        auto result      = OperandSimplifyResult{ .expr    = std::move(walk_result.expr),
                                                  .changed = walk_result.changed };
        COBRA_TRACE("OperandSimp", "SimplifyMixedOperands: changed={}", result.changed);
        return result;
    }

} // namespace cobra
