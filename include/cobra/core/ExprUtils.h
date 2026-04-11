#pragma once

#include "cobra/core/Expr.h"
#include <cstdint>
#include <string>
#include <vector>

namespace cobra {

    /// Build an AND-product expression from a bitmask (v0 & v1 & ...).
    std::unique_ptr< Expr > BuildAndProduct(uint64_t mask);

    /// Apply a coefficient to an expression: 1*e → e, -1*e → -e, c*e → c*e.
    std::unique_ptr< Expr > ApplyCoefficient(
        std::unique_ptr< Expr > expr, uint64_t coeff, uint32_t bitwidth
    ); // NOLINT(readability-identifier-naming)

    /// Rewrite every kVariable node's var_index through index_map.
    void RemapVarIndices(Expr &expr, const std::vector< uint32_t > &index_map);

    /// Map a subset of variable names to their indices in the full variable list.
    std::vector< uint32_t > BuildVarSupport(
        const std::vector< std::string > &all_vars,
        const std::vector< std::string > &subset_vars
    );

    /// Check if an Expr subtree contains only constants (no variables).
    bool IsConstantSubtree(const Expr &expr);

    /// Evaluate a constant-only Expr subtree (no variables allowed).
    /// Throws std::runtime_error if a Variable node is encountered.
    /// Result is masked to Bitmask(bitwidth).
    uint64_t EvalConstantExpr(const Expr &expr, uint32_t bitwidth);

    /// Cosmetic cleanup on the final simplified expression.
    /// Chains: constant folding → negation refolding.
    /// Semantics-preserving, no verification needed.
    std::unique_ptr< Expr > CleanupFinalExpr(std::unique_ptr< Expr > expr, uint32_t bitwidth);

    /// Check if an Expr subtree depends on any variable.
    bool HasVarDep(const Expr &expr);

    /// Check if an Expr subtree contains a non-leaf bitwise node
    /// (And/Or/Xor/Not with variable dependence).
    bool HasNonleafBitwise(const Expr &expr);

    /// Replace AND(var-dep, var-dep) with MUL in an expression tree.
    /// Corrects the product-shadow divergence where AND = MUL on {0,1}
    /// but not at full width.
    std::unique_ptr< Expr > RepairProductShadow(std::unique_ptr< Expr > expr);

} // namespace cobra
