#pragma once

#include "cobra/core/SemilinearIR.h"
#include <vector>

namespace cobra {

    std::vector< PartitionClass > ComputePartitions(const SemilinearIR &ir);

} // namespace cobra
