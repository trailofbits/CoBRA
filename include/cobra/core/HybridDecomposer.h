#pragma once

#include "cobra/core/ExprCost.h"
#include "cobra/core/SignatureSimplifier.h"
#include <cstdint>
#include <optional>
#include <vector>

namespace cobra {

    // Variable-extraction decomposition for hybrid expressions.
    // For each variable xi and invertible operator OP (XOR, ADD):
    //   1. Compute residual r(vars) = f(vars) OP^{-1} xi
    //   2. If r is simpler than f, recursively simplify r
    //   3. Compose: f = xi OP r_simplified
    //   4. Full-width verify
    std::optional< SubResult > TryHybridDecomposition(
        const std::vector< uint64_t > &sig, const SignatureContext &ctx, const Options &opts,
        uint32_t depth, const ExprCost *baseline_cost
    );

} // namespace cobra
