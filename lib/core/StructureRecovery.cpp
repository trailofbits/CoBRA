#include "cobra/core/StructureRecovery.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include "cobra/core/SemilinearIR.h"
#include "cobra/core/Trace.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cobra {

    namespace {

        struct RecoveryTerm
        {
            uint64_t coeff;
            uint64_t mask;
            AtomId atom_id;
            bool consumed = false;
        };

        /// Find or create a bare-variable atom for the given variable index.
        AtomId FindOrCreateBareAtom(SemilinearIR &ir, const Expr &basis) {
            for (size_t k = 0; k < ir.atom_table.size(); ++k) {
                const auto &info = ir.atom_table[k];
                if (info.original_subtree->kind == basis.kind) {
                    if (basis.kind == Expr::Kind::kVariable
                        && info.original_subtree->var_index == basis.var_index)
                    {
                        return static_cast< AtomId >(k);
                    }
                    if (StructuralHash(*info.original_subtree) == StructuralHash(basis)) {
                        return static_cast< AtomId >(k);
                    }
                }
            }
            return CreateAtom(ir, CloneExpr(basis), OperatorFamily::kMixed);
        }

        /// Rebuild ir.terms from basis groups + non-group terms.
        void RebuildTerms(
            SemilinearIR &ir,
            const std::unordered_map< uint64_t, std::vector< RecoveryTerm > > &basis_groups,
            const std::vector< bool > &in_group
        ) {
            const uint64_t kMod = Bitmask(ir.bitwidth);
            std::vector< WeightedAtom > new_terms;

            for (size_t i = 0; i < ir.terms.size(); ++i) {
                if (!in_group[i]) { new_terms.push_back(ir.terms[i]); }
            }
            for (const auto &[hash, entries] : basis_groups) {
                for (const auto &e : entries) {
                    if (e.consumed || e.coeff == 0) { continue; }
                    new_terms.push_back({ .coeff = e.coeff & kMod, .atom_id = e.atom_id });
                }
            }

            std::unordered_map< AtomId, uint64_t > merged;
            for (const auto &t : new_terms) {
                merged[t.atom_id] = (merged[t.atom_id] + t.coeff) & kMod;
            }

            ir.terms.clear();
            for (const auto &[aid, coeff] : merged) {
                if (coeff != 0) { ir.terms.push_back({ .coeff = coeff, .atom_id = aid }); }
            }
            std::sort(
                ir.terms.begin(), ir.terms.end(),
                [](const WeightedAtom &a, const WeightedAtom &b) {
                    return a.atom_id < b.atom_id;
                }
            );
        }

    } // namespace

    void RecoverStructure(SemilinearIR &ir) {
        COBRA_TRACE("StructRecovery", "RecoverStructure: terms={}", ir.terms.size());
        if (ir.terms.size() < 2) { return; }

        const uint64_t kMod = Bitmask(ir.bitwidth);

        // Decompose and group by basis.
        std::unordered_map< uint64_t, std::vector< RecoveryTerm > > basis_groups;
        std::unordered_map< uint64_t, const Expr * > basis_repr;
        std::vector< bool > in_group(ir.terms.size(), false);

        for (size_t i = 0; i < ir.terms.size(); ++i) {
            const auto &term = ir.terms[i];
            auto decomp      = DecomposeAtom(ir.atom_table[term.atom_id], kMod);
            if (!decomp.valid) { continue; }

            in_group[i] = true;
            basis_repr.emplace(decomp.basis_hash, decomp.basis);
            basis_groups[decomp.basis_hash].push_back(
                { .coeff = term.coeff, .mask = decomp.mask, .atom_id = term.atom_id }
            );
        }

        bool any_changed = false;

        for (auto &[hash, entries] : basis_groups) {
            if (entries.size() < 2) { continue; }
            const Expr *basis = basis_repr.at(hash);

            for (size_t i = 0; i < entries.size(); ++i) {
                if (entries[i].consumed) { continue; }
                for (size_t j = i + 1; j < entries.size(); ++j) {
                    if (entries[j].consumed) { continue; }

                    auto &a = entries[i];
                    auto &b = entries[j];

                    // Check for complementary masks.
                    if ((a.mask | b.mask) != kMod) { continue; }
                    if ((a.mask & b.mask) != 0) { continue; }

                    // XOR recovery: m*(c&x) + (-m)*((~c)&x) = -m*(c^x) + m*c
                    if (((a.coeff + b.coeff) & kMod) == 0) {
                        const uint64_t kM = a.coeff;
                        const uint64_t kC = a.mask;

                        auto xor_expr = Expr::BitwiseXor(Expr::Constant(kC), CloneExpr(*basis));
                        AtomId xor_id =
                            CreateAtom(ir, std::move(xor_expr), OperatorFamily::kXor);

                        a.coeff    = b.coeff; // = -m
                        a.atom_id  = xor_id;
                        a.mask     = kMod;
                        b.consumed = true;

                        ir.constant = (ir.constant + (kM * kC)) & kMod;
                        any_changed = true;
                        COBRA_TRACE(
                            "StructRecovery", "XOR recovery: mask={:#x} coeff={}", kC, b.coeff
                        );
                        break;
                    }

                    // Mask elimination: a*(c&x) + b*((~c)&x) = (a-b)*(c&x) + b*x
                    const uint64_t kDiff = (a.coeff - b.coeff) & kMod;
                    if (kDiff != 0) {
                        a.coeff     = kDiff;
                        AtomId bare = FindOrCreateBareAtom(ir, *basis);
                        b.atom_id   = bare;
                        b.mask      = kMod;
                        any_changed = true;
                        COBRA_TRACE(
                            "StructRecovery",
                            "Mask elimination: mask={:#x} diff_coeff={} bare_coeff={}", a.mask,
                            kDiff, b.coeff
                        );
                        break;
                    }
                }
            }
        }

        if (!any_changed) {
            COBRA_TRACE("StructRecovery", "RecoverStructure: no changes");
            return;
        }

        RebuildTerms(ir, basis_groups, in_group);
        COBRA_TRACE("StructRecovery", "RecoverStructure: refined to {} terms", ir.terms.size());
    }

    void CoalesceTerms(SemilinearIR &ir) {
        COBRA_TRACE("StructRecovery", "CoalesceTerms: terms={}", ir.terms.size());
        if (ir.terms.size() < 2) { return; }

        const uint64_t kMod = Bitmask(ir.bitwidth);

        // Decompose and group by basis — only single-variable basis.
        struct GroupTerm
        {
            uint64_t coeff;
            uint64_t mask;
            AtomId atom_id;
        };

        std::unordered_map< uint64_t, std::vector< GroupTerm > > basis_groups;
        std::unordered_map< uint64_t, const Expr * > basis_repr;
        std::vector< bool > in_group(ir.terms.size(), false);

        for (size_t i = 0; i < ir.terms.size(); ++i) {
            const auto &term = ir.terms[i];
            auto decomp      = DecomposeAtom(ir.atom_table[term.atom_id], kMod);
            if (!decomp.valid) { continue; }
            if (decomp.basis->kind != Expr::Kind::kVariable) { continue; }

            in_group[i] = true;
            basis_repr.emplace(decomp.basis_hash, decomp.basis);
            basis_groups[decomp.basis_hash].push_back(
                { .coeff = term.coeff, .mask = decomp.mask, .atom_id = term.atom_id }
            );
        }

        bool any_changed = false;

        for (auto &[hash, entries] : basis_groups) {
            if (entries.size() < 2) { continue; }
            const Expr *basis = basis_repr.at(hash);

            // Compute per-bit effective coefficient:
            // eff[i] = sum(c_k * ((m_k >> i) & 1)) mod 2^bw
            std::vector< uint64_t > eff(ir.bitwidth, 0);
            for (uint32_t bit = 0; bit < ir.bitwidth; ++bit) {
                for (const auto &t : entries) {
                    if (((t.mask >> bit) & 1) != 0) { eff[bit] = (eff[bit] + t.coeff) & kMod; }
                }
            }

            // Group bits by effective coefficient → partitions.
            std::unordered_map< uint64_t, uint64_t > coeff_to_mask;
            for (uint32_t bit = 0; bit < ir.bitwidth; ++bit) {
                if (eff[bit] != 0) { coeff_to_mask[eff[bit]] |= (1ULL << bit); }
            }

            // If the new partition count >= current term count, no improvement.
            if (coeff_to_mask.size() >= entries.size()) { continue; }

            COBRA_TRACE(
                "StructRecovery", "CoalesceTerms: group var={} old_terms={} new_terms={}",
                basis->var_index, entries.size(), coeff_to_mask.size()
            );

            // Replace group with new terms.
            // Mark old entries as needing removal.
            for (auto &e : entries) { e.atom_id = UINT32_MAX; }

            // Create new terms for each partition.
            for (const auto &[coeff, mask] : coeff_to_mask) {
                AtomId aid{};
                if (mask == kMod) {
                    aid = FindOrCreateBareAtom(ir, *basis);
                } else {
                    auto and_expr = Expr::BitwiseAnd(CloneExpr(*basis), Expr::Constant(mask));
                    aid           = CreateAtom(ir, std::move(and_expr), OperatorFamily::kAnd);
                }
                entries.push_back({ .coeff = coeff, .mask = mask, .atom_id = aid });
            }

            any_changed = true;
        }

        if (!any_changed) {
            COBRA_TRACE("StructRecovery", "CoalesceTerms: no changes");
            return;
        }

        // Rebuild terms: skip entries with atom_id == UINT32_MAX (marked for removal).
        std::vector< WeightedAtom > new_terms;
        for (size_t i = 0; i < ir.terms.size(); ++i) {
            if (!in_group[i]) { new_terms.push_back(ir.terms[i]); }
        }
        for (const auto &[hash, entries] : basis_groups) {
            for (const auto &e : entries) {
                if (e.atom_id == UINT32_MAX || e.coeff == 0) { continue; }
                new_terms.push_back({ .coeff = e.coeff & kMod, .atom_id = e.atom_id });
            }
        }

        // Merge duplicate atom_ids.
        std::unordered_map< AtomId, uint64_t > merged;
        for (const auto &t : new_terms) {
            merged[t.atom_id] = (merged[t.atom_id] + t.coeff) & kMod;
        }

        ir.terms.clear();
        for (const auto &[aid, coeff] : merged) {
            if (coeff != 0) { ir.terms.push_back({ .coeff = coeff, .atom_id = aid }); }
        }
        std::sort(
            ir.terms.begin(), ir.terms.end(),
            [](const WeightedAtom &a, const WeightedAtom &b) { return a.atom_id < b.atom_id; }
        );

        COBRA_TRACE("StructRecovery", "CoalesceTerms: refined to {} terms", ir.terms.size());
    }

} // namespace cobra
