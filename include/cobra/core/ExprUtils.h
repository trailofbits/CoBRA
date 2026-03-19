#pragma once

#include "cobra/core/Expr.h"
#include <cstdint>

namespace cobra {

    /// Build an AND-product expression from a bitmask (v0 & v1 & ...).
    std::unique_ptr< Expr > BuildAndProduct(uint64_t mask);

    /// Apply a coefficient to an expression: 1*e → e, -1*e → -e, c*e → c*e.
    std::unique_ptr< Expr >
    ApplyCoefficient(std::unique_ptr< Expr > expr, uint64_t coeff, uint32_t bitwidth);

    /// Check if an Expr subtree contains only constants (no variables).
    bool IsConstantSubtree(const Expr &expr);

    /// Evaluate a constant-only Expr subtree (no variables allowed).
    /// Throws std::runtime_error if a Variable node is encountered.
    /// Result is masked to Bitmask(bitwidth).
    uint64_t EvalConstantExpr(const Expr &expr, uint32_t bitwidth);

    /// Check if an Expr subtree depends on any variable.
    bool HasVarDep(const Expr &expr);

    /// Check if an Expr subtree contains a non-leaf bitwise node
    /// (And/Or/Xor/Not with variable dependence).
    bool HasNonleafBitwise(const Expr &expr);

} // namespace cobra
