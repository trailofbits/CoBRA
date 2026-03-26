#pragma once

#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/PassContract.h"
#include "cobra/core/SignatureSimplifier.h"
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

    // Variable-extraction decomposition for hybrid expressions.
    // For each variable xi and invertible operator OP (XOR, ADD):
    //   1. Compute residual r(vars) = f(vars) OP^{-1} xi
    //   2. If r is simpler than f, recursively simplify r
    //   3. Compose: f = xi OP r_simplified
    //   4. Full-width verify
    SolverResult< SignaturePayload > TryHybridDecomposition(
        const std::vector< uint64_t > &sig, const SignatureContext &ctx, const Options &opts,
        uint32_t depth, const ExprCost *baseline_cost
    );

} // namespace cobra
