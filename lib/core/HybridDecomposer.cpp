#include "cobra/core/HybridDecomposer.h"
#include "ContinuationTypes.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/PassContract.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureSimplifier.h"
#include "cobra/core/Simplifier.h"
#include "cobra/core/Trace.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace cobra {

    // ---------------------------------------------------------------
    // Public helpers for candidate enumeration
    // ---------------------------------------------------------------

    namespace {

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

    } // namespace

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

    std::vector< HybridExtractionCandidate >
    EnumerateHybridCandidates(const std::vector< uint64_t > &sig, uint32_t num_vars) {
        std::vector< HybridExtractionCandidate > candidates;
        candidates.reserve(2 * num_vars);

        for (uint32_t k = 0; k < num_vars; ++k) {
            for (auto op : { ExtractOp::kXor, ExtractOp::kAdd }) {
                auto r_sig = BuildResidualSig(sig, k, op);

                if (r_sig == sig) { continue; }

                const uint32_t r_active = CountActiveVars(r_sig, num_vars);
                candidates.push_back(
                    { .var_k        = k,
                      .op           = op,
                      .r_sig        = std::move(r_sig),
                      .active_count = r_active }
                );
            }
        }

        std::sort(
            candidates.begin(), candidates.end(),
            [](const HybridExtractionCandidate &a, const HybridExtractionCandidate &b) {
                return a.active_count < b.active_count;
            }
        );

        return candidates;
    }

    namespace {

        void AppendReasonFrames(std::vector< ReasonFrame > &out, const ReasonDetail &detail) {
            out.push_back(detail.top);
            for (const auto &cause : detail.causes) { out.push_back(cause); }
        }

    } // namespace

    namespace hybrid_decomposer {

        enum Subcode : uint16_t {
            kNoEvaluator  = 1,
            kDepthLimit   = 2,
            kTooFewVars   = 3,
            kNoCandidates = 4,
            kNoMatch      = 5,
        };

    } // namespace hybrid_decomposer

    SolverResult< SignaturePayload > TryHybridDecomposition(
        const std::vector< uint64_t > &sig, const SignatureContext &ctx, const Options &opts,
        uint32_t depth, const ExprCost *baseline_cost
    ) {
        COBRA_TRACE(
            "HybridDecomp", "TryHybridDecomposition: vars={} depth={}", ctx.vars.size(), depth
        );
        if (!ctx.eval) {
            ReasonDetail reason{
                .top = { .code    = { ReasonCategory::kGuardFailed,
                                      ReasonDomain::kHybridDecomposer,
                                      hybrid_decomposer::kNoEvaluator },
                        .message = "no evaluator available" }
            };
            return SolverResult< SignaturePayload >::Inapplicable(std::move(reason));
        }
        // Only try extraction at the top level (depth 0) to limit
        // combinatorial blowup. The recursive call at depth 1 uses
        // the standard pipeline without further extraction.
        if (depth >= 1) {
            ReasonDetail reason{
                .top = { .code    = { ReasonCategory::kResourceLimit,
                                      ReasonDomain::kHybridDecomposer,
                                      hybrid_decomposer::kDepthLimit },
                        .message = "recursion depth limit reached" }
            };
            return SolverResult< SignaturePayload >::Blocked(std::move(reason));
        }
        if (sig.size() < 2) {
            ReasonDetail reason{
                .top = { .code    = { ReasonCategory::kGuardFailed,
                                      ReasonDomain::kHybridDecomposer,
                                      hybrid_decomposer::kTooFewVars },
                        .message = "too few variables for extraction" }
            };
            return SolverResult< SignaturePayload >::Inapplicable(std::move(reason));
        }

        const auto kN = static_cast< uint32_t >(ctx.vars.size());

        auto candidates = EnumerateHybridCandidates(sig, kN);

        if (candidates.empty()) {
            ReasonDetail reason{
                .top = { .code = { ReasonCategory::kNoSolution, ReasonDomain::kHybridDecomposer,
                                   hybrid_decomposer::kNoCandidates },
                        .message = "no extraction candidates found" }
            };
            return SolverResult< SignaturePayload >::Blocked(std::move(reason));
        }

        std::optional< SignaturePayload > best;
        std::vector< ReasonFrame > all_causes;

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

            if (!sub_result.Succeeded()) {
                AppendReasonFrames(all_causes, sub_result.Reason());
                continue;
            }

            // Compose in context-space (variable indices 0..n-1).
            // No remap needed: sub-result already uses context indices.
            auto composed =
                ComposeExtraction(cand.op, cand.var_k, CloneExpr(*sub_result.Payload().expr));

            // Full-width verification
            auto check = FullWidthCheckEval(*ctx.eval, kN, *composed, opts.bitwidth);
            if (!check.passed) { continue; }

            auto info = ComputeCost(*composed);
            if ((baseline_cost != nullptr) && !IsBetter(info.cost, *baseline_cost)) {
                continue;
            }
            if (best.has_value() && !IsBetter(info.cost, best->cost)) { continue; }

            best = SignaturePayload{
                .expr         = std::move(composed),
                .cost         = info.cost,
                .verification = VerificationState::kVerified,
            };
        }

        COBRA_TRACE("HybridDecomp", "TryHybridDecomposition: found={}", best.has_value());
        if (best.has_value()) {
            return SolverResult< SignaturePayload >::Success(std::move(*best));
        }
        ReasonDetail reason{
            .top = { .code = { ReasonCategory::kSearchExhausted,
                               ReasonDomain::kHybridDecomposer, hybrid_decomposer::kNoMatch },
                    .message = "no extraction matched" },
            .causes = std::move(all_causes),
        };
        return SolverResult< SignaturePayload >::Blocked(std::move(reason));
    }

} // namespace cobra
