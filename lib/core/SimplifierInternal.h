#pragma once

#include "cobra/core/Expr.h"
#include "cobra/core/PassContract.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/Simplifier.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cobra::internal {

    bool HasNotOverArith(const Expr &e);

    std::unique_ptr< Expr > LowerNotOverArith(std::unique_ptr< Expr > e, uint32_t bitwidth);

    CheckResult VerifyInOriginalSpace(
        const Evaluator &eval, const std::vector< std::string > &all_vars,
        const std::vector< std::string > &real_vars, const Expr &reduced_expr, uint32_t bitwidth
    );

} // namespace cobra::internal
