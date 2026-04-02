#pragma once

#include "cobra/core/Classification.h"
#include "cobra/core/Evaluator.h"
#include "cobra/core/Expr.h"
#include "cobra/core/PassContract.h"
#include "cobra/core/Result.h"
#include "cobra/core/SimplifyOutcome.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cobra {

    struct Options
    {
        uint32_t bitwidth                 = 64;
        uint32_t max_vars                 = 16;
        bool spot_check                   = true;
        bool enable_bitwise_decomposition = true;
        StructuralFlag structural_flags   = static_cast< StructuralFlag >(0);
        Evaluator evaluator;
    };

    Result< SimplifyOutcome > Simplify(
        const std::vector< uint64_t > &sig, const std::vector< std::string > &vars,
        const Expr *input_expr, const Options &opts
    );

} // namespace cobra
