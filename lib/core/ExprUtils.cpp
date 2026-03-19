#include "cobra/core/ExprUtils.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include <bit>
#include <cstdint>
#include <memory>
#include <utility>

namespace cobra {

    std::unique_ptr< Expr > BuildAndProduct(uint64_t mask) {
        std::unique_ptr< Expr > result;
        while (mask != 0) {
            const auto bit = static_cast< uint32_t >(std::countr_zero(mask));
            auto var       = Expr::Variable(bit);
            if (result) {
                result = Expr::BitwiseAnd(std::move(result), std::move(var));
            } else {
                result = std::move(var);
            }
            mask &= mask - 1;
        }
        return result;
    }

    std::unique_ptr< Expr >
    ApplyCoefficient(std::unique_ptr< Expr > expr, uint64_t coeff, uint32_t bitwidth) {
        if (coeff == 1) {
            return expr;
        }
        if (coeff == Bitmask(bitwidth)) {
            return Expr::Negate(std::move(expr));
        }
        return Expr::Mul(Expr::Constant(coeff), std::move(expr));
    }

    bool IsConstantSubtree(const Expr &expr) {
        if (expr.kind == Expr::Kind::kConstant) {
            return true;
        }
        if (expr.kind == Expr::Kind::kVariable) {
            return false;
        }
        for (const auto &child : expr.children) {
            if (!IsConstantSubtree(*child)) {
                return false;
            }
        }
        return true;
    }

    uint64_t EvalConstantExpr(const Expr &expr, uint32_t bitwidth) {
        const uint64_t mask = Bitmask(bitwidth);

        switch (expr.kind) {
            case Expr::Kind::kConstant:
                return expr.constant_val & mask;
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
        if (expr.kind == Expr::Kind::kVariable) {
            return true;
        }
        for (const auto &c : expr.children) {
            if (HasVarDep(*c)) {
                return true;
            }
        }
        return false;
    }

    bool HasNonleafBitwise(const Expr &expr) {
        if ((expr.kind == Expr::Kind::kAnd || expr.kind == Expr::Kind::kOr
             || expr.kind == Expr::Kind::kXor || expr.kind == Expr::Kind::kNot)
            && HasVarDep(expr))
        {
            return true;
        }
        for (const auto &c : expr.children) {
            if (HasNonleafBitwise(*c)) {
                return true;
            }
        }
        return false;
    }

} // namespace cobra
