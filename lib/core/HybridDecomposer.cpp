#include "cobra/core/HybridDecomposer.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureSimplifier.h"
#include "cobra/core/Simplifier.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace cobra {

    namespace {

        // Count active variables in a signature.
        uint32_t CountActiveVars(const std::vector< uint64_t > &sig, uint32_t n) {
            uint32_t count = 0;
            for (uint32_t v = 0; v < n; ++v) {
                for (size_t j = 0; j < sig.size(); ++j) {
                    const size_t kFlipped = j ^ (1U << v);
                    if (sig[j] != sig[kFlipped]) {
                        ++count;
                        break;
                    }
                }
            }
            return count;
        }

        // Remap variable indices using the provided index map.
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

        enum class ExtractOp { kXor, kAdd };

        // Build the residual signature: r_sig[i] = sig[i] OP^{-1} bit_k(i)
        // XOR: r_sig[i] = sig[i] ^ ((i >> k) & 1)
        // ADD: r_sig[i] = sig[i] - ((i >> k) & 1)
        std::vector< uint64_t >
        BuildResidualSig(const std::vector< uint64_t > &sig, uint32_t k, ExtractOp op) {
            std::vector< uint64_t > r_sig(sig.size());
            for (size_t i = 0; i < sig.size(); ++i) {
                const uint64_t kVk = (i >> k) & 1;
                switch (op) {
                    case ExtractOp::kXor:
                        r_sig[i] = sig[i] ^ kVk;
                        break;
                    case ExtractOp::kAdd:
                        r_sig[i] = sig[i] - kVk;
                        break;
                }
            }
            return r_sig;
        }

        // Compose the final expression: f = xi OP r_expr
        std::unique_ptr< Expr >
        ComposeExtraction(ExtractOp op, uint32_t original_k, std::unique_ptr< Expr > r_expr) {
            auto var_k = Expr::Variable(original_k);
            switch (op) {
                case ExtractOp::kXor:
                    return Expr::BitwiseXor(std::move(var_k), std::move(r_expr));
                case ExtractOp::kAdd:
                    return Expr::Add(std::move(var_k), std::move(r_expr));
            }
            return Expr::Constant(0);
        }

        struct ExtractionCandidate
        {
            uint32_t var_k;
            ExtractOp op;
            std::vector< uint64_t > r_sig;
            uint32_t active_count;
        };

    } // namespace

    std::optional< SubResult > TryHybridDecomposition(
        const std::vector< uint64_t > &sig, const SignatureContext &ctx, const Options &opts,
        uint32_t depth, const ExprCost *baseline_cost
    ) {
        if (!ctx.eval) { return std::nullopt; }
        // Only try extraction at the top level (depth 0) to limit
        // combinatorial blowup. The recursive call at depth 1 uses
        // the standard pipeline without further extraction.
        if (depth >= 1) { return std::nullopt; }
        if (sig.size() < 2) { return std::nullopt; }

        const auto kN = static_cast< uint32_t >(ctx.vars.size());

        std::vector< ExtractionCandidate > candidates;

        for (uint32_t k = 0; k < kN; ++k) {
            for (auto op : { ExtractOp::kXor, ExtractOp::kAdd }) {
                auto r_sig = BuildResidualSig(sig, k, op);

                // Skip if the residual is identical to the original
                // (extraction had no effect, e.g., variable is unused).
                if (r_sig == sig) { continue; }

                const uint32_t r_active = CountActiveVars(r_sig, kN);
                candidates.push_back(
                    { .var_k        = k,
                      .op           = op,
                      .r_sig        = std::move(r_sig),
                      .active_count = r_active }
                );
            }
        }

        if (candidates.empty()) { return std::nullopt; }

        // Sort by active variable count (simpler first)
        std::sort(
            candidates.begin(), candidates.end(),
            [](const ExtractionCandidate &a, const ExtractionCandidate &b) {
                return a.active_count < b.active_count;
            }
        );

        std::optional< SubResult > best;

        for (const auto &cand : candidates) {
            // Build sub-evaluator: r_eval(vars) = f(vars) OP^{-1} v_k
            auto r_eval = [eval = *ctx.eval, k = cand.var_k,
                           op = cand.op](const std::vector< uint64_t > &vals) -> uint64_t {
                const uint64_t f_val = eval(vals);
                switch (op) {
                    case ExtractOp::kXor:
                        return f_val ^ vals[k];
                    case ExtractOp::kAdd:
                        return f_val - vals[k];
                }
                return f_val;
            };

            SignatureContext r_ctx;
            r_ctx.vars             = ctx.vars;
            r_ctx.original_indices = ctx.original_indices;
            r_ctx.eval             = r_eval;

            auto sub_result =
                SimplifyFromSignature(cand.r_sig, r_ctx, opts, depth + 1, baseline_cost);

            if (!sub_result.has_value()) { continue; }

            // Compose in context-space (variable indices 0..n-1).
            // No remap needed: sub-result already uses context indices.
            auto composed =
                ComposeExtraction(cand.op, cand.var_k, CloneExpr(*sub_result->expr));

            // Full-width verification
            auto check = FullWidthCheckEval(*ctx.eval, kN, *composed, opts.bitwidth);
            if (!check.passed) { continue; }

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

        return best;
    }

} // namespace cobra
