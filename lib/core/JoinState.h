#pragma once

#include "CompetitionGroup.h"
#include "ContinuationTypes.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace cobra {

    struct OperandJoinState
    {
        std::optional< CandidateRecord > lhs_winner;
        std::optional< CandidateRecord > rhs_winner;
        bool lhs_resolved = false;
        bool rhs_resolved = false;
        // The full AST containing the target Mul.
        std::unique_ptr< Expr > full_ast;
        // The target Mul node (cloned for FW verification).
        std::unique_ptr< Expr > original_mul;
        // Hash of the target Mul for replacement in the full AST.
        size_t target_hash = 0;
        ExprCost baseline_cost;
        std::vector< std::string > vars;
        uint32_t bitwidth    = 64;
        uint32_t rewrite_gen = 0;
    };

    struct ProductJoinState
    {
        std::optional< CandidateRecord > x_winner;
        std::optional< CandidateRecord > y_winner;
        bool x_resolved = false;
        bool y_resolved = false;
        std::unique_ptr< Expr > original_expr;
        ExprCost baseline_cost;
        std::vector< std::string > vars;
        uint32_t bitwidth    = 64;
        uint32_t rewrite_gen = 0;
        // Full AST for replacement splicing.
        std::unique_ptr< Expr > full_ast;
        size_t target_hash = 0;
    };

    using JoinState = std::variant< OperandJoinState, ProductJoinState >;

    // Allocates a new join and returns its id.
    JoinId CreateJoin(
        std::unordered_map< JoinId, JoinState > &joins, JoinId &next_id, JoinState state
    );

    // Walk the AST, replacing the first node whose hash matches
    // target_hash with the replacement. Returns the rebuilt AST.
    // The replacement is consumed on match; left intact otherwise.
    std::unique_ptr< Expr > ReplaceByHash(
        std::unique_ptr< Expr > root, size_t target_hash, std::unique_ptr< Expr > &replacement,
        bool &replaced
    );

} // namespace cobra
