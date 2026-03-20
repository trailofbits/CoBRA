#pragma once

#include "cobra/core/Expr.h"
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace cobra {

    // Evaluator for a ghost primitive — takes exactly `arity` args.
    using GhostEval = uint64_t (*)(std::span< const uint64_t >, uint32_t bw);

    // Builder: takes original variable indices, returns canonical Expr.
    using GhostBuilder = std::unique_ptr< Expr > (*)(std::span< const uint32_t >);

    struct GhostPrimitive
    {
        const char *name;
        uint8_t arity;
        bool symmetric;
        GhostEval eval;
        GhostBuilder build;
    };

    const std::vector< GhostPrimitive > &GetGhostBasis();

} // namespace cobra
