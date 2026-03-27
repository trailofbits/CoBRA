#pragma once

#include "cobra/core/BitwiseDecomposer.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/HybridDecomposer.h"
#include "cobra/core/Simplifier.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cobra {

    // Forward declarations to avoid circular includes.
    // GroupId is defined identically in CompetitionGroup.h.
    using GroupId = uint32_t;
    // ResidualOrigin is defined in Orchestrator.h.
    enum class ResidualOrigin : uint8_t;

    using JoinId = uint32_t;

    struct BitwiseComposeCont
    {
        uint32_t var_k;
        GateKind gate;
        uint64_t add_coeff;
        std::vector< uint32_t > active_context_indices;
        GroupId parent_group_id;
        std::optional< Evaluator > parent_eval;
        std::vector< std::string > parent_real_vars;
        std::vector< uint32_t > parent_original_indices;
        uint32_t parent_num_vars                      = 0;
        bool parent_needs_original_space_verification = true;
    };

    struct HybridComposeCont
    {
        uint32_t var_k;
        ExtractOp op;
        GroupId parent_group_id;
        std::optional< Evaluator > parent_eval;
        std::vector< std::string > parent_real_vars;
        std::vector< uint32_t > parent_original_indices;
        uint32_t parent_num_vars                      = 0;
        bool parent_needs_original_space_verification = true;
    };

    struct ResidualRecombineCont
    {
        std::unique_ptr< Expr > core_expr;
        ResidualOrigin origin;
        Evaluator residual_eval;
        std::vector< uint64_t > source_sig;
        std::vector< uint32_t > residual_support;
        uint8_t core_degree = 0;
        std::optional< GroupId > parent_group_id;
        // Target-local context for verification. When target_vars is
        // non-empty, recombination verifies against target_eval in
        // the target variable space instead of ctx.original_vars.
        Evaluator target_eval;
        std::vector< std::string > target_vars;
    };

    struct OperandRewriteCont
    {
        JoinId join_id;
        enum class OperandRole : uint8_t { kLhs, kRhs };
        OperandRole role;
    };

    struct ProductCollapseCont
    {
        JoinId join_id;
        enum class FactorRole : uint8_t { kX, kY };
        FactorRole role;
    };

    enum class LiftedValueKind : uint8_t {
        kArithmeticAtom,
        kRepeatedSubexpression,
    };

    struct LiftedBinding
    {
        LiftedValueKind kind;
        uint32_t outer_var_index;
        std::unique_ptr< Expr > subtree;
        uint64_t structural_hash = 0;
        std::vector< uint32_t > original_support;
    };

    struct LiftedSubstituteCont
    {
        std::vector< LiftedBinding > bindings;
        std::vector< std::string > outer_vars;
        std::vector< uint32_t > outer_original_indices;
        uint32_t original_var_count = 0;
        Evaluator original_eval;
        std::vector< std::string > original_vars;
        std::vector< uint64_t > source_sig;
    };

    using ContinuationData = std::variant<
        std::monostate, BitwiseComposeCont, HybridComposeCont, ResidualRecombineCont,
        OperandRewriteCont, ProductCollapseCont, LiftedSubstituteCont >;

    // Projects a parent's baseline cost for a child work item
    // given the continuation that will recombine the child's result.
    // Conservative initial implementation: returns nullopt for all
    // continuation types (no baseline inheritance).
    inline std::optional< ExprCost > ProjectBaselineForChild(
        const std::optional< ExprCost > & /*parent_baseline*/,
        const ContinuationData & /*continuation*/
    ) {
        return std::nullopt;
    }

} // namespace cobra
