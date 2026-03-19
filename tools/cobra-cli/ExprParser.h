#pragma once

#include "cobra/core/Expr.h"
#include "cobra/core/Result.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cobra {

    struct ParseResult
    {
        std::vector< uint64_t > sig;
        std::vector< std::string > vars;
    };

    struct AstResult
    {
        std::unique_ptr< Expr > expr;
        std::vector< std::string > vars;
    };

    // Parse an MBA expression string and evaluate it over all {0,1}
    // combinations to produce a signature vector. Variables are extracted
    // and sorted lexicographically.
    Result< ParseResult > ParseAndEvaluate(const std::string &expr, uint32_t bitwidth);

    // Parse an MBA expression string into an Expr tree. Variables are
    // extracted and sorted lexicographically; variable indices correspond
    // to positions in the sorted vars list. Constants are masked to
    // Bitmask(bitwidth).
    Result< AstResult > ParseToAst(const std::string &expr, uint32_t bitwidth);

    // Check if a parsed expression is a linear MBA (no variable*variable
    // multiplication). Returns false if any Mul node has both children
    // depending on variables — including x*x.
    bool IsLinearMba(const std::string &expr);

} // namespace cobra
