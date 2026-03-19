#pragma once

#include "cobra/core/Expr.h"
#include "cobra/core/SemilinearIR.h"
#include <memory>
#include <vector>

namespace cobra {

    std::unique_ptr< Expr > ReconstructMaskedAtoms(
        const SemilinearIR &ir, const std::vector< PartitionClass > &partitions
    );

} // namespace cobra
