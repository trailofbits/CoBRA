#include "cobra/core/DynamicMask.h"

#include <bit>

namespace cobra {

    std::optional< uint32_t > IsPowerOfTwoMinusOne(uint64_t val) {
        if (val == 0) { return std::nullopt; }
        uint64_t next = val + 1;
        // Unsigned wraparound makes UINT64_MAX + 1 become 0, so the
        // full-width all-ones mask is rejected here. Otherwise,
        // val = 2^m - 1 iff val + 1 is a power of 2.
        if (!std::has_single_bit(next)) { return std::nullopt; }
        auto m = static_cast< uint32_t >(std::countr_zero(next));
        if (m == 0 || m >= 64) { return std::nullopt; }
        return m;
    }

    std::optional< MaskInfo > DetectRootLowBitMask(const Expr &expr, uint32_t bitwidth) {
        if (expr.kind != Expr::Kind::kAnd) { return std::nullopt; }
        if (expr.children.size() != 2) { return std::nullopt; }

        const Expr *constant_child = nullptr;
        const Expr *other_child    = nullptr;

        if (expr.children[0]->kind == Expr::Kind::kConstant) {
            constant_child = expr.children[0].get();
            other_child    = expr.children[1].get();
        } else if (expr.children[1]->kind == Expr::Kind::kConstant) {
            constant_child = expr.children[1].get();
            other_child    = expr.children[0].get();
        } else {
            return std::nullopt;
        }

        auto m = IsPowerOfTwoMinusOne(constant_child->constant_val);
        if (!m.has_value()) { return std::nullopt; }
        if (*m >= bitwidth) { return std::nullopt; }

        return MaskInfo{ .effective_width = *m, .inner = other_child };
    }

    bool ContainsShr(const Expr &expr) {
        if (expr.kind == Expr::Kind::kShr) { return true; }
        for (const auto &child : expr.children) {
            if (ContainsShr(*child)) { return true; }
        }
        return false;
    }

} // namespace cobra
