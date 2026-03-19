#pragma once

#include "cobra/core/CoefficientSplitter.h" // Evaluator type
#include "cobra/core/Result.h"
#include "cobra/core/SingletonPowerPoly.h"

namespace cobra {

    Result< SingletonPowerResult >
    RecoverSingletonPowers(const Evaluator &eval, uint32_t num_vars, uint32_t bitwidth);

} // namespace cobra
