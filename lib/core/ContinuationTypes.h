#pragma once

#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/Simplifier.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace cobra {

    // Forward declarations to avoid circular includes.
    // GroupId is defined identically in CompetitionGroup.h.
    using GroupId = uint32_t;
    // ResidualOrigin is defined in Orchestrator.h.
    enum class ResidualOrigin : uint8_t;

    using JoinId = uint32_t;

    enum class ExtractOp : uint8_t { kXor, kAdd };

    struct BitwiseComposeCont
    {
        uint32_t var_k;
        Expr::Kind gate;
        uint64_t add_coeff;
        std::vector< uint32_t > active_context_indices;
        GroupId parent_group_id;
    };

    struct HybridComposeCont
    {
        uint32_t var_k;
        ExtractOp op;
        GroupId parent_group_id;
    };

    struct ResidualRecombineCont
    {
        std::unique_ptr< Expr > core_expr;
        ResidualOrigin origin;
        Evaluator residual_eval;
        std::vector< uint64_t > source_sig;
        uint8_t core_degree = 0;
        std::optional< GroupId > parent_group_id;
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

    using ContinuationData = std::variant<
        std::monostate, BitwiseComposeCont, HybridComposeCont, ResidualRecombineCont,
        OperandRewriteCont, ProductCollapseCont >;

    // Projects a parent's baseline cost for a child work item
    // given the continuation that will recombine the child's result.
    std::optional< ExprCost > ProjectBaselineForChild(
        const std::optional< ExprCost > &parent_baseline, const ContinuationData &continuation
    );

} // namespace cobra
