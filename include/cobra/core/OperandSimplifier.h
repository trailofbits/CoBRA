#pragma once

#include "cobra/core/Expr.h"
#include "cobra/core/Simplifier.h"
#include <memory>
#include <string>
#include <vector>

namespace cobra {

    struct OperandSimplifyResult
    {
        std::unique_ptr< Expr > expr;
        bool changed = false;
    };

    // NOLINTNEXTLINE(readability-identifier-naming)
    OperandSimplifyResult SimplifyMixedOperands(
        std::unique_ptr< Expr > expr, const std::vector< std::string > &vars,
        const Options &opts
    );

} // namespace cobra
