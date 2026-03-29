#pragma once

#include "cobra/core/Evaluator.h"
#include "cobra/core/Expr.h"
#include <cstdint>
#include <vector>

namespace cobra {

    struct CheckResult
    {
        bool passed;
        std::vector< uint64_t > failing_input; // empty if passed
    };

    // Verify that the simplified Expr produces the same output as the original
    // signature vector at all {0,1} input combinations. This is an exhaustive
    // check — sufficient for linear MBAs by the signature-vector equivalence
    // theorem (Reichenwallner et al.).
    CheckResult SignatureCheck(
        const std::vector< uint64_t > &original_sig, const Expr &simplified, uint32_t num_vars,
        uint32_t bitwidth
    );

    // Verify that the simplified Expr produces the same output as the original
    // Expr at random full-width inputs. Catches polynomial MBA results that are
    // {0,1}-correct but wrong on wider bitvectors (e.g., CoB emitting x&y when
    // the original computes x*y).
    //
    // var_map maps simplified variable indices to original variable indices
    // (needed when aux variable elimination reduced the variable set).
    // If empty, a 1:1 identity mapping is assumed.
    CheckResult FullWidthCheck(
        const Expr &original, uint32_t original_num_vars, const Expr &simplified,
        const std::vector< uint32_t > &var_map, uint32_t bitwidth, uint32_t num_samples = 8
    );

    // Verify that the simplified Expr produces the same output as an evaluator
    // at random full-width inputs. When the evaluator is Expr-backed, the
    // original side runs through its compiled program; otherwise this falls
    // back to the evaluator callback.
    CheckResult FullWidthCheckEval(
        const Evaluator &eval_original, uint32_t num_vars, const Expr &simplified,
        uint32_t bitwidth, uint32_t num_samples = 8
    );

    // Evaluate an Expr tree at given variable values.
    uint64_t
    EvalExpr(const Expr &expr, const std::vector< uint64_t > &var_values, uint32_t bitwidth);

} // namespace cobra
