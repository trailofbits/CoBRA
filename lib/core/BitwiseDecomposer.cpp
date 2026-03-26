#include "cobra/core/BitwiseDecomposer.h"
#include "cobra/core/Expr.h"
#include "cobra/core/SignatureSimplifier.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace cobra {

    // ---------------------------------------------------------------
    // Public helpers for candidate enumeration
    // ---------------------------------------------------------------

    std::unique_ptr< Expr >
    RemapVars(const Expr &expr, const std::vector< uint32_t > &index_map) {
        switch (expr.kind) {
            case Expr::Kind::kConstant:
                return Expr::Constant(expr.constant_val);
            case Expr::Kind::kVariable:
                return Expr::Variable(index_map[expr.var_index]);
            case Expr::Kind::kAdd:
                return Expr::Add(
                    RemapVars(*expr.children[0], index_map),
                    RemapVars(*expr.children[1], index_map)
                );
            case Expr::Kind::kMul:
                return Expr::Mul(
                    RemapVars(*expr.children[0], index_map),
                    RemapVars(*expr.children[1], index_map)
                );
            case Expr::Kind::kAnd:
                return Expr::BitwiseAnd(
                    RemapVars(*expr.children[0], index_map),
                    RemapVars(*expr.children[1], index_map)
                );
            case Expr::Kind::kOr:
                return Expr::BitwiseOr(
                    RemapVars(*expr.children[0], index_map),
                    RemapVars(*expr.children[1], index_map)
                );
            case Expr::Kind::kXor:
                return Expr::BitwiseXor(
                    RemapVars(*expr.children[0], index_map),
                    RemapVars(*expr.children[1], index_map)
                );
            case Expr::Kind::kNot:
                return Expr::BitwiseNot(RemapVars(*expr.children[0], index_map));
            case Expr::Kind::kNeg:
                return Expr::Negate(RemapVars(*expr.children[0], index_map));
            case Expr::Kind::kShr:
                return Expr::LogicalShr(
                    RemapVars(*expr.children[0], index_map), expr.constant_val
                );
        }
        return Expr::Constant(0);
    }

    std::unique_ptr< Expr > Compose(
        GateKind gate, uint32_t original_k, std::unique_ptr< Expr > g_expr, uint64_t add_coeff
    ) {
        auto var_k = Expr::Variable(original_k);
        switch (gate) {
            case GateKind::kAnd:
                return Expr::BitwiseAnd(std::move(var_k), std::move(g_expr));
            case GateKind::kOr:
                return Expr::BitwiseOr(std::move(var_k), std::move(g_expr));
            case GateKind::kXor:
                return Expr::BitwiseXor(std::move(var_k), std::move(g_expr));
            case GateKind::kMul:
                return Expr::Mul(std::move(var_k), std::move(g_expr));
            case GateKind::kAdd: {
                std::unique_ptr< Expr > var_term;
                if (add_coeff == 1) {
                    var_term = std::move(var_k);
                } else {
                    var_term = Expr::Mul(Expr::Constant(add_coeff), std::move(var_k));
                }
                return Expr::Add(std::move(var_term), std::move(g_expr));
            }
        }
        return Expr::Constant(0);
    }

    uint32_t CountActive(const std::vector< uint64_t > &g_sig, uint32_t n_g) {
        uint32_t count = 0;
        for (uint32_t v = 0; v < n_g; ++v) {
            bool active = false;
            for (size_t j = 0; j < g_sig.size(); ++j) {
                const size_t kFlipped = j ^ (1U << v);
                if (g_sig[j] != g_sig[kFlipped]) {
                    active = true;
                    break;
                }
            }
            if (active) { ++count; }
        }
        return count;
    }

    std::pair< std::vector< uint64_t >, std::vector< uint32_t > >
    CompactSignature(const std::vector< uint64_t > &g_sig, uint32_t n_g) {
        std::vector< uint32_t > active_vars;
        for (uint32_t v = 0; v < n_g; ++v) {
            for (size_t j = 0; j < g_sig.size(); ++j) {
                const size_t kFlipped = j ^ (1U << v);
                if (g_sig[j] != g_sig[kFlipped]) {
                    active_vars.push_back(v);
                    break;
                }
            }
        }

        if (active_vars.empty()) { return { { g_sig[0] }, {} }; }

        const auto n_active = static_cast< uint32_t >(active_vars.size());
        std::vector< uint64_t > compacted(1U << n_active);

        for (uint32_t ci = 0; ci < (1U << n_active); ++ci) {
            uint32_t orig_idx = 0;
            for (uint32_t a = 0; a < n_active; ++a) {
                if (((ci >> a) & 1) != 0u) { orig_idx |= (1U << active_vars[a]); }
            }
            compacted[ci] = g_sig[orig_idx];
        }

        return { compacted, active_vars };
    }

    std::vector< BitwiseSplitCandidate >
    EnumerateBitwiseCandidates(const std::vector< uint64_t > &sig, uint32_t num_vars) {
        const size_t kHalf = sig.size() / 2;
        std::vector< BitwiseSplitCandidate > candidates;
        std::vector< uint64_t > cof0;
        std::vector< uint64_t > cof1;
        cof0.reserve(kHalf);
        cof1.reserve(kHalf);

        for (uint32_t k = 0; k < num_vars; ++k) {
            cof0.clear();
            cof1.clear();

            for (size_t j = 0; j < sig.size(); ++j) {
                if (((j >> k) & 1) == 0) {
                    cof0.push_back(sig[j]);
                    cof1.push_back(sig[j | (1U << k)]);
                }
            }

            const bool kAllCof0Zero =
                std::all_of(cof0.begin(), cof0.end(), [](uint64_t v) { return v == 0; });

            if (kAllCof0Zero) {
                const uint32_t kNg = num_vars - 1;
                const uint32_t kAc = CountActive(cof1, kNg);
                if (IsBooleanValued(cof1)) {
                    candidates.push_back(
                        { .var_k        = k,
                          .gate         = GateKind::kAnd,
                          .g_sig        = cof1,
                          .add_coeff    = 0,
                          .active_count = kAc }
                    );
                }
                candidates.push_back(
                    { .var_k        = k,
                      .gate         = GateKind::kMul,
                      .g_sig        = cof1,
                      .add_coeff    = 0,
                      .active_count = kAc }
                );
            }

            bool or_match = true;
            for (size_t j = 0; j < cof0.size(); ++j) {
                if (cof1[j] != (cof0[j] | 1)) {
                    or_match = false;
                    break;
                }
            }

            if (or_match) {
                const uint32_t kNg = num_vars - 1;
                const uint32_t kAc = CountActive(cof0, kNg);
                candidates.push_back(
                    { .var_k        = k,
                      .gate         = GateKind::kOr,
                      .g_sig        = cof0,
                      .add_coeff    = 0,
                      .active_count = kAc }
                );
            }

            bool xor_match = true;
            for (size_t j = 0; j < cof0.size(); ++j) {
                if (cof1[j] != (cof0[j] ^ 1)) {
                    xor_match = false;
                    break;
                }
            }

            if (xor_match) {
                const uint32_t kNg = num_vars - 1;
                const uint32_t kAc = CountActive(cof0, kNg);
                candidates.push_back(
                    { .var_k        = k,
                      .gate         = GateKind::kXor,
                      .g_sig        = cof0,
                      .add_coeff    = 0,
                      .active_count = kAc }
                );
            }

            if (!kAllCof0Zero && !cof0.empty()) {
                const uint64_t kDiff = cof1[0] - cof0[0];
                if (kDiff != 0) {
                    bool add_match = true;
                    for (size_t j = 1; j < cof0.size(); ++j) {
                        if ((cof1[j] - cof0[j]) != kDiff) {
                            add_match = false;
                            break;
                        }
                    }
                    if (add_match) {
                        const uint32_t kNg = num_vars - 1;
                        const uint32_t kAc = CountActive(cof0, kNg);
                        candidates.push_back(
                            { .var_k        = k,
                              .gate         = GateKind::kAdd,
                              .g_sig        = cof0,
                              .add_coeff    = kDiff,
                              .active_count = kAc }
                        );
                    }
                }
            }
        }

        std::sort(
            candidates.begin(), candidates.end(),
            [](const BitwiseSplitCandidate &a, const BitwiseSplitCandidate &b) {
                return a.active_count < b.active_count;
            }
        );

        return candidates;
    }

} // namespace cobra
