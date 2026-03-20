#include "cobra/core/BitwiseDecomposer.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureSimplifier.h"
#include "cobra/core/Simplifier.h"
#include "cobra/core/Trace.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cobra {

    namespace {

        enum class GateKind { kAnd, kOr, kXor, kMul, kAdd };

        struct Candidate
        {
            uint32_t var_k;
            GateKind gate;
            std::vector< uint64_t > g_sig;
            uint64_t add_coeff = 0; // only used for GateKind::kAdd
            uint32_t active_count;
        };

        // Remap variable indices in an expression tree using the
        // provided index map (compacted index -> original index).
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

        // Build composed expression: gate(v_k, g_expr_remapped)
        std::unique_ptr< Expr > Compose(
            GateKind gate, uint32_t original_k, std::unique_ptr< Expr > g_expr,
            uint64_t add_coeff = 0
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

        // Count active variables in g_sig (variables whose flipping
        // changes at least one signature entry). n_g is the number of
        // variables in g_sig (all variables except k).
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

        // Build compacted signature and variable mapping for active
        // variables only. Returns {compacted_sig, active_var_indices}
        // where active_var_indices[i] is the index in the n_g variable
        // space (all vars except k).
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

            if (active_vars.empty()) {
                // g is a constant — return single-entry sig
                return { { g_sig[0] }, {} };
            }

            const auto n_active = static_cast< uint32_t >(active_vars.size());
            std::vector< uint64_t > compacted(1U << n_active);

            for (uint32_t ci = 0; ci < (1U << n_active); ++ci) {
                // Map compacted index to original g_sig index
                uint32_t orig_idx = 0;
                for (uint32_t a = 0; a < n_active; ++a) {
                    if (((ci >> a) & 1) != 0u) { orig_idx |= (1U << active_vars[a]); }
                }
                compacted[ci] = g_sig[orig_idx];
            }

            return { compacted, active_vars };
        }

    } // namespace

    std::optional< SubResult > TryBitwiseDecomposition(
        const std::vector< uint64_t > &sig, const SignatureContext &ctx, const Options &opts,
        uint32_t depth, const ExprCost *baseline_cost
    ) {
        COBRA_TRACE(
            "BitwiseDecomp", "TryBitwiseDecomposition: vars={} depth={}", ctx.vars.size(), depth
        );
        // Early returns
        if (!ctx.eval) { return std::nullopt; }
        if (depth >= 2) { return std::nullopt; }
        if (sig.size() < 2) { return std::nullopt; }

        const auto kN      = static_cast< uint32_t >(ctx.vars.size());
        const size_t kHalf = sig.size() / 2;

        // When the entire signature is boolean-valued, the decomposed
        // Collect candidates across all variables and gate types
        std::vector< Candidate > candidates;

        for (uint32_t k = 0; k < kN; ++k) {
            // Extract cofactors
            std::vector< uint64_t > cof0;
            std::vector< uint64_t > cof1;
            cof0.reserve(kHalf);
            cof1.reserve(kHalf);

            for (size_t j = 0; j < sig.size(); ++j) {
                if (((j >> k) & 1) == 0) {
                    cof0.push_back(sig[j]);
                    cof1.push_back(sig[j | (1U << k)]);
                }
            }

            // AND/MUL check: all of cofactor0 are 0
            // Always try MUL when cof0 is all-zero. For boolean cof1,
            // also try AND. Full-width check disambiguates (AND and MUL
            // agree on {0,1} but differ at full width).
            const bool kAllCof0Zero =
                std::all_of(cof0.begin(), cof0.end(), [](uint64_t v) { return v == 0; });

            if (kAllCof0Zero) {
                const uint32_t kNg = kN - 1;
                const uint32_t kAc = CountActive(cof1, kNg);
                if (IsBooleanValued(cof1)) {
                    candidates.push_back(
                        { .var_k = k, .gate = GateKind::kAnd, .g_sig = cof1, .add_coeff = kAc }
                    );
                }
                candidates.push_back(
                    { .var_k = k, .gate = GateKind::kMul, .g_sig = cof1, .add_coeff = kAc }
                );
            }

            // OR check: cofactor1[j] == (cofactor0[j] | 1) for all j
            // Valid for both boolean and word-valued cofactors.
            bool or_match = true;
            for (size_t j = 0; j < cof0.size(); ++j) {
                if (cof1[j] != (cof0[j] | 1)) {
                    or_match = false;
                    break;
                }
            }

            if (or_match) {
                const uint32_t kNg = kN - 1;
                const uint32_t kAc = CountActive(cof0, kNg);
                candidates.push_back(
                    { .var_k = k, .gate = GateKind::kOr, .g_sig = cof0, .add_coeff = kAc }
                );
            }

            // XOR check: cofactor1[j] == (cofactor0[j] ^ 1) for all j
            // Valid for both boolean and word-valued cofactors.
            bool xor_match = true;
            for (size_t j = 0; j < cof0.size(); ++j) {
                if (cof1[j] != (cof0[j] ^ 1)) {
                    xor_match = false;
                    break;
                }
            }

            if (xor_match) {
                const uint32_t kNg = kN - 1;
                const uint32_t kAc = CountActive(cof0, kNg);
                candidates.push_back(
                    { .var_k = k, .gate = GateKind::kXor, .g_sig = cof0, .add_coeff = kAc }
                );
            }

            // ADD check: cof1[j] - cof0[j] is a constant c for all j.
            // Decomposition: f = c * v_k + g(rest), g = cof0.
            // Skip when c==0 (variable is irrelevant) or when all_cof0_zero
            // (already covered by AND/MUL above).
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
                        const uint32_t kNg = kN - 1;
                        const uint32_t kAc = CountActive(cof0, kNg);
                        candidates.push_back(
                            { .var_k        = k,
                              .gate         = GateKind::kAdd,
                              .g_sig        = cof0,
                              .add_coeff    = kAc,
                              .active_count = static_cast< uint32_t >(kDiff) }
                        );
                    }
                }
            }
        }

        if (candidates.empty()) { return std::nullopt; }

        // Sort by active variable count ascending (simpler first)
        std::sort(
            candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
                return a.active_count < b.active_count;
            }
        );

        std::optional< SubResult > best;

        for (const auto &cand : candidates) {
            // Build the variable list for g (all vars except k)
            // The cofactors were extracted by removing bit k from
            // the index. Variable indices in g_sig are: for the
            // original vars [0..n-1] minus k, packed in order.
            // Specifically, original var v maps to g-var v if v < k,
            // or g-var v-1 if v > k.
            std::vector< std::string > g_vars;
            std::vector< uint32_t > g_context_indices;
            for (uint32_t v = 0; v < kN; ++v) {
                if (v == cand.var_k) { continue; }
                g_vars.push_back(ctx.vars[v]);
                g_context_indices.push_back(v);
            }

            // Compact to active variables only
            const auto n_g                           = static_cast< uint32_t >(g_vars.size());
            auto [compacted_sig, active_var_indices] = CompactSignature(cand.g_sig, n_g);

            // Build context for active variables
            std::vector< std::string > active_vars;
            std::vector< uint32_t > active_context_indices;
            for (const uint32_t ai : active_var_indices) {
                active_vars.push_back(g_vars[ai]);
                active_context_indices.push_back(g_context_indices[ai]);
            }

            // Handle constant g_sig (no active variables)
            if (active_var_indices.empty()) {
                // g is a constant: build the composed expression
                auto g_expr = Expr::Constant(cand.g_sig[0]);
                auto composed =
                    Compose(cand.gate, cand.var_k, std::move(g_expr), cand.add_coeff);

                {
                    auto check = FullWidthCheckEval(*ctx.eval, kN, *composed, opts.bitwidth);
                    if (!check.passed) { continue; }
                }

                auto info = ComputeCost(*composed);
                if ((baseline_cost != nullptr) && !IsBetter(info.cost, *baseline_cost)) {
                    continue;
                }
                if (best.has_value() && !IsBetter(info.cost, best->cost)) { continue; }

                SubResult sub;
                sub.expr     = std::move(composed);
                sub.cost     = info.cost;
                sub.verified = true;
                best         = std::move(sub);
                continue;
            }

            // Build sub-problem context with evaluator for polynomial
            // recovery. The sub-evaluator fixes the split variable to
            // the appropriate value and maps compacted active variables
            // back to the parent context's variable space.
            SignatureContext g_ctx;
            g_ctx.vars = active_vars;
            g_ctx.original_indices.resize(active_vars.size());
            std::iota(g_ctx.original_indices.begin(), g_ctx.original_indices.end(), 0);

            // Construct sub-evaluator from parent evaluator.
            // AND: g(rest) = f(rest, v_k=all_ones)
            // OR/XOR/Add: g(rest) = f(rest, v_k=0)
            // Mul: g(rest) = f(rest, v_k=1)
            {
                uint64_t fixed_val = 0;
                if (cand.gate == GateKind::kAnd) {
                    fixed_val =
                        (opts.bitwidth == 64) ? UINT64_MAX : ((1ULL << opts.bitwidth) - 1);
                } else if (cand.gate == GateKind::kMul) {
                    fixed_val = 1;
                }
                // Add: fixed_val = 0 (already set)

                // active_var_indices maps compacted → g-space index
                // g-space index v maps to ctx variable index:
                //   v if v < k, v+1 if v >= k
                std::vector< uint32_t > compact_to_ctx;
                compact_to_ctx.reserve(active_var_indices.size());
                for (const uint32_t ai : active_var_indices) {
                    const uint32_t ctx_idx = (ai < cand.var_k) ? ai : (ai + 1);
                    compact_to_ctx.push_back(ctx_idx);
                }

                g_ctx.eval =
                    [eval = *ctx.eval, n_parent = kN, k = cand.var_k, fixed_val,
                     compact_to_ctx](const std::vector< uint64_t > &sub_vals) -> uint64_t {
                    std::vector< uint64_t > parent_vals(n_parent, 0);
                    parent_vals[k] = fixed_val;
                    for (size_t i = 0; i < compact_to_ctx.size(); ++i) {
                        parent_vals[compact_to_ctx[i]] = sub_vals[i];
                    }
                    return eval(parent_vals);
                };
            }

            auto sub_result =
                SimplifyFromSignature(compacted_sig, g_ctx, opts, depth + 1, baseline_cost);

            if (!sub_result.has_value()) { continue; }

            // Remap the sub-expression variable indices back to
            // context-space indices
            auto remapped = RemapVars(*sub_result->expr, active_context_indices);

            // Compose with the split variable
            auto composed = Compose(cand.gate, cand.var_k, std::move(remapped), cand.add_coeff);

            // Full-width verification using original evaluator.
            // Always required: sub-result may contain non-bitwise
            // nodes (e.g., x*y instead of x&y) that are {0,1}-correct
            // but wrong at full width.
            auto check = FullWidthCheckEval(*ctx.eval, kN, *composed, opts.bitwidth);
            if (!check.passed) { continue; }

            // Cost gate
            auto info = ComputeCost(*composed);
            if ((baseline_cost != nullptr) && !IsBetter(info.cost, *baseline_cost)) {
                continue;
            }
            if (best.has_value() && !IsBetter(info.cost, best->cost)) { continue; }

            SubResult sub;
            sub.expr     = std::move(composed);
            sub.cost     = info.cost;
            sub.verified = true;
            best         = std::move(sub);
        }

        COBRA_TRACE("BitwiseDecomp", "TryBitwiseDecomposition: found={}", best.has_value());
        return best;
    }

} // namespace cobra
