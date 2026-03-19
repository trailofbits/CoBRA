#include "cobra/core/AnfCleanup.h"
#include "cobra/core/Expr.h"
#include "cobra/core/PackedAnf.h"
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cobra {

    namespace {

        bool MonomialLess(uint32_t a, uint32_t b) {
            const int kDa = std::popcount(a);
            const int kDb = std::popcount(b);
            if (kDa != kDb) { return kDa < kDb; }
            return a < b;
        }

        // Cost of a monomial AND-tree (e.g., a&b&c) without building Expr.
        // Equal to ExprCost(build_monomial(mask)).
        uint32_t MonomialExprCost(uint32_t mask) {
            const int kPc = std::popcount(mask);
            return static_cast< uint32_t >((2 * kPc) - 1);
        }

        // Cost of a raw ANF XOR-tree without building Expr.
        // Equal to ExprCost(EmitRawAnf(form)).
        uint32_t RawAnfCost(const std::vector< uint32_t > &monomials, uint8_t constant_bit) {
            const uint32_t kN = static_cast< uint32_t >(monomials.size()) + constant_bit;
            if (kN == 0) { return 1; }
            uint32_t leaf_cost = constant_bit;
            for (const uint32_t m : monomials) { leaf_cost += MonomialExprCost(m); }
            return leaf_cost + (kN > 1 ? kN - 1 : 0);
        }

        // Cost of build_or_chain(var_mask) without building Expr.
        // Accounts for the OR-chain depth discount in expr_cost.
        uint32_t OrChainCost(uint32_t var_mask) {
            const int kK = std::popcount(var_mask);
            if (kK <= 0) { return 1; }
            if (kK == 1) { return 1; }
            if (kK == 2) { return 3; }
            return static_cast< uint32_t >(kK + 1);
        }

        std::unique_ptr< Expr > BuildMonomial(uint32_t mask) {
            std::vector< uint32_t > vars;
            uint32_t m = mask;
            while (m != 0) {
                vars.push_back(static_cast< uint32_t >(std::countr_zero(m)));
                m &= m - 1;
            }
            if (vars.size() == 1) { return Expr::Variable(vars[0]); }
            auto result = Expr::Variable(vars[0]);
            for (size_t i = 1; i < vars.size(); ++i) {
                result = Expr::BitwiseAnd(std::move(result), Expr::Variable(vars[i]));
            }
            return result;
        }

        std::unique_ptr< Expr > XorTree(
            std::vector< std::unique_ptr< Expr > > &terms, size_t lo, size_t hi
        ) { // NOLINT(readability-identifier-naming)
            if (hi - lo == 1) { return std::move(terms[lo]); }
            const size_t mid = lo + ((hi - lo) / 2);
            auto left        = XorTree(terms, lo, mid);
            auto right       = XorTree(terms, mid, hi);
            return Expr::BitwiseXor(std::move(left), std::move(right));
        }

        uint32_t CountOrChainDepth(const Expr &expr) {
            if (expr.kind != Expr::Kind::kOr) { return 0; }
            return 1 + CountOrChainDepth(*expr.children[0]);
        }

        std::optional< uint32_t > DetectOrFamily(const std::vector< uint32_t > &monomials) {
            if (monomials.empty()) { return std::nullopt; }

            uint32_t var_union = 0;
            for (const uint32_t m : monomials) {
                if (std::has_single_bit(m)) { var_union |= m; }
            }
            if (var_union == 0) { return std::nullopt; }

            const int kNv          = std::popcount(var_union);
            const size_t kExpected = (1ULL << kNv) - 1;
            if (monomials.size() != kExpected) { return std::nullopt; }

            std::vector< uint32_t > bits;
            uint32_t tmp = var_union;
            while (tmp != 0u) {
                bits.push_back(tmp & (~tmp + 1));
                tmp &= tmp - 1;
            }

            // Use unordered_set — monomials are sorted by monomial_less,
            // NOT numeric order, so binary_search would be UB
            std::unordered_set< uint32_t > mono_set(monomials.begin(), monomials.end());

            const size_t kTotal = 1ULL << bits.size();
            for (size_t s = 1; s < kTotal; ++s) {
                uint32_t mask = 0;
                for (size_t b = 0; b < bits.size(); ++b) {
                    if ((s & (1ULL << b)) != 0u) { mask |= bits[b]; }
                }
                if (!mono_set.contains(mask)) { return std::nullopt; }
            }
            return var_union;
        }

        std::unique_ptr< Expr > BuildOrChain(uint32_t var_mask) {
            std::vector< uint32_t > vars;
            uint32_t m = var_mask;
            while (m != 0u) {
                vars.push_back(static_cast< uint32_t >(std::countr_zero(m)));
                m &= m - 1;
            }
            auto result = Expr::Variable(vars[0]);
            for (size_t i = 1; i < vars.size(); ++i) {
                result = Expr::BitwiseOr(std::move(result), Expr::Variable(vars[i]));
            }
            return result;
        }

        struct FactorCandidate
        {
            uint32_t factor_mask;
            std::vector< uint32_t > covered_indices;
        };

        std::optional< FactorCandidate > FindBestFactor(const AnfForm &form) {
            if (form.monomials.size() < 2) { return std::nullopt; }

            // Collect candidate factors: single vars + var-pairs
            std::vector< uint32_t > candidates;
            uint32_t all_vars = 0;
            for (const uint32_t m : form.monomials) { all_vars |= m; }

            // Single variables
            uint32_t tmp = all_vars;
            while (tmp != 0u) {
                candidates.push_back(tmp & (~tmp + 1));
                tmp &= tmp - 1;
            }

            // Variable pairs (intersections of monomial pairs)
            for (size_t i = 0; i < form.monomials.size(); ++i) {
                for (size_t j = i + 1; j < form.monomials.size(); ++j) {
                    const uint32_t kCommon = form.monomials[i] & form.monomials[j];
                    if (kCommon != 0 && std::popcount(kCommon) >= 2) {
                        candidates.push_back(kCommon);
                    }
                }
            }

            std::sort(candidates.begin(), candidates.end());
            candidates.erase(
                std::unique(candidates.begin(), candidates.end()), candidates.end()
            );

            std::optional< FactorCandidate > best;
            uint32_t best_saving = 0;

            for (const uint32_t cand : candidates) {
                std::vector< uint32_t > covered;
                for (size_t i = 0; i < form.monomials.size(); ++i) {
                    if ((form.monomials[i] & cand) == cand) {
                        covered.push_back(static_cast< uint32_t >(i));
                    }
                }
                if (covered.size() < 2) { continue; }

                // Build inner AnfForm (strip factor from covered)
                AnfForm inner;
                inner.constant_bit = 0;
                inner.num_vars     = form.num_vars;
                for (const uint32_t idx : covered) {
                    const uint32_t stripped = form.monomials[idx] & ~cand;
                    if (stripped == 0) {
                        inner.constant_bit = 1;
                    } else {
                        inner.monomials.push_back(stripped);
                    }
                }
                std::sort(inner.monomials.begin(), inner.monomials.end(), MonomialLess);

                // Compute raw cost of covered terms without building
                // Expr trees — pure arithmetic on bitmasks.
                std::vector< uint32_t > covered_monos;
                covered_monos.reserve(covered.size());
                for (const uint32_t idx : covered) {
                    covered_monos.push_back(form.monomials[idx]);
                }
                const uint32_t kRawCost = RawAnfCost(covered_monos, 0);

                // Quick upper bound: factored_cost ≥ 1 + monomial_cost.
                // If even the cheapest possible factored form can't beat
                // raw, skip the recursive cleanup_anf call entirely.
                const uint32_t factor_cost = MonomialExprCost(cand);
                if (1 + factor_cost >= kRawCost) { continue; }

                auto inner_expr              = CleanupAnf(inner);
                const uint32_t factored_cost = 1 + factor_cost + ExprCost(*inner_expr);

                if (factored_cost < kRawCost) {
                    const uint32_t kSaving = kRawCost - factored_cost;
                    if (kSaving > best_saving) {
                        best_saving = kSaving;
                        best =
                            FactorCandidate{ .factor_mask = cand, .covered_indices = covered };
                    }
                }
            }

            return best;
        }

        struct PartialOrCandidate
        {
            uint32_t var_mask;
            std::vector< uint32_t > family_masks;
        };

        std::optional< PartialOrCandidate >
        FindPartialOr(const std::vector< uint32_t > &monomials) {
            if (monomials.size() < 3) { return std::nullopt; }

            std::unordered_set< uint32_t > mono_set(monomials.begin(), monomials.end());

            // Collect individual variable bits
            uint32_t all_vars = 0;
            for (const uint32_t m : monomials) { all_vars |= m; }

            std::vector< uint32_t > var_bits;
            uint32_t tmp = all_vars;
            while (tmp != 0u) {
                var_bits.push_back(tmp & (~tmp + 1));
                tmp &= tmp - 1;
            }

            const int kNv = static_cast< int >(var_bits.size());
            if (kNv < 2) { return std::nullopt; }

            std::optional< PartialOrCandidate > best;
            uint32_t best_saving = 0;

            const uint32_t kLimit = 1U << kNv;
            for (uint32_t s = 3; s < kLimit; ++s) {
                const int kSubsetSize = std::popcount(s);
                if (kSubsetSize < 2) { continue; }

                uint32_t var_mask = 0;
                for (int b = 0; b < kNv; ++b) {
                    if ((s & (1U << b)) != 0u) { var_mask |= var_bits[b]; }
                }

                const uint32_t kFamilySize = (1ULL << kSubsetSize) - 1;
                // Skip if this covers all monomials (Rule 1 handles)
                if (kFamilySize >= monomials.size()) { continue; }

                // Check all nonempty submasks of var_mask exist
                bool all_present = true;
                std::vector< uint32_t > family_masks;
                family_masks.reserve(kFamilySize);

                for (uint32_t sub = var_mask; sub > 0; sub = (sub - 1) & var_mask) {
                    if (!mono_set.contains(sub)) {
                        all_present = false;
                        break;
                    }
                    family_masks.push_back(sub);
                }

                if (!all_present) { continue; }

                // Compare OR cost vs raw cost of family
                const uint32_t kOc      = OrChainCost(var_mask);
                const uint32_t kRawCost = RawAnfCost(family_masks, 0);

                if (kOc < kRawCost) {
                    const uint32_t kSaving = kRawCost - kOc;
                    if (kSaving > best_saving) {
                        best_saving = kSaving;
                        best = PartialOrCandidate{ .var_mask     = var_mask,
                                                   .family_masks = std::move(family_masks) };
                    }
                }
            }

            return best;
        }

        struct AbsorptionCandidate
        {
            size_t m_idx;    // index of M in monomials
            size_t mn_idx;   // index of M&N in monomials
            uint32_t n_mask; // the extra bits (N = MN & ~M)
        };

        std::optional< AbsorptionCandidate > FindAbsorption(const AnfForm &form) {
            if (form.monomials.size() != 2) { return std::nullopt; }

            const uint32_t kA = form.monomials[0];
            const uint32_t kB = form.monomials[1];

            const uint32_t kRawCost = RawAnfCost(form.monomials, form.constant_bit);

            // Check if a is subset of b (a is M, b is M&N)
            if ((kA & kB) == kA && kA != kB) {
                const uint32_t kN = kB & ~kA;
                // M & ~N cost: 1 (AND) + monomial(M) + 1 (NOT) + monomial(N)
                const uint32_t kAbsorptionCost =
                    2 + MonomialExprCost(kA) + MonomialExprCost(kN);

                if (kAbsorptionCost < kRawCost) {
                    return AbsorptionCandidate{ .m_idx = 0, .mn_idx = 1, .n_mask = kN };
                }
            }

            // Check if b is subset of a (b is M, a is M&N)
            if ((kB & kA) == kB && kB != kA) {
                const uint32_t kN = kA & ~kB;
                const uint32_t kAbsorptionCost =
                    2 + MonomialExprCost(kB) + MonomialExprCost(kN);

                if (kAbsorptionCost < kRawCost) {
                    return AbsorptionCandidate{ .m_idx = 1, .mn_idx = 0, .n_mask = kN };
                }
            }

            return std::nullopt;
        }

    } // namespace

    AnfForm AnfForm::FromAnfCoeffs(const PackedAnf &anf, uint32_t num_vars) {
        AnfForm form;
        form.num_vars     = num_vars;
        form.constant_bit = (!anf.Empty() && (anf[0] != 0u))
            ? 1
            : 0; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        for (size_t w = 0; w < anf.WordCount(); ++w) {
            uint64_t bits = anf.Word(w);
            if (w == 0) {
                bits &= ~1ULL; // skip constant bit
            }
            while (bits != 0u) {
                const auto kBit = static_cast< uint32_t >(std::countr_zero(bits));
                form.monomials.push_back(static_cast< uint32_t >((w * 64) + kBit));
                bits &= bits - 1; // clear lowest set bit
            }
        }
        std::sort(form.monomials.begin(), form.monomials.end(), MonomialLess);
        return form;
    }

    std::unique_ptr< Expr > EmitRawAnf(const AnfForm &form) {
        std::vector< std::unique_ptr< Expr > > terms;
        if (form.constant_bit != 0u) { terms.push_back(Expr::Constant(1)); }
        for (const uint32_t mask : form.monomials) { terms.push_back(BuildMonomial(mask)); }
        if (terms.empty()) { return Expr::Constant(0); }
        if (terms.size() == 1) { return std::move(terms[0]); }
        return XorTree(terms, 0, terms.size());
    }

    uint32_t ExprCost(const Expr &expr) {
        switch (expr.kind) {
            case Expr::Kind::kConstant:
            case Expr::Kind::kVariable:
                return 1;
            case Expr::Kind::kNot:
            case Expr::Kind::kNeg:
                return 1 + ExprCost(*expr.children[0]);
            case Expr::Kind::kShr:
            case Expr::Kind::kAdd:
            case Expr::Kind::kMul:
            case Expr::Kind::kAnd:
            case Expr::Kind::kXor:
            case Expr::Kind::kOr: {
                uint32_t base = 1 + ExprCost(*expr.children[0]) + ExprCost(*expr.children[1]);
                if (expr.kind == Expr::Kind::kOr) {
                    const uint32_t kDepth = CountOrChainDepth(expr);
                    if (kDepth >= 2) { base -= 1; }
                }
                return base;
            }
        }
        return 1;
    }

    std::unique_ptr< Expr > CleanupAnf(const AnfForm &form) {
        if (form.monomials.empty()) { return Expr::Constant(form.constant_bit); }

        // Rule 1: Full OR recognizer (exact match)
        auto or_var = DetectOrFamily(form.monomials);
        if (or_var.has_value()) {
            auto or_expr = BuildOrChain(or_var.value());
            if (form.constant_bit != 0u) {
                // Use XOR-with-1 instead of NOT so the result is
                // correct for 0/1 inputs at any bitwidth.
                return Expr::BitwiseXor(Expr::Constant(1), std::move(or_expr));
            }
            return or_expr;
        }

        auto raw                = EmitRawAnf(form);
        const uint32_t kRawCost = ExprCost(*raw);
        std::unique_ptr< Expr > best;
        uint32_t best_cost = kRawCost;

        // Rule 2: Partial OR recognition
        auto partial = FindPartialOr(form.monomials);
        if (partial.has_value()) {
            auto or_expr = BuildOrChain(partial->var_mask);

            std::unordered_set< uint32_t > family_set(
                partial->family_masks.begin(), partial->family_masks.end()
            );

            AnfForm remainder; // NOLINT(misc-const-correctness)
            remainder.constant_bit = form.constant_bit;
            remainder.num_vars     = form.num_vars;
            for (const uint32_t m : form.monomials) {
                if (!family_set.contains(m)) { remainder.monomials.push_back(m); }
            }

            if (remainder.monomials.empty() && remainder.constant_bit == 0) { return or_expr; }

            auto rem_expr        = CleanupAnf(remainder);
            auto result          = Expr::BitwiseXor(std::move(or_expr), std::move(rem_expr));
            const uint32_t kCost = ExprCost(*result);
            if (kCost < best_cost) {
                best_cost = kCost;
                best      = std::move(result);
            }
        }

        // Rule 3: Common-cube factoring
        auto factor = FindBestFactor(form);
        if (factor.has_value()) {
            std::vector< bool > used(form.monomials.size(), false);
            for (const uint32_t idx : factor->covered_indices) { used[idx] = true; }

            AnfForm inner; // NOLINT(misc-const-correctness)
            inner.constant_bit = 0;
            inner.num_vars     = form.num_vars;
            for (const uint32_t idx : factor->covered_indices) {
                const uint32_t stripped = form.monomials[idx] & ~factor->factor_mask;
                if (stripped == 0) {
                    inner.constant_bit = 1;
                } else {
                    inner.monomials.push_back(stripped);
                }
            }
            std::sort(inner.monomials.begin(), inner.monomials.end(), MonomialLess);

            auto inner_expr  = CleanupAnf(inner);
            auto factor_expr = BuildMonomial(factor->factor_mask);
            auto factored    = Expr::BitwiseAnd(std::move(factor_expr), std::move(inner_expr));

            AnfForm remainder; // NOLINT(misc-const-correctness)
            remainder.constant_bit = form.constant_bit;
            remainder.num_vars     = form.num_vars;
            for (size_t i = 0; i < form.monomials.size(); ++i) {
                if (!used[i]) { remainder.monomials.push_back(form.monomials[i]); }
            }

            std::unique_ptr< Expr > result;
            if (remainder.monomials.empty() && remainder.constant_bit == 0) {
                result = std::move(factored);
            } else {
                auto rem_expr = CleanupAnf(remainder);
                result        = Expr::BitwiseXor(std::move(factored), std::move(rem_expr));
            }

            const uint32_t kCost = ExprCost(*result);
            if (kCost < best_cost) {
                best_cost = kCost;
                best      = std::move(result);
            }
        }

        // Rule 4: Absorption M ^ M&N → M & ~N (2-monomial only)
        auto absorb = FindAbsorption(form);
        if (absorb.has_value()) {
            auto m_expr = BuildMonomial(form.monomials[absorb->m_idx]);
            auto not_n  = Expr::BitwiseNot(BuildMonomial(absorb->n_mask));
            auto result = Expr::BitwiseAnd(std::move(m_expr), std::move(not_n));
            if (form.constant_bit != 0u) {
                result = Expr::BitwiseXor(Expr::Constant(1), std::move(result));
            }
            const uint32_t kCost = ExprCost(*result);
            if (kCost < best_cost) {
                best_cost = kCost;
                best      = std::move(result);
            }
        }

        return best ? std::move(best) : std::move(raw);
    }

} // namespace cobra
