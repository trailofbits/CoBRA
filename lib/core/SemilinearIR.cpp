#include "cobra/core/SemilinearIR.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

size_t std::hash< cobra::AtomKey >::operator()(const cobra::AtomKey &k) const {
    using cobra::detail::hash_combine;
    size_t h = std::hash< size_t >{}(k.support.size());
    for (auto v : k.support) { h = hash_combine(h, std::hash< uint32_t >{}(v)); }
    for (auto t : k.truth_table) { h = hash_combine(h, std::hash< uint64_t >{}(t)); }
    return h;
}

namespace cobra {

    namespace {

        uint64_t EvalExprBool(
            const Expr &e, const std::vector< GlobalVarIdx > &support, uint64_t assignment,
            uint64_t mask
        ) {
            switch (e.kind) {
                case Expr::Kind::kConstant:
                    return e.constant_val & mask;
                case Expr::Kind::kVariable: {
                    for (size_t i = 0; i < support.size(); ++i) {
                        if (support[i] == e.var_index) { return (assignment >> i) & 1; }
                    }
                    return 0;
                }
                case Expr::Kind::kAnd:
                    return EvalExprBool(*e.children[0], support, assignment, mask)
                        & EvalExprBool(*e.children[1], support, assignment, mask);
                case Expr::Kind::kOr:
                    return EvalExprBool(*e.children[0], support, assignment, mask)
                        | EvalExprBool(*e.children[1], support, assignment, mask);
                case Expr::Kind::kXor:
                    return EvalExprBool(*e.children[0], support, assignment, mask)
                        ^ EvalExprBool(*e.children[1], support, assignment, mask);
                case Expr::Kind::kNot:
                    return (~EvalExprBool(*e.children[0], support, assignment, mask)) & mask;
                case Expr::Kind::kShr: {
                    const uint64_t kVal =
                        EvalExprBool(*e.children[0], support, assignment, mask);
                    return (kVal >> e.constant_val) & mask;
                }
                case Expr::Kind::kAdd:
                case Expr::Kind::kMul:
                case Expr::Kind::kNeg:
                    std::unreachable();
            }
            std::unreachable();
        }

    } // namespace

    std::vector< uint64_t > ComputeAtomTruthTable(
        const Expr &atom, const std::vector< GlobalVarIdx > &support, uint32_t bitwidth
    ) {
        const size_t kN = support.size();
        if (kN > 5) { return {}; }
        const size_t kLen    = size_t{ 1 } << kN;
        const uint64_t kMask = Bitmask(bitwidth);
        std::vector< uint64_t > tt(kLen);
        for (size_t i = 0; i < kLen; ++i) { tt[i] = EvalExprBool(atom, support, i, kMask); }
        return tt;
    }

    uint64_t StructuralHash(const Expr &expr) {
        auto h = static_cast< uint64_t >(expr.kind);
        if (expr.kind == Expr::Kind::kConstant || expr.kind == Expr::Kind::kShr) {
            h ^= expr.constant_val * 0x9E3779B97F4A7C15ULL;
        } else if (expr.kind == Expr::Kind::kVariable) {
            h ^= (expr.var_index + 1) * 0x517CC1B727220A95ULL;
        }
        for (const auto &child : expr.children) {
            const uint64_t kCh  = StructuralHash(*child);
            h                  ^= kCh + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
        }
        return h;
    }

    Decomposed DecomposeAtom(const AtomInfo &info, uint64_t modmask) {
        const auto &atom = *info.original_subtree;

        if (atom.kind == Expr::Kind::kVariable) {
            return { .valid      = true,
                     .basis      = &atom,
                     .mask       = modmask,
                     .basis_hash = StructuralHash(atom) };
        }

        if (atom.kind == Expr::Kind::kAnd && atom.children.size() == 2) {
            const bool kLhsConst = IsConstantSubtree(*atom.children[0]);
            const bool kRhsConst = IsConstantSubtree(*atom.children[1]);
            if (kRhsConst && !kLhsConst) {
                const uint64_t kC = EvalConstantExpr(*atom.children[1], 64) & modmask;
                return { .valid      = true,
                         .basis      = atom.children[0].get(),
                         .mask       = kC,
                         .basis_hash = StructuralHash(*atom.children[0]) };
            }
            if (kLhsConst && !kRhsConst) {
                const uint64_t kC = EvalConstantExpr(*atom.children[0], 64) & modmask;
                return { .valid      = true,
                         .basis      = atom.children[1].get(),
                         .mask       = kC,
                         .basis_hash = StructuralHash(*atom.children[1]) };
            }
        }
        return {};
    }

    void CollectVarsFromExpr(const Expr &expr, std::vector< GlobalVarIdx > &out) {
        if (expr.kind == Expr::Kind::kVariable) {
            out.push_back(expr.var_index);
            return;
        }
        for (const auto &child : expr.children) { CollectVarsFromExpr(*child, out); }
    }

    AtomId
    CreateAtom(SemilinearIR &ir, std::unique_ptr< Expr > subtree, OperatorFamily provenance) {
        std::vector< GlobalVarIdx > support;
        CollectVarsFromExpr(*subtree, support);
        std::sort(support.begin(), support.end());
        support.erase(std::unique(support.begin(), support.end()), support.end());

        auto tt     = ComputeAtomTruthTable(*subtree, support, ir.bitwidth);
        auto new_id = static_cast< AtomId >(ir.atom_table.size());

        AtomInfo info;
        info.atom_id          = new_id;
        info.key              = { .support = std::move(support), .truth_table = std::move(tt) };
        info.structural_hash  = StructuralHash(*subtree);
        info.provenance       = provenance;
        info.original_subtree = std::move(subtree);
        ir.atom_table.push_back(std::move(info));
        return new_id;
    }

    void CompactAtomTable(SemilinearIR &ir) {
        // Collect live atom IDs referenced by terms.
        std::vector< bool > live(ir.atom_table.size(), false);
        for (const auto &term : ir.terms) { live[term.atom_id] = true; }

        // Count live atoms; bail if all are live (nothing to compact).
        size_t live_count = 0;
        for (bool b : live) { live_count += b ? 1 : 0; }
        if (live_count == ir.atom_table.size()) { return; }

        // Build old→new ID mapping and compact the table.
        std::vector< AtomId > remap(ir.atom_table.size(), 0);
        std::vector< AtomInfo > compacted;
        compacted.reserve(live_count);
        for (size_t i = 0; i < ir.atom_table.size(); ++i) {
            if (!live[i]) { continue; }
            remap[i]                 = static_cast< AtomId >(compacted.size());
            ir.atom_table[i].atom_id = remap[i];
            compacted.push_back(std::move(ir.atom_table[i]));
        }

        ir.atom_table = std::move(compacted);
        for (auto &term : ir.terms) { term.atom_id = remap[term.atom_id]; }
    }

} // namespace cobra
