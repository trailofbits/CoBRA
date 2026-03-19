#pragma once

#include "cobra/core/PolyIR.h"
#include "cobra/core/Result.h"
#include <vector>

namespace cobra {

    struct LoweringResult
    {
        PolyIR poly;
        std::vector< Coeff > residual_and_coeffs;
    };

    Result< LoweringResult > LowerArithmeticFragment(
        const std::vector< Coeff > &and_coeffs, const std::vector< Coeff > &mul_coeffs,
        uint8_t num_vars, uint32_t bitwidth
    );

} // namespace cobra
