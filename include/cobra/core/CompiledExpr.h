#pragma once

#include "cobra/core/Expr.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cobra {

    struct EvalInstr
    {
        Expr::Kind kind;
        uint64_t operand = 0;
    };

    struct CompiledExpr
    {
        uint32_t bitwidth = 64;
        uint64_t mask     = 0;
        uint32_t arity    = 0;
        size_t stack_size = 1;
        std::vector< EvalInstr > program;
    };

    CompiledExpr CompileExpr(const Expr &expr, uint32_t bitwidth);

    uint64_t EvalCompiledExpr(
        const CompiledExpr &compiled, const std::vector< uint64_t > &var_values,
        std::vector< uint64_t > &stack
    );

} // namespace cobra
