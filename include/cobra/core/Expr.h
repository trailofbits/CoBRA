#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cobra {

    struct Expr
    {
        enum class Kind {
            kConstant,
            kVariable,
            kAdd,
            kMul,
            kAnd,
            kOr,
            kXor,
            kNot,
            kNeg,
            kShr,
        };

        Kind kind;
        uint64_t constant_val = 0;
        uint32_t var_index    = 0;
        std::vector< std::unique_ptr< Expr > > children; // move-only by design

        static std::unique_ptr< Expr > Constant(uint64_t val);
        static std::unique_ptr< Expr > Variable(uint32_t index);
        static std::unique_ptr< Expr >
        Add(std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs);
        static std::unique_ptr< Expr >
        Mul(std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs);
        static std::unique_ptr< Expr >
        BitwiseAnd(std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs);
        static std::unique_ptr< Expr >
        BitwiseOr(std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs);
        static std::unique_ptr< Expr >
        BitwiseXor(std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs);
        static std::unique_ptr< Expr > BitwiseNot(std::unique_ptr< Expr > operand);
        static std::unique_ptr< Expr > Negate(std::unique_ptr< Expr > operand);
        static std::unique_ptr< Expr >
        LogicalShr(std::unique_ptr< Expr > operand, uint64_t amount);
    };

    std::unique_ptr< Expr > CloneExpr(const Expr &expr);

    std::string Render(
        const Expr &expr, const std::vector< std::string > &var_names, uint32_t bitwidth = 64
    );

} // namespace cobra

namespace cobra::detail {
    inline size_t hash_combine(size_t seed, size_t value) {
        return seed ^ (value + 0x9e3779b9 + (seed << 6) + (seed >> 2));
    }
} // namespace cobra::detail

template<>
struct std::hash< cobra::Expr >
{
    size_t operator()(const cobra::Expr &e) const {
        size_t h = std::hash< int >{}(static_cast< int >(e.kind));
        h        = cobra::detail::hash_combine(h, std::hash< uint64_t >{}(e.constant_val));
        h        = cobra::detail::hash_combine(h, std::hash< uint32_t >{}(e.var_index));
        for (const auto &child : e.children) {
            h = cobra::detail::hash_combine(h, std::hash< cobra::Expr >{}(*child));
        }
        return h;
    }
};
