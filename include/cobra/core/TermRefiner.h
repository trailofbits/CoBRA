#pragma once

#include "cobra/core/SemilinearIR.h"
#include <cstdint>

namespace cobra {

    /// Returns true if `old_coeff * (mask & x)` is semantically equivalent to
    /// `new_coeff * (mask & x)` for all x, modulo 2^bitwidth.
    /// (MSiMBA Section 5.1)
    bool CanChangeCoefficientTo(
        uint64_t old_coeff, uint64_t new_coeff, uint64_t bitmask, uint32_t bitwidth
    );

    /// Returns true if `coeff * (old_mask & x)` is semantically equivalent to
    /// `coeff * (new_mask & x)` for all x, modulo 2^bitwidth.
    /// (MSiMBA Section 5.1)
    bool
    CanChangeMaskTo(uint64_t coeff, uint64_t old_mask, uint64_t new_mask, uint32_t bitwidth);

    /// Compute the minimal bitmask such that
    /// `coeff * (mask & x)` ≡ `coeff * (reduced & x)` for all x.
    /// Strips bits whose contribution is zeroed by the coefficient.
    uint64_t ReduceMask(uint64_t coeff, uint64_t mask, uint32_t bitwidth);

    /// Run the Section 5.2 refinement pass on a semilinear IR.
    /// Groups atoms by basis expression and applies:
    ///   1. Mask reduction (strip dead bits)
    ///   2. Zero-term elimination (discard coefficient-killed terms)
    ///   3. Disjoint-mask merging (same coefficient)
    ///   4. Coefficient matching + merge (CanChangeCoefficientTo)
    ///   5. Coefficient-to-minus-one normalization
    ///   6. Three-term collapse (m1+m2=m3 with disjoint masks)
    void RefineTerms(SemilinearIR &ir);

} // namespace cobra
