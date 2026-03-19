#pragma once

#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/Simplifier.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cobra {

    struct SubResult
    {
        std::unique_ptr< Expr > expr;
        ExprCost cost;
        bool verified = false;
    };

    struct SignatureContext
    {
        std::vector< std::string > vars;
        std::vector< uint32_t > original_indices;
        std::optional< Evaluator > eval;
    };

    std::optional< SubResult > SimplifyFromSignature(
        const std::vector< uint64_t > &sig, const SignatureContext &ctx, const Options &opts,
        uint32_t depth, const ExprCost *baseline_cost = nullptr
    );

    bool IsBooleanValued(const std::vector< uint64_t > &sig);

} // namespace cobra
