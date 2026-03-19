#pragma once

#include "cobra/core/ExprCost.h"
#include "cobra/core/SignatureSimplifier.h"
#include <cstdint>
#include <optional>
#include <vector>

namespace cobra {

    std::optional< SubResult > TryBitwiseDecomposition(
        const std::vector< uint64_t > &sig, const SignatureContext &ctx, const Options &opts,
        uint32_t depth, const ExprCost *baseline_cost
    );

} // namespace cobra
