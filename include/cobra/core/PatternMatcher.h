#pragma once

#include "cobra/core/Expr.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace cobra {

    // Attempt fast-path recognition of common signature vectors.
    // Returns the simplified Expr if matched, std::nullopt if not.
    // This is an optimization — unmatched signatures fall through to CoB.
    std::optional< std::unique_ptr< Expr > >
    MatchPattern(const std::vector< uint64_t > &sig, uint32_t num_vars, uint32_t bitwidth);

    // Recursively simplify small-support subtrees via MatchPattern, but only
    // when the replacement is strictly cheaper and passes full-width
    // equivalence against the original subtree.
    std::unique_ptr< Expr >
    SimplifyPatternSubtrees(std::unique_ptr< Expr > expr, uint32_t bitwidth);

} // namespace cobra
