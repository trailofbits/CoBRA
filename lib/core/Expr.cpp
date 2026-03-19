#include "cobra/core/Expr.h"
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace cobra {

    namespace {

        std::unique_ptr< Expr > MakeLeaf(Expr::Kind kind) {
            auto e  = std::make_unique< Expr >();
            e->kind = kind;
            return e;
        }

        std::unique_ptr< Expr > MakeUnary(Expr::Kind kind, std::unique_ptr< Expr > child) {
            auto e  = std::make_unique< Expr >();
            e->kind = kind;
            e->children.push_back(std::move(child));
            return e;
        }

        std::unique_ptr< Expr >
        MakeBinary(Expr::Kind kind, std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs) {
            auto e  = std::make_unique< Expr >();
            e->kind = kind;
            e->children.reserve(2);
            e->children.push_back(std::move(lhs));
            e->children.push_back(std::move(rhs));
            return e;
        }

        int Precedence(Expr::Kind kind) {
            switch (kind) {
                case Expr::Kind::kNot:
                case Expr::Kind::kNeg:
                    return 1;
                case Expr::Kind::kMul:
                    return 2;
                case Expr::Kind::kAdd:
                    return 3;
                case Expr::Kind::kShr:
                    return 4;
                case Expr::Kind::kAnd:
                    return 5;
                case Expr::Kind::kXor:
                    return 6;
                case Expr::Kind::kOr:
                    return 7;
                default:
                    return 0;
            }
        }

        const char *BinopStr(Expr::Kind kind) {
            switch (kind) {
                case Expr::Kind::kAdd:
                    return " + ";
                case Expr::Kind::kMul:
                    return " * ";
                case Expr::Kind::kAnd:
                    return " & ";
                case Expr::Kind::kOr:
                    return " | ";
                case Expr::Kind::kXor:
                    return " ^ ";
                default:
                    return " ? ";
            }
        }

        void RenderImpl(
            std::ostringstream &out, const Expr &expr,
            const std::vector< std::string > &var_names, uint32_t bitwidth, int parent_prec
        ) {
            switch (expr.kind) {
                case Expr::Kind::kConstant: {
                    const uint64_t kMask =
                        (bitwidth >= 64) ? UINT64_MAX : (1ULL << bitwidth) - 1;
                    const uint64_t kHalf =
                        (bitwidth >= 64) ? (1ULL << 63) : (1ULL << (bitwidth - 1));
                    if (expr.constant_val >= kHalf && expr.constant_val <= kMask) {
                        const uint64_t kNeg = (kMask - expr.constant_val) + 1;
                        out << "-" << kNeg;
                    } else {
                        out << expr.constant_val;
                    }
                    break;
                }
                case Expr::Kind::kVariable:
                    out << var_names.at(expr.var_index);
                    break;
                case Expr::Kind::kNot:
                    out << "~";
                    RenderImpl(out, *expr.children[0], var_names, bitwidth, 1);
                    break;
                case Expr::Kind::kNeg:
                    out << "-";
                    RenderImpl(out, *expr.children[0], var_names, bitwidth, 1);
                    break;
                case Expr::Kind::kShr: {
                    const int kPrec         = Precedence(Expr::Kind::kShr);
                    const bool kNeedsParens = kPrec > parent_prec && parent_prec > 0;
                    if (kNeedsParens) { out << "("; }
                    RenderImpl(out, *expr.children[0], var_names, bitwidth, kPrec);
                    out << " >> " << expr.constant_val;
                    if (kNeedsParens) { out << ")"; }
                    break;
                }
                default: {
                    const int kPrec         = Precedence(expr.kind);
                    const bool kNeedsParens = kPrec > parent_prec && parent_prec > 0;
                    if (kNeedsParens) { out << "("; }
                    RenderImpl(out, *expr.children[0], var_names, bitwidth, kPrec);
                    out << BinopStr(expr.kind);
                    RenderImpl(out, *expr.children[1], var_names, bitwidth, kPrec);
                    if (kNeedsParens) { out << ")"; }
                    break;
                }
            }
        }

    } // namespace

    std::unique_ptr< Expr > Expr::Constant(uint64_t val) {
        auto e          = MakeLeaf(Kind::kConstant);
        e->constant_val = val;
        return e;
    }

    std::unique_ptr< Expr > Expr::Variable(uint32_t index) {
        auto e       = MakeLeaf(Kind::kVariable);
        e->var_index = index;
        return e;
    }

    std::unique_ptr< Expr > Expr::Add(
        std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs
    ) { // NOLINT(readability-identifier-naming)
        return MakeBinary(Kind::kAdd, std::move(lhs), std::move(rhs));
    }

    std::unique_ptr< Expr > Expr::Mul(
        std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs
    ) { // NOLINT(readability-identifier-naming)
        return MakeBinary(Kind::kMul, std::move(lhs), std::move(rhs));
    }

    std::unique_ptr< Expr > Expr::BitwiseAnd(
        std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs
    ) { // NOLINT(readability-identifier-naming)
        return MakeBinary(Kind::kAnd, std::move(lhs), std::move(rhs));
    }

    std::unique_ptr< Expr > Expr::BitwiseOr(
        std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs
    ) { // NOLINT(readability-identifier-naming)
        return MakeBinary(Kind::kOr, std::move(lhs), std::move(rhs));
    }

    std::unique_ptr< Expr > Expr::BitwiseXor(
        std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs
    ) { // NOLINT(readability-identifier-naming)
        return MakeBinary(Kind::kXor, std::move(lhs), std::move(rhs));
    }

    // NOLINTNEXTLINE(readability-identifier-naming)
    std::unique_ptr< Expr > Expr::BitwiseNot(std::unique_ptr< Expr > operand) {
        return MakeUnary(Kind::kNot, std::move(operand));
    }

    // NOLINTNEXTLINE(readability-identifier-naming)
    std::unique_ptr< Expr > Expr::Negate(std::unique_ptr< Expr > operand) {
        return MakeUnary(Kind::kNeg, std::move(operand));
    }

    // NOLINTNEXTLINE(readability-identifier-naming)
    std::unique_ptr< Expr > Expr::LogicalShr(std::unique_ptr< Expr > operand, uint64_t amount) {
        auto e          = MakeUnary(Kind::kShr, std::move(operand));
        e->constant_val = amount;
        return e;
    }

    std::unique_ptr< Expr > CloneExpr(const Expr &expr) {
        auto dst          = std::make_unique< Expr >();
        dst->kind         = expr.kind;
        dst->constant_val = expr.constant_val;
        dst->var_index    = expr.var_index;
        for (const auto &child : expr.children) { dst->children.push_back(CloneExpr(*child)); }
        return dst;
    }

    std::string
    Render(const Expr &expr, const std::vector< std::string > &var_names, uint32_t bitwidth) {
        std::ostringstream out;
        RenderImpl(out, expr, var_names, bitwidth, 0);
        return out.str();
    }

} // namespace cobra
