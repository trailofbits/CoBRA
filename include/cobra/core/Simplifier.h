#pragma once

#include "cobra/core/Classification.h"
#include "cobra/core/Expr.h"
#include "cobra/core/Result.h"
#include "cobra/core/SimplifyOutcome.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cobra {

    using Evaluator = std::function< uint64_t(const std::vector< uint64_t > &) >;

    struct Options
    {
        uint32_t bitwidth                 = 64;
        uint32_t max_vars                 = 16;
        bool spot_check                   = true;
        bool enable_bitwise_decomposition = true;
        Evaluator evaluator;
        StructuralFlag structural_flags = static_cast< StructuralFlag >(0);
    };

    Result< SimplifyOutcome > Simplify(
        const std::vector< uint64_t > &sig, const std::vector< std::string > &vars,
        const Expr *input_expr, const Options &opts
    );

    // Internal: run the standard supported-fragment pipeline.
    // Exposed for use by the decomposition engine's residual solver.
    // Not part of the public API — use Simplify() for external callers.
    Result< SimplifyOutcome > RunSupportedPipeline(
        const std::vector< uint64_t > &sig, const std::vector< std::string > &vars,
        const Options &opts
    );

} // namespace cobra
