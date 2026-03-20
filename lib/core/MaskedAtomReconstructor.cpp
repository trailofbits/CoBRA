#include "cobra/core/MaskedAtomReconstructor.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SemilinearIR.h"
#include "cobra/core/Trace.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace cobra {

    namespace {

        /// Compute the "active mask" for an atom: the OR of all partition
        /// class masks where the atom has a non-zero semantic ID.
        uint64_t
        ComputeActiveMask(AtomId atom_id, const std::vector< PartitionClass > &partitions) {
            uint64_t mask = 0;
            for (const auto &pc : partitions) {
                if (atom_id < pc.profile.size() && pc.profile[atom_id] != 0) {
                    mask |= pc.mask;
                }
            }
            return mask;
        }

    } // namespace

    std::unique_ptr< Expr > ReconstructMaskedAtoms(
        const SemilinearIR &ir, const std::vector< PartitionClass > &partitions
    ) {
        COBRA_TRACE(
            "AtomRecon", "ReconstructMaskedAtoms: terms={} partitions={}", ir.terms.size(),
            partitions.size()
        );
        if (ir.terms.empty()) { return Expr::Constant(ir.constant); }

        // Build per-term expressions with coefficients applied.
        struct TermEntry
        {
            std::unique_ptr< Expr > expr;
            uint64_t coeff;
            AtomId atom_id;
        };

        std::vector< TermEntry > entries;
        entries.reserve(ir.terms.size());
        for (const auto &term : ir.terms) {
            auto atom_clone = CloneExpr(*ir.atom_table[term.atom_id].original_subtree);
            auto applied    = ApplyCoefficient(std::move(atom_clone), term.coeff, ir.bitwidth);
            entries.push_back(
                { .expr = std::move(applied), .coeff = term.coeff, .atom_id = term.atom_id }
            );
        }

        // Non-overlapping OR rewrite: find disjoint pairs among
        // bare-coefficient (coeff==1) terms.
        std::vector< bool > consumed(entries.size(), false);
        std::vector< std::unique_ptr< Expr > > combined;

        if (!partitions.empty()) {
            for (size_t i = 0; i < entries.size(); ++i) {
                if (consumed[i] || entries[i].coeff != 1) { continue; }
                const uint64_t kMaskI = ComputeActiveMask(entries[i].atom_id, partitions);

                for (size_t j = i + 1; j < entries.size(); ++j) {
                    if (consumed[j] || entries[j].coeff != 1) { continue; }
                    const uint64_t kMaskJ = ComputeActiveMask(entries[j].atom_id, partitions);

                    if ((kMaskI & kMaskJ) == 0) {
                        auto or_expr = Expr::BitwiseOr(
                            std::move(entries[i].expr), std::move(entries[j].expr)
                        );
                        consumed[i] = true;
                        consumed[j] = true;
                        combined.push_back(std::move(or_expr));
                        break;
                    }
                }
            }
        }

        // Collect OR-combined pairs first, then remaining terms.
        std::vector< std::unique_ptr< Expr > > all_terms;
        all_terms.reserve(combined.size());
        for (auto &c : combined) { all_terms.push_back(std::move(c)); }
        for (size_t i = 0; i < entries.size(); ++i) {
            if (!consumed[i]) { all_terms.push_back(std::move(entries[i].expr)); }
        }

        // Assemble with Add, left-folding.
        std::unique_ptr< Expr > result = std::move(all_terms[0]);
        for (size_t i = 1; i < all_terms.size(); ++i) {
            result = Expr::Add(std::move(result), std::move(all_terms[i]));
        }

        // Prepend constant if non-zero.
        if (ir.constant != 0) {
            result = Expr::Add(Expr::Constant(ir.constant), std::move(result));
        }

        return result;
    }

} // namespace cobra
