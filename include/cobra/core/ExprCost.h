#pragma once

#include "cobra/core/Expr.h"
#include <cstdint>

namespace cobra {

    struct ExprCost
    {
        uint32_t weighted_size       = 0;
        uint32_t nonlinear_mul_count = 0;
        uint32_t max_depth           = 0;
    };

    struct CostInfo
    {
        ExprCost cost;
        bool has_var_dep = false;
    };

    CostInfo ComputeCost(const Expr &expr);

    bool IsBetter(const ExprCost &candidate, const ExprCost &baseline);

} // namespace cobra
