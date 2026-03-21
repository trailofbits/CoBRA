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
            expr.var_index = index_map[expr.var_index];
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

} // namespace cobra
