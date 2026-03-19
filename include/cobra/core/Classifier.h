#pragma once

#include "cobra/core/Classification.h"
#include "cobra/core/Expr.h"
#include <memory>

namespace cobra {

    std::unique_ptr< Expr >
    FoldConstantBitwise(std::unique_ptr< Expr > expr, uint32_t bitwidth);

    Classification ClassifyStructural(const Expr &expr);

} // namespace cobra
