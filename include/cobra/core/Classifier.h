#pragma once

#include "cobra/core/Classification.h"
#include "cobra/core/Expr.h"
#include <memory>

namespace cobra {

    std::unique_ptr< Expr > FoldConstantBitwise(
        std::unique_ptr< Expr > expr, uint32_t bitwidth
    ); // NOLINT(readability-identifier-naming)

    Classification ClassifyStructural(const Expr &expr);

} // namespace cobra
