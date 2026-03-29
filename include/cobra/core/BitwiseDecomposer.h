#pragma once

#include "cobra/core/Expr.h"
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace cobra {

    // Gate kind for bitwise decomposition candidates.
    enum class GateKind { kAnd, kOr, kXor, kMul, kAdd };

    struct BitwiseSplitCandidate
    {
        uint32_t var_k;
        GateKind gate;
        std::vector< uint64_t > g_sig;
        uint64_t add_coeff    = 0;
        uint32_t active_count = 0;
    };

    // Enumerate cofactor-based decomposition candidates for all
    // variables and gate types. Returns candidates sorted by
    // active_count ascending.
    std::vector< BitwiseSplitCandidate >
    EnumerateBitwiseCandidates(const std::vector< uint64_t > &sig, uint32_t num_vars);

    // Count active variables in a signature (variables whose
    // flipping changes at least one signature entry).
    uint32_t CountActive(const std::vector< uint64_t > &sig, uint32_t n);

    // Compact a signature to only its active variables.
    // Returns {compacted_sig, active_var_indices}.
    std::pair< std::vector< uint64_t >, std::vector< uint32_t > >
    CompactSignature(const std::vector< uint64_t > &sig, uint32_t n);

    // Remap variable indices in an expression tree using the
    // provided index map (compacted index -> original index).
    std::unique_ptr< Expr >
    RemapVars(const Expr &expr, const std::vector< uint32_t > &index_map);

    // Build composed expression: gate(v_k, g_expr)
    std::unique_ptr< Expr > Compose(
        GateKind gate, uint32_t original_k, std::unique_ptr< Expr > g_expr,
        uint64_t add_coeff = 0
    );

} // namespace cobra
