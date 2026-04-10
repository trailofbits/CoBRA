#pragma once

// Root-level contiguous low-bit mask detection for dynamic masking.
//
// Detects expressions of the form  (2^m - 1) & g  at the AST root.
// When found, g can be solved under bitwidth=m instead of the full
// width, provided g contains no right-shift nodes (which break the
// modular arithmetic homomorphism).

#include "cobra/core/Expr.h"

#include <cstdint>
#include <optional>

namespace cobra {

    struct MaskInfo
    {
        uint32_t effective_width; // m where mask = 2^m - 1
        const Expr *inner;        // pointer into the original AST
    };

    // Returns m if val == 2^m - 1 for some m in [1, 63].
    std::optional< uint32_t > IsPowerOfTwoMinusOne(uint64_t val);

    // Detects root-level And(g, 2^m-1) or And(2^m-1, g) where
    // m < bitwidth. Returns the effective width and a pointer to
    // the inner expression g.
    std::optional< MaskInfo > DetectRootLowBitMask(const Expr &expr, uint32_t bitwidth);

    // Returns true if any node in the AST is kShr.
    bool ContainsShr(const Expr &expr);

} // namespace cobra
