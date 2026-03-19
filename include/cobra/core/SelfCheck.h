#pragma once
#include "cobra/core/Expr.h"
#include "cobra/core/SemilinearIR.h"
#include <string>
#include <vector>

namespace cobra {

    struct SelfCheckResult
    {
        bool passed;
        std::string mismatch_detail;
    };

    SelfCheckResult SelfChecSemilinear(
        const SemilinearIR &original_ir, const Expr &reconstructed,
        const std::vector< std::string > &vars, uint32_t bitwidth
    );

} // namespace cobra
