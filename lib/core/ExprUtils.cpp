#include "cobra/core/ExprUtils.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include <algorithm>
#include <bit>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace cobra {

    std::vector< uint32_t > BuildVarSupport(
        const std::vector< std::string > &all_vars,
        const std::vector< std::string > &subset_vars
    ) {
        std::unordered_map< std::string, uint32_t > idx;
        for (uint32_t j = 0; j < all_vars.size(); ++j) { idx[all_vars[j]] = j; }

        std::vector< uint32_t > support;
        support.reserve(subset_vars.size());
        for (const auto &v : subset_vars) { support.push_back(idx.at(v)); }
        return support;
    }

    void RemapVarIndices(Expr &expr, const std::vector< uint32_t > &index_map) {
        if (expr.kind == Expr::Kind::kVariable) {
            expr.var_index = index_map.at(expr.var_index);
            return;
        }
        for (auto &child : expr.children) { RemapVarIndices(*child, index_map); }
    }

    std::unique_ptr< Expr > BuildAndProduct(uint64_t mask) {
        std::unique_ptr< Expr > result;
        while (mask != 0) {
            const auto kBit = static_cast< uint32_t >(std::countr_zero(mask));
            auto var        = Expr::Variable(kBit);
            result =
                result ? Expr::BitwiseAnd(std::move(result), std::move(var)) : std::move(var);
            mask &= mask - 1;
        }
        return result;
    }

    std::unique_ptr< Expr > ApplyCoefficient(
        std::unique_ptr< Expr > expr, uint64_t coeff, uint32_t bitwidth
    ) { // NOLINT(readability-identifier-naming)
        if (coeff == 1) { return expr; }
        if (coeff == Bitmask(bitwidth)) { return Expr::Negate(std::move(expr)); }
        return Expr::Mul(Expr::Constant(coeff), std::move(expr));
    }

    bool IsConstantSubtree(const Expr &expr) {
        if (expr.kind == Expr::Kind::kConstant) { return true; }
        if (expr.kind == Expr::Kind::kVariable) { return false; }
        return std::ranges::all_of(expr.children, [](const auto &child) {
            return IsConstantSubtree(*child);
        });
    }

    uint64_t EvalConstantExpr(const Expr &expr, uint32_t bitwidth) {
        const uint64_t kMask = Bitmask(bitwidth);

        switch (expr.kind) {
            case Expr::Kind::kConstant:
                return expr.constant_val & kMask;
            case Expr::Kind::kNot:
                return ModNot(EvalConstantExpr(*expr.children[0], bitwidth), bitwidth);
            case Expr::Kind::kNeg:
                return ModNeg(EvalConstantExpr(*expr.children[0], bitwidth), bitwidth);
            case Expr::Kind::kAnd:
                return EvalConstantExpr(*expr.children[0], bitwidth)
                    & EvalConstantExpr(*expr.children[1], bitwidth);
            case Expr::Kind::kOr:
                return EvalConstantExpr(*expr.children[0], bitwidth)
                    | EvalConstantExpr(*expr.children[1], bitwidth);
            case Expr::Kind::kXor:
                return EvalConstantExpr(*expr.children[0], bitwidth)
                    ^ EvalConstantExpr(*expr.children[1], bitwidth);
            case Expr::Kind::kAdd:
                return ModAdd(
                    EvalConstantExpr(*expr.children[0], bitwidth),
                    EvalConstantExpr(*expr.children[1], bitwidth), bitwidth
                );
            case Expr::Kind::kMul:
                return ModMul(
                    EvalConstantExpr(*expr.children[0], bitwidth),
                    EvalConstantExpr(*expr.children[1], bitwidth), bitwidth
                );
            case Expr::Kind::kShr:
                return ModShr(
                    EvalConstantExpr(*expr.children[0], bitwidth), expr.constant_val, bitwidth
                );
            case Expr::Kind::kVariable:
                std::unreachable();
        }
        std::unreachable();
    }

    bool HasVarDep(const Expr &expr) {
        if (expr.kind == Expr::Kind::kVariable) { return true; }
        return std::ranges::any_of(expr.children, [](const auto &c) { return HasVarDep(*c); });
    }

    bool HasNonleafBitwise(const Expr &expr) {
        if ((expr.kind == Expr::Kind::kAnd || expr.kind == Expr::Kind::kOr
             || expr.kind == Expr::Kind::kXor || expr.kind == Expr::Kind::kNot)
            && HasVarDep(expr))
        {
            return true;
        }
        return std::ranges::any_of(expr.children, [](const auto &c) {
            return HasNonleafBitwise(*c);
        });
    }

    namespace {

        // Flatten a left-folded Add chain into a list of terms.
        void FlattenAdd(
            std::unique_ptr< Expr > node, std::vector< std::unique_ptr< Expr > > &terms
        ) {
            if (node->kind == Expr::Kind::kAdd) {
                FlattenAdd(std::move(node->children[0]), terms);
                FlattenAdd(std::move(node->children[1]), terms);
            } else {
                terms.push_back(std::move(node));
            }
        }

        std::unique_ptr< Expr >
        FoldConstantArithmetic(std::unique_ptr< Expr > expr, uint32_t bitwidth) {
            // Recurse into children first.
            for (auto &child : expr->children) {
                child = FoldConstantArithmetic(std::move(child), bitwidth);
            }

            // Fold fully-constant subtrees.
            if (IsConstantSubtree(*expr) && expr->kind != Expr::Kind::kConstant) {
                return Expr::Constant(EvalConstantExpr(*expr, bitwidth));
            }

            // Flatten Add chains and combine constant terms.
            if (expr->kind != Expr::Kind::kAdd) { return expr; }

            std::vector< std::unique_ptr< Expr > > terms;
            FlattenAdd(std::move(expr), terms);

            uint64_t const_sum = 0;
            std::vector< std::unique_ptr< Expr > > non_const;
            for (auto &t : terms) {
                if (t->kind == Expr::Kind::kConstant) {
                    const_sum = ModAdd(const_sum, t->constant_val, bitwidth);
                } else {
                    non_const.push_back(std::move(t));
                }
            }

            // Rebuild: non-constant terms first, then constant if nonzero.
            if (non_const.empty()) { return Expr::Constant(const_sum); }

            auto result = std::move(non_const[0]);
            for (size_t i = 1; i < non_const.size(); ++i) {
                result = Expr::Add(std::move(result), std::move(non_const[i]));
            }
            if (const_sum != 0) {
                result = Expr::Add(std::move(result), Expr::Constant(const_sum));
            }
            return result;
        }

        std::unique_ptr< Expr >
        RefoldNegation(std::unique_ptr< Expr > expr, uint32_t bitwidth) {
            for (auto &child : expr->children) {
                child = RefoldNegation(std::move(child), bitwidth);
            }

            // Add(Neg(x), all_ones) or Add(all_ones, Neg(x)) → Not(x)
            // since -x + (2^n - 1) = ~x in modular arithmetic.
            if (expr->kind != Expr::Kind::kAdd) { return expr; }

            const uint64_t kMask = Bitmask(bitwidth);
            auto &lhs            = expr->children[0];
            auto &rhs            = expr->children[1];

            if (lhs->kind == Expr::Kind::kNeg && rhs->kind == Expr::Kind::kConstant
                && rhs->constant_val == kMask)
            {
                return Expr::BitwiseNot(std::move(lhs->children[0]));
            }
            if (rhs->kind == Expr::Kind::kNeg && lhs->kind == Expr::Kind::kConstant
                && lhs->constant_val == kMask)
            {
                return Expr::BitwiseNot(std::move(rhs->children[0]));
            }

            return expr;
        }

    } // namespace

    std::unique_ptr< Expr > CleanupFinalExpr(std::unique_ptr< Expr > expr, uint32_t bitwidth) {
        expr = FoldConstantArithmetic(std::move(expr), bitwidth);
        expr = RefoldNegation(std::move(expr), bitwidth);
        return expr;
    }

} // namespace cobra
