#include "cobra/core/HybridDecomposer.h"
#include "cobra/core/Expr.h"
#include "cobra/core/Profile.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
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

} // namespace cobra
