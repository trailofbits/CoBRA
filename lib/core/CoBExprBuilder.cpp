#include "cobra/core/CoBExprBuilder.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/Trace.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cobra {

    namespace {

        struct Term
        {
            uint64_t mask;
            uint64_t coeff;
            std::unique_ptr< Expr > expr; // set for compound (OR/XOR) terms
            bool consumed = false;
        };

        // Build sorted indices by (popcount, mask value) for greedy pairing.
        std::vector< size_t > SortedByPopcount(const std::vector< Term > &terms) {
            std::vector< size_t > indices;
            indices.reserve(terms.size());
            for (size_t i = 0; i < terms.size(); ++i) {
                if (terms[i].mask != 0) { // skip constant term
                    indices.push_back(i);
                }
            }
            std::sort(indices.begin(), indices.end(), [&terms](size_t a, size_t b) {
                const uint32_t pa = std::popcount(terms[a].mask);
                const uint32_t pb = std::popcount(terms[b].mask);
                if (pa != pb) { return pa < pb; }
                return terms[a].mask < terms[b].mask;
            });
            return indices;
        }

        // NOLINTNEXTLINE(readability-identifier-naming)
        void GreedyRewrite(
            std::vector< Term > &terms, std::unordered_map< uint64_t, size_t > &mask_index,
            uint32_t bitwidth
        ) {
            auto sorted = SortedByPopcount(terms);

            for (size_t si = 0; si < sorted.size(); ++si) {
                const size_t i = sorted[si];
                if (terms[i].consumed) { continue; }

                for (size_t sj = si + 1; sj < sorted.size(); ++sj) {
                    const size_t j = sorted[sj];
                    if (terms[j].consumed) { continue; }
                    if (terms[i].consumed) { break; }

                    const uint64_t c1 = terms[i].coeff;
                    const uint64_t c2 = terms[j].coeff;
                    if (c1 != c2) { continue; }

                    const uint64_t m1      = terms[i].mask;
                    const uint64_t m2      = terms[j].mask;
                    const uint64_t m_union = m1 | m2;
                    if (m_union == m1 || m_union == m2) { continue; }

                    uint64_t c_union = 0;
                    size_t union_idx = terms.size(); // sentinel
                    auto it          = mask_index.find(m_union);
                    if (it != mask_index.end() && !terms[it->second].consumed) {
                        union_idx = it->second;
                        c_union   = terms[union_idx].coeff;
                    }

                    const uint64_t c     = c1;
                    const uint64_t neg_c = ModNeg(c, bitwidth);

                    auto build_operand = [&](Term &t) -> std::unique_ptr< Expr > {
                        if (t.expr) { return std::move(t.expr); }
                        return BuildAndProduct(t.mask);
                    };

                    if (c_union == neg_c) {
                        // OR match: C*T(m1) + C*T(m2) - C*T(m1|m2) = C*(T(m1)|T(m2))
                        auto lhs     = build_operand(terms[i]);
                        auto rhs     = build_operand(terms[j]);
                        auto or_expr = Expr::BitwiseOr(std::move(lhs), std::move(rhs));

                        COBRA_TRACE(
                            "CoBBuilder",
                            "GreedyRewrite: OR match masks=0x{:x},0x{:x} coeff={}", m1, m2, c
                        );

                        terms[i].consumed = true;
                        terms[j].consumed = true;
                        if (union_idx < terms.size()) { terms[union_idx].consumed = true; }

                        terms.push_back(
                            { .mask     = m_union,
                              .coeff    = c,
                              .expr     = std::move(or_expr),
                              .consumed = false }
                        );
                        break; // compound terms don't re-enter pairing
                    }

                    const uint64_t neg_2_c = ModNeg(ModMul(2, c, bitwidth), bitwidth);
                    if (c_union == neg_2_c) {
                        // XOR match: C*T(m1) + C*T(m2) - 2C*T(m1|m2) = C*(T(m1)^T(m2))
                        auto lhs      = build_operand(terms[i]);
                        auto rhs      = build_operand(terms[j]);
                        auto xor_expr = Expr::BitwiseXor(std::move(lhs), std::move(rhs));

                        COBRA_TRACE(
                            "CoBBuilder",
                            "GreedyRewrite: XOR match masks=0x{:x},0x{:x} coeff={}", m1, m2, c
                        );

                        terms[i].consumed = true;
                        terms[j].consumed = true;
                        if (union_idx < terms.size()) { terms[union_idx].consumed = true; }

                        terms.push_back(
                            { .mask     = m_union,
                              .coeff    = c,
                              .expr     = std::move(xor_expr),
                              .consumed = false }
                        );
                        break; // compound terms don't re-enter pairing
                    }
                }
            }
        }

        bool TryNotRecognition(
            std::vector< Term > &terms, uint64_t const_coeff, uint32_t bitwidth
        ) { // NOLINT(readability-identifier-naming)
            const uint64_t neg1 = Bitmask(bitwidth);
            if (const_coeff != neg1) { return false; }

            // Find exactly one unconsumed non-constant term with coeff == -1
            size_t match = terms.size();
            for (size_t i = 0; i < terms.size(); ++i) {
                if (terms[i].consumed || terms[i].mask == 0) { continue; }
                if (terms[i].coeff != neg1) { return false; }
                if (match < terms.size()) {
                    return false; // more than one
                }
                match = i;
            }
            if (match >= terms.size()) { return false; }

            auto operand          = terms[match].expr ? std::move(terms[match].expr)
                                                      : BuildAndProduct(terms[match].mask);
            terms[match].consumed = true;
            terms[match].expr     = Expr::BitwiseNot(std::move(operand));
            terms[match].coeff    = 1;
            terms[match].consumed = false;
            COBRA_TRACE(
                "CoBBuilder", "NotRecognition: match at mask=0x{:x}", terms[match].mask
            );
            return true;
        }

    } // namespace

    std::unique_ptr< Expr > BuildCobExpr(
        const std::vector< uint64_t > &coeffs, uint32_t /*num_vars*/, uint32_t bitwidth
    ) {
        // Step 1: Collect non-zero terms
        std::vector< Term > terms;
        std::unordered_map< uint64_t, size_t > mask_index;
        uint64_t const_coeff = coeffs[0];
        COBRA_TRACE(
            "CoBBuilder", "BuildCobExpr: coeff_count={} const_coeff={} bitwidth={}",
            coeffs.size(), const_coeff, bitwidth
        );

        for (size_t i = 1; i < coeffs.size(); ++i) {
            if (coeffs[i] == 0) { continue; }
            const size_t kIdx = terms.size();
            terms.push_back(
                { .mask     = static_cast< uint64_t >(i),
                  .coeff    = coeffs[i],
                  .expr     = nullptr,
                  .consumed = false }
            );
            mask_index[static_cast< uint64_t >(i)] = kIdx;
        }

        // Step 2: Greedy pairwise rewriting
        GreedyRewrite(terms, mask_index, bitwidth);

        // Step 3: NOT recognition
        const bool kUsedNot = TryNotRecognition(terms, const_coeff, bitwidth);
        if (kUsedNot) {
            const_coeff = 0; // consumed into NOT
        }

        // Step 4: Build final expression tree
        std::unique_ptr< Expr > result;

        if (const_coeff != 0) { result = Expr::Constant(const_coeff); }

        for (auto &t : terms) {
            if (t.consumed) { continue; }

            std::unique_ptr< Expr > term_expr;
            if (t.expr) {
                term_expr = std::move(t.expr);
            } else {
                term_expr = BuildAndProduct(t.mask);
            }
            term_expr = ApplyCoefficient(std::move(term_expr), t.coeff, bitwidth);

            if (result) {
                result = Expr::Add(std::move(result), std::move(term_expr));
            } else {
                result = std::move(term_expr);
            }
        }

        if (!result) { result = Expr::Constant(0); }
        COBRA_TRACE("CoBBuilder", "BuildCobExpr: done");
        return result;
    }

} // namespace cobra
