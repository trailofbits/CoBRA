#pragma once

#include "cobra/core/SemilinearIR.h"

namespace cobra {

    /// XOR recovery + mask elimination (MSiMBA Section 5.3).
    /// Detects complementary-mask pairs in basis groups and recovers
    /// XOR atoms or eliminates masks as appropriate.
    void RecoverStructure(SemilinearIR &ir);

    /// Coalesce terms by per-bit effective coefficient (MSiMBA Section 5.3).
    /// For single-variable basis groups, computes per-bit effective
    /// coefficients and re-partitions to minimize term count.
    void CoalesceTerms(SemilinearIR &ir);

} // namespace cobra
