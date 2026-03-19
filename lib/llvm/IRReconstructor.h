#pragma once

#include "MBADetector.h"
#include "cobra/core/Expr.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Value.h"

namespace cobra {

    // Convert a simplified Expr tree back to LLVM IR instructions.
    llvm::Value *
    ReconstructIr(const Expr &expr, const MBACandidate &candidate, llvm::IRBuilder<> &builder);

} // namespace cobra
