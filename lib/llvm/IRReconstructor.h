#pragma once

#include "MBADetector.h"
#include "cobra/core/Expr.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Value.h"

#include <cstdint>
#include <vector>

namespace cobra {

    // Convert a simplified Expr tree back to LLVM IR instructions.
    // var_map remaps simplified variable indices to original leaf indices
    // (needed when aux variable elimination reduced the variable set).
    // If empty, identity mapping is assumed.
    llvm::Value *ReconstructIr(
        const Expr &expr, const MBACandidate &candidate, llvm::IRBuilder<> &builder,
        const std::vector< uint32_t > &var_map = {}
    );

} // namespace cobra
