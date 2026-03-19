#pragma once

#include "cobra/core/Expr.h"
#include <cstdint>
#include <memory>

namespace cobra {

    struct RewriteResult
    {
        std::unique_ptr< Expr > expr;
        uint32_t rounds_applied = 0;
        bool structure_changed  = false;
    };

    struct RewriteOptions
    {
        uint32_t max_rounds      = 2;
        uint32_t max_node_growth = 3;
        uint32_t bitwidth        = 64;
    };

    uint32_t NodeCount(const Expr &expr);

    uint32_t CountRewriteableSites(const Expr &expr);

    RewriteResult
    RewriteMixedProducts(std::unique_ptr< Expr > expr, const RewriteOptions &opts);

} // namespace cobra
