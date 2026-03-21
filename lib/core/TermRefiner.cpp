#include "cobra/core/TermRefiner.h"
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

    bool CanChangeCoefficientTo(
        uint64_t old_coeff, uint64_t new_coeff, uint64_t bitmask, uint32_t bitwidth
    ) {
        const uint64_t kMod = Bitmask(bitwidth);
        for (uint32_t i = 0; i < bitwidth; ++i) {
            const uint64_t kBit = 1ULL << i;
            if ((kMod & (old_coeff * (kBit & bitmask)))
                != (kMod & (new_coeff * (kBit & bitmask))))
            {
                return false;
            }
        }
        return true;
    }

    bool
    CanChangeMaskTo(uint64_t coeff, uint64_t old_mask, uint64_t new_mask, uint32_t bitwidth) {
        const uint64_t kMod = Bitmask(bitwidth);
        for (uint32_t i = 0; i < bitwidth; ++i) {
            const uint64_t kBit = 1ULL << i;
            if ((kMod & (coeff * (kBit & old_mask))) != (kMod & (coeff * (kBit & new_mask)))) {
                return false;
            }
        }
        return true;
    }

    uint64_t ReduceMask(uint64_t coeff, uint64_t mask, uint32_t bitwidth) {
        const uint64_t kMod = Bitmask(bitwidth);
        uint64_t reduced    = 0;
        for (uint32_t i = 0; i < bitwidth; ++i) {
            const uint64_t kBit = 1ULL << i;
            if ((mask & kBit) != 0 && (kMod & (coeff * kBit)) != 0) { reduced |= kBit; }
        }
        return reduced;
    }

    namespace {

        /// A term during refinement: coefficient, constant mask, and atom identity.
        struct RefineTerm
        {
            uint64_t coeff;
            uint64_t mask;
            AtomId atom_id;
            bool consumed = false;
        };

        /// Create a masked atom: BitwiseAnd(basis, Constant(mask)).
        AtomId CreateMaskedAtom(SemilinearIR &ir, const Expr &basis, uint64_t mask) {
            return CreateAtom(
                ir, Expr::BitwiseAnd(CloneExpr(basis), Expr::Constant(mask)),
                OperatorFamily::kAnd
            );
        }

        /// Try to merge two disjoint-mask terms into one.
        /// Returns true if merge succeeded.
        bool TryMergePair(
            RefineTerm &a, RefineTerm &b, SemilinearIR &ir, const Expr &basis, uint64_t modmask
        ) {
            if (a.consumed || b.consumed) { return false; }
            if ((a.mask & b.mask) != 0) { return false; }

            // Same coefficient: direct merge
            if (a.coeff == b.coeff) {
                const uint64_t kMergedMask = (a.mask | b.mask) & modmask;
                a.atom_id                  = CreateMaskedAtom(ir, basis, kMergedMask);
                a.mask                     = kMergedMask;
                b.consumed                 = true;
                return true;
            }

            // Can change b's coefficient to match a's?
            if (CanChangeCoefficientTo(b.coeff, a.coeff, b.mask, ir.bitwidth)) {
                b.coeff                    = a.coeff;
                const uint64_t kMergedMask = (a.mask | b.mask) & modmask;
                a.atom_id                  = CreateMaskedAtom(ir, basis, kMergedMask);
                a.mask                     = kMergedMask;
                b.consumed                 = true;
                return true;
            }

            // Can change a's coefficient to match b's?
            if (CanChangeCoefficientTo(a.coeff, b.coeff, a.mask, ir.bitwidth)) {
                a.coeff                    = b.coeff;
                const uint64_t kMergedMask = (a.mask | b.mask) & modmask;
                a.atom_id                  = CreateMaskedAtom(ir, basis, kMergedMask);
                a.mask                     = kMergedMask;
                b.consumed                 = true;
                return true;
            }

            return false;
        }

        /// Section 5.2 step 6: Three-term collapse.
        /// If m1+m2=m3 with disjoint masks, rewrite 3 terms as 2:
        ///   m1*(c1&x) + m2*(c2&x) + m3*(c3&x)
        ///     → m1*((c1|c3)&x) + m2*((c2|c3)&x)
        bool TryThreeTermCollapse(
            std::vector< RefineTerm > &group, SemilinearIR &ir, const Expr &basis,
            uint64_t modmask
        ) {
            const uint64_t kMod = Bitmask(ir.bitwidth);
            bool changed        = false;

            for (size_t i = 0; i < group.size(); ++i) {
                if (group[i].consumed) { continue; }
                for (size_t j = i + 1; j < group.size(); ++j) {
                    if (group[j].consumed) { continue; }
                    for (size_t k = j + 1; k < group.size(); ++k) {
                        if (group[k].consumed) { continue; }

                        // Try all orderings of (a, b, c) where a.coeff + b.coeff = c.coeff
                        auto try_collapse = [&](size_t ai, size_t bi, size_t ci) -> bool {
                            auto &a = group[ai];
                            auto &b = group[bi];
                            auto &c = group[ci];
                            if (((a.coeff + b.coeff) & kMod) != c.coeff) { return false; }
                            if ((a.mask & c.mask) != 0 || (b.mask & c.mask) != 0) {
                                return false;
                            }
                            // Merge c into a and b
                            const uint64_t kMaskAc = (a.mask | c.mask) & modmask;
                            const uint64_t kMaskBc = (b.mask | c.mask) & modmask;
                            a.atom_id              = CreateMaskedAtom(ir, basis, kMaskAc);
                            a.mask                 = kMaskAc;
                            b.atom_id              = CreateMaskedAtom(ir, basis, kMaskBc);
                            b.mask                 = kMaskBc;
                            c.consumed             = true;
                            return true;
                        };

                        if (try_collapse(i, j, k) || try_collapse(i, k, j)
                            || try_collapse(j, k, i))
                        {
                            changed = true;
                            break; // restart inner loops via outer re-scan
                        }
                    }
                    if (changed) { break; }
                }
                if (changed) { break; }
            }
            return changed;
        }

        /// Refine a single basis group (all terms share the same basis expression).
        void RefineGroup(
            std::vector< RefineTerm > &group, SemilinearIR &ir, const Expr &basis,
            uint64_t modmask
        ) {
            const uint64_t kMod = Bitmask(ir.bitwidth);

            // Step 0: Reduce masks (strip coefficient-killed bits)
            for (auto &t : group) {
                if (t.consumed) { continue; }
                const uint64_t kReduced = ReduceMask(t.coeff, t.mask, ir.bitwidth);
                if (kReduced != t.mask) {
                    t.mask    = kReduced;
                    t.atom_id = CreateMaskedAtom(ir, basis, kReduced);
                }
            }

            // Step 3 (paper 5.2): Discard zero-effective terms
            for (auto &t : group) {
                if (t.consumed) { continue; }
                if (CanChangeCoefficientTo(t.coeff, 0, t.mask, ir.bitwidth)) {
                    t.consumed = true;
                }
            }

            // Step 1: Merge disjoint-mask pairs with same coefficient
            bool merged = true;
            while (merged) {
                merged = false;
                for (size_t i = 0; i < group.size(); ++i) {
                    if (group[i].consumed) { continue; }
                    for (size_t j = i + 1; j < group.size(); ++j) {
                        if (group[j].consumed) { continue; }
                        if (group[i].coeff == group[j].coeff
                            && (group[i].mask & group[j].mask) == 0)
                        {
                            const uint64_t kM = (group[i].mask | group[j].mask) & modmask;
                            group[i].atom_id  = CreateMaskedAtom(ir, basis, kM);
                            group[i].mask     = kM;
                            group[j].consumed = true;
                            merged            = true;
                        }
                    }
                }
            }

            // Step 2: Change coefficients to match, then merge disjoint pairs
            for (size_t i = 0; i < group.size(); ++i) {
                if (group[i].consumed) { continue; }
                for (size_t j = i + 1; j < group.size(); ++j) {
                    if (group[j].consumed) { continue; }
                    TryMergePair(group[i], group[j], ir, basis, modmask);
                }
            }

            // Step 4: Change coefficient to -1 where possible
            for (auto &t : group) {
                if (t.consumed) { continue; }
                const uint64_t kNegOne = kMod; // -1 mod 2^bitwidth
                if (t.coeff != kNegOne
                    && CanChangeCoefficientTo(t.coeff, kNegOne, t.mask, ir.bitwidth))
                {
                    t.coeff = kNegOne;
                }
            }

            // Step 5: Repeat step 2 after coefficient normalization
            for (size_t i = 0; i < group.size(); ++i) {
                if (group[i].consumed) { continue; }
                for (size_t j = i + 1; j < group.size(); ++j) {
                    if (group[j].consumed) { continue; }
                    TryMergePair(group[i], group[j], ir, basis, modmask);
                }
            }

            // Step 6: Three-term collapse
            TryThreeTermCollapse(group, ir, basis, modmask);
        }

    } // namespace

    void RefineTerms(SemilinearIR &ir) {
        COBRA_TRACE("TermRefiner", "RefineTerms: terms={}", ir.terms.size());
        if (ir.terms.empty()) { return; }

        const uint64_t kMod = Bitmask(ir.bitwidth);

        // Decompose atoms and group by basis hash.
        struct GroupEntry
        {
            RefineTerm term;
            size_t original_index;
        };

        std::unordered_map< uint64_t, std::vector< GroupEntry > > basis_groups;
        std::vector< bool > in_group(ir.terms.size(), false);

        // Map from basis_hash to a representative basis expression.
        std::unordered_map< uint64_t, const Expr * > basis_repr;

        for (size_t i = 0; i < ir.terms.size(); ++i) {
            const auto &term = ir.terms[i];
            auto decomp      = DecomposeAtom(ir.atom_table[term.atom_id], kMod);
            if (!decomp.valid) { continue; }

            in_group[i] = true;
            basis_repr.emplace(decomp.basis_hash, decomp.basis);
            basis_groups[decomp.basis_hash].push_back(
                {
                    .term           = { .coeff   = term.coeff,
                                       .mask    = decomp.mask,
                                       .atom_id = term.atom_id },
                    .original_index = i
            }
            );
        }

        bool any_refined = false;
        for (auto &[hash, entries] : basis_groups) {
            const Expr *basis = basis_repr.at(hash);
            std::vector< RefineTerm > group;
            group.reserve(entries.size());
            for (auto &e : entries) { group.push_back(e.term); }

            RefineGroup(group, ir, *basis, kMod);

            // Write back results
            bool group_changed = false;
            for (size_t gi = 0; gi < group.size(); ++gi) {
                const auto &orig = entries[gi].term;
                const auto &ref  = group[gi];
                if (ref.consumed || ref.coeff != orig.coeff || ref.mask != orig.mask
                    || ref.atom_id != orig.atom_id)
                {
                    group_changed = true;
                    break;
                }
            }

            if (group_changed) {
                any_refined = true;
                for (size_t gi = 0; gi < group.size(); ++gi) { entries[gi].term = group[gi]; }
            }
        }

        if (!any_refined) {
            COBRA_TRACE("TermRefiner", "RefineTerms: no changes");
            return;
        }

        // Rebuild ir.terms from refined groups + non-group terms
        std::vector< WeightedAtom > new_terms;

        for (size_t i = 0; i < ir.terms.size(); ++i) {
            if (!in_group[i]) { new_terms.push_back(ir.terms[i]); }
        }

        for (auto &[hash, entries] : basis_groups) {
            for (const auto &e : entries) {
                if (e.term.consumed) { continue; }
                if (e.term.coeff == 0) { continue; }
                new_terms.push_back(
                    { .coeff = e.term.coeff & kMod, .atom_id = e.term.atom_id }
                );
            }
        }

        // Re-merge any duplicate atom_ids
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

        COBRA_TRACE("TermRefiner", "RefineTerms: refined to {} terms", ir.terms.size());
    }

} // namespace cobra
