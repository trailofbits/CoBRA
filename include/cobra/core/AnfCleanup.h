#pragma once

#include "cobra/core/Expr.h"
#include "cobra/core/PackedAnf.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace cobra {

    struct AnfForm
    {
        uint8_t constant_bit = 0;
        std::vector< uint32_t > monomials; // nonzero masks, sorted by
                                           // degree then value
        uint32_t num_vars = 0;

        static AnfForm FromAnfCoeffs(const PackedAnf &anf, uint32_t num_vars);
    };

    // Emit an Expr from an AnfForm without any cleanup (raw XOR/AND tree).
    std::unique_ptr< Expr > EmitRawAnf(const AnfForm &form);

    // Compute tree cost: number of AST nodes.
    uint32_t ExprCost(const Expr &expr);

    // Clean up an ANF monomial set and emit an optimized Expr.
    std::unique_ptr< Expr > CleanupAnf(const AnfForm &form);

} // namespace cobra
