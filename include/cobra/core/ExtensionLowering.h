#pragma once

#include "cobra/core/Expr.h"

#include <cstdint>
#include <memory>

namespace cobra {

    // Scalar helpers — used by frontend evaluators.
    // Precondition: 1 <= source_bits <= 64.
    uint64_t EvalZeroExtend(uint64_t val, uint32_t source_bits, uint64_t result_mask);
    uint64_t EvalSignExtend(uint64_t val, uint32_t source_bits, uint64_t result_mask);

    // Expr helpers — used by frontend AST builders.
    // Return ordinary fixed-width Expr; no new Expr::Kind values.
    // Precondition: 1 <= source_bits <= 64.
    // When source_bits == 64, returns inner unchanged (identity).
    std::unique_ptr< Expr >
    LowerZeroExtend(std::unique_ptr< Expr > inner, uint32_t source_bits);
    std::unique_ptr< Expr >
    LowerSignExtend(std::unique_ptr< Expr > inner, uint32_t source_bits);

} // namespace cobra
