#pragma once

#include "cobra/core/Expr.h"
#include "cobra/core/Result.h"
#include "cobra/core/SemilinearIR.h"
#include <string>
#include <vector>

namespace cobra {

    Result< SemilinearIR > NormalizeToSemilinear(
        const Expr &expr, const std::vector< std::string > &vars, uint32_t bitwidth
    );

} // namespace cobra
