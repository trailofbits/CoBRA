#include "cobra/core/AtomSimplifier.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SemilinearIR.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cobra {

    namespace {

        bool IsConst(const Expr &e) { return e.kind == Expr::Kind::kConstant; }

        std::unique_ptr< Expr > TryFoldBinary(
            Expr::Kind kind, std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs,
            uint32_t bitwidth
        ) {
            const bool lc           = IsConst(*lhs);
            const bool rc           = IsConst(*rhs);
            const uint64_t all_ones = Bitmask(bitwidth);

            if (kind == Expr::Kind::kAnd) {
                if (rc && rhs->constant_val == 0) {
                    return Expr::Constant(0);
                }
                if (lc && lhs->constant_val == 0) {
                    return Expr::Constant(0);
                }
                if (rc && rhs->constant_val == all_ones) {
                    return lhs;
                }
                if (lc && lhs->constant_val == all_ones) {
                    return rhs;
                }
            }

            if (kind == Expr::Kind::kOr) {
                if (rc && rhs->constant_val == 0) {
                    return lhs;
                }
                if (lc && lhs->constant_val == 0) {
                    return rhs;
                }
                if (rc && rhs->constant_val == all_ones) {
                    return Expr::Constant(all_ones);
                }
                if (lc && lhs->constant_val == all_ones) {
                    return Expr::Constant(all_ones);
                }
            }

            if (kind == Expr::Kind::kXor) {
                if (rc && rhs->constant_val == 0) {
                    return lhs;
                }
                if (lc && lhs->constant_val == 0) {
                    return rhs;
                }
            }

            auto result  = std::make_unique< Expr >();
            result->kind = kind;
            result->children.reserve(2);
            result->children.push_back(std::move(lhs));
            result->children.push_back(std::move(rhs));
            return result;
        }

        // Precondition: trees have bounded depth (e.g. post-simplify_atom).
        bool ExprsEqual(const Expr &a, const Expr &b) {
            if (a.kind != b.kind) {
                return false;
            }
            if (a.kind == Expr::Kind::kConstant) {
                return a.constant_val == b.constant_val;
            }
            if (a.kind == Expr::Kind::kVariable) {
                return a.var_index == b.var_index;
            }
            if (a.children.size() != b.children.size()) {
                return false;
            }
            for (size_t i = 0; i < a.children.size(); ++i) {
                if (!ExprsEqual(*a.children[i], *b.children[i])) {
                    return false;
                }
            }
            return true;
        }

    } // namespace

    std::unique_ptr< Expr > SimplifyAtom(std::unique_ptr< Expr > atom, uint32_t bitwidth) {
        if (atom->kind == Expr::Kind::kConstant || atom->kind == Expr::Kind::kVariable) {
            return atom;
        }

        // Bottom-up: simplify children first
        for (auto &child : atom->children) {
            child = SimplifyAtom(std::move(child), bitwidth);
        }

        // Shr identity: x >> 0 -> x
        if (atom->kind == Expr::Kind::kShr && atom->constant_val == 0) {
            return std::move(atom->children[0]);
        }

        // Double NOT elimination
        if (atom->kind == Expr::Kind::kNot && atom->children[0]->kind == Expr::Kind::kNot) {
            return std::move(atom->children[0]->children[0]);
        }

        // De Morgan: ~(~A & ~B) -> A | B
        if (atom->kind == Expr::Kind::kNot) {
            auto &inner = atom->children[0];
            if (inner->kind == Expr::Kind::kAnd && inner->children.size() == 2
                && inner->children[0]->kind == Expr::Kind::kNot
                && inner->children[1]->kind == Expr::Kind::kNot)
            {
                return Expr::BitwiseOr(
                    std::move(inner->children[0]->children[0]),
                    std::move(inner->children[1]->children[0])
                );
            }
            // De Morgan: ~(~A | ~B) -> A & B
            if (inner->kind == Expr::Kind::kOr && inner->children.size() == 2
                && inner->children[0]->kind == Expr::Kind::kNot
                && inner->children[1]->kind == Expr::Kind::kNot)
            {
                return Expr::BitwiseAnd(
                    std::move(inner->children[0]->children[0]),
                    std::move(inner->children[1]->children[0])
                );
            }
        }

        // Idempotency: A & A -> A, A | A -> A
        if ((atom->kind == Expr::Kind::kAnd || atom->kind == Expr::Kind::kOr)
            && atom->children.size() == 2 && ExprsEqual(*atom->children[0], *atom->children[1]))
        {
            return std::move(atom->children[0]);
        }

        // Constant fold: all children constant -> evaluate
        if (IsConstantSubtree(*atom)) {
            return Expr::Constant(EvalConstantExpr(*atom, bitwidth));
        }

        // Binary bitwise identity folds
        if (atom->children.size() == 2) {
            return TryFoldBinary(
                atom->kind, std::move(atom->children[0]), std::move(atom->children[1]), bitwidth
            );
        }

        return atom;
    }

    void SimplifyStructure(SemilinearIR &ir) {
        if (ir.bitwidth == 0 || ir.bitwidth > 64) {
            return;
        }

        const uint64_t mask = (ir.bitwidth >= 64) ? UINT64_MAX : (1ULL << ir.bitwidth) - 1;

        // Merge like atoms: accumulate coefficients by atom_id
        std::unordered_map< AtomId, uint64_t > merged;
        for (const auto &term : ir.terms) {
            merged[term.atom_id] = (merged[term.atom_id] + term.coeff) & mask;
        }

        // Rebuild terms, dropping zeros, sorted by atom_id
        std::vector< WeightedAtom > result;
        for (const auto &[aid, coeff] : merged) {
            if (coeff != 0) {
                result.push_back({ .coeff = coeff, .atom_id = aid });
            }
        }

        std::sort(
            result.begin(), result.end(),
            [](const WeightedAtom &a, const WeightedAtom &b) { return a.atom_id < b.atom_id; }
        );

        ir.terms = std::move(result);

        // Complement recognition: if two atoms share support and have
        // complementary truth tables with equal coefficients, absorb
        // c * mask_all into the constant and remove both terms.
        {
            std::vector< bool > removed(ir.terms.size(), false);
            for (size_t i = 0; i < ir.terms.size(); ++i) {
                if (removed[i]) {
                    continue;
                }
                for (size_t j = i + 1; j < ir.terms.size(); ++j) {
                    if (removed[j]) {
                        continue;
                    }
                    if (ir.terms[i].coeff != ir.terms[j].coeff) {
                        continue;
                    }

                    const auto &ki = ir.atom_table[ir.terms[i].atom_id].key;
                    const auto &kj = ir.atom_table[ir.terms[j].atom_id].key;

                    if (ki.truth_table.empty() || kj.truth_table.empty()) {
                        continue;
                    }
                    if (ki.support != kj.support) {
                        continue;
                    }
                    if (ki.truth_table.size() != kj.truth_table.size()) {
                        continue;
                    }

                    bool complementary = true;
                    for (size_t k = 0; k < ki.truth_table.size(); ++k) {
                        if (ki.truth_table[k] != ((~kj.truth_table[k]) & mask)) {
                            complementary = false;
                            break;
                        }
                    }
                    if (!complementary) {
                        continue;
                    }

                    ir.constant = (ir.constant + (ir.terms[i].coeff * mask)) & mask;
                    removed[i]  = true;
                    removed[j]  = true;
                    break;
                }
            }

            std::vector< WeightedAtom > kept;
            for (size_t i = 0; i < ir.terms.size(); ++i) {
                if (!removed[i]) {
                    kept.push_back(ir.terms[i]);
                }
            }
            ir.terms = std::move(kept);
        }

        // Simplify each atom's expression tree (algebraic identities)
        for (auto &info : ir.atom_table) {
            if (info.original_subtree) {
                info.original_subtree =
                    SimplifyAtom(std::move(info.original_subtree), ir.bitwidth);
            }
        }
    }

} // namespace cobra
