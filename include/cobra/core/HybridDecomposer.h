#pragma once

#include "cobra/core/Expr.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace cobra {

    enum class ExtractOp : uint8_t { kXor, kAdd };

    struct HybridExtractionCandidate
    {
        uint32_t var_k;
        ExtractOp op;
        std::vector< uint64_t > r_sig;
        uint32_t active_count = 0;
    };

    // Enumerate variable-extraction candidates for all variables
    // and invertible operators. Returns candidates sorted by
    // active_count ascending.
    std::vector< HybridExtractionCandidate >
    EnumerateHybridCandidates(const std::vector< uint64_t > &sig, uint32_t num_vars);

    // Build the residual signature: r_sig[i] = sig[i] OP^{-1} bit_k(i)
    std::vector< uint64_t >
    BuildResidualSig(const std::vector< uint64_t > &sig, uint32_t k, ExtractOp op);

    // Compose the final expression: f = xi OP r_expr
    std::unique_ptr< Expr >
    ComposeExtraction(ExtractOp op, uint32_t original_k, std::unique_ptr< Expr > r_expr);

} // namespace cobra
