#pragma once

#include "cobra/core/ExprCost.h"
#include "cobra/core/PassContract.h"
#include "cobra/core/SignatureSimplifier.h"
#include <cstdint>
#include <vector>

namespace cobra {

    SolverResult< SignaturePayload > TryBitwiseDecomposition(
        const std::vector< uint64_t > &sig, const SignatureContext &ctx, const Options &opts,
        uint32_t depth, const ExprCost *baseline_cost
    );

} // namespace cobra
