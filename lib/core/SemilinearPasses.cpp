#include "SemilinearPasses.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/ExprUtils.h"

namespace cobra {

    SemilinearIR CloneSemilinearIR(const SemilinearIR &src) {
        SemilinearIR dst;
        dst.constant = src.constant;
        dst.bitwidth = src.bitwidth;
        dst.terms    = src.terms;
        dst.atom_table.reserve(src.atom_table.size());
        for (const auto &info : src.atom_table) {
            AtomInfo clone;
            clone.atom_id         = info.atom_id;
            clone.key             = info.key;
            clone.structural_hash = info.structural_hash;
            clone.provenance      = info.provenance;
            clone.original_subtree =
                info.original_subtree ? CloneExpr(*info.original_subtree) : nullptr;
            dst.atom_table.push_back(std::move(clone));
        }
        return dst;
    }

    // Rewrite k + k*(c^x) -> (-k)*(~c ^ x).
    // The semilinear XOR recovery produces a negated coefficient
    // and an additive constant that cancel via this identity.
    std::unique_ptr< Expr >
    SimplifyXorConstant(std::unique_ptr< Expr > expr, uint32_t bitwidth) {
        if (expr->kind != Expr::Kind::kAdd) { return expr; }
        if (expr->children.size() != 2) { return expr; }

        // Match Add(Constant(k), Mul(Constant(k), XOR(Constant(c), ...)))
        // or   Add(Mul(Constant(k), XOR(Constant(c), ...)), Constant(k))
        const Expr *const_node = nullptr;
        const Expr *mul_node   = nullptr;
        int const_idx          = -1;

        for (int i = 0; i < 2; ++i) {
            if (expr->children[i]->kind == Expr::Kind::kConstant
                && expr->children[1 - i]->kind == Expr::Kind::kMul)
            {
                const_node = expr->children[i].get();
                mul_node   = expr->children[1 - i].get();
                const_idx  = i;
                break;
            }
        }
        (void) const_idx;
        if (const_node == nullptr) { return expr; }

        // Mul must have Constant(k) as first child and XOR as second.
        if (mul_node->children.size() != 2) { return expr; }
        const auto &mul_lhs = *mul_node->children[0];
        const auto &mul_rhs = *mul_node->children[1];
        if (mul_lhs.kind != Expr::Kind::kConstant) { return expr; }
        if (mul_rhs.kind != Expr::Kind::kXor) { return expr; }

        // Constants must match: k == k
        if (const_node->constant_val != mul_lhs.constant_val) { return expr; }

        // XOR must have a constant child.
        const auto &xor_node = mul_rhs;
        if (xor_node.children.size() != 2) { return expr; }

        int xor_const_idx = -1;
        if (xor_node.children[0]->kind == Expr::Kind::kConstant) {
            xor_const_idx = 0;
        } else if (xor_node.children[1]->kind == Expr::Kind::kConstant) {
            xor_const_idx = 1;
        }
        if (xor_const_idx < 0) { return expr; }

        // k + k*(c^x) = (-k)*(~c^x)
        const uint64_t kMask = Bitmask(bitwidth);
        const uint64_t kK    = const_node->constant_val;
        const uint64_t kNegK = ModNeg(kK, bitwidth);
        const uint64_t kC    = xor_node.children[xor_const_idx]->constant_val;
        const uint64_t kNotC = (~kC) & kMask;

        auto var_child = CloneExpr(*xor_node.children[1 - xor_const_idx]);
        auto new_xor   = Expr::BitwiseXor(Expr::Constant(kNotC), std::move(var_child));
        return ApplyCoefficient(std::move(new_xor), kNegK, bitwidth);
    }

} // namespace cobra
