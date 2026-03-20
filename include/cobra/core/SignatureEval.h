#pragma once

#include "cobra/core/Expr.h"
#include <cstdint>
#include <functional>
#include <vector>

namespace cobra {

    // Evaluate an Expr at all 2^n Boolean input combinations.
    // Variable index v corresponds to bit v in the signature index.
    std::vector< uint64_t >
    EvaluateBooleanSignature(const Expr &expr, uint32_t num_vars, uint32_t bitwidth);

    // Evaluate an evaluator function at all 2^n Boolean input combinations.
    // Equivalent to the Expr overload but takes a callback instead of an AST.
    std::vector< uint64_t > EvaluateBooleanSignature(
        const std::function< uint64_t(const std::vector< uint64_t > &) > &eval,
        uint32_t num_vars, uint32_t bitwidth
    );

} // namespace cobra
