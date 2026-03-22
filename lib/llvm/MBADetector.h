#pragma once

#include "cobra/core/Expr.h"
#include "cobra/core/Simplifier.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

namespace cobra {

    struct MBACandidate
    {
        llvm::Instruction *root;
        std::vector< llvm::Value * > leaf_values;
        std::vector< std::string > var_names;
        std::vector< uint64_t > sig;
        uint32_t bitwidth;
        std::unique_ptr< Expr > expr;
        Evaluator evaluator;
    };

    // Find MBA candidates in a basic block.
    std::vector< MBACandidate >
    DetectMbaCandidates(llvm::BasicBlock &bb, uint32_t min_ast_size, uint32_t max_vars);

} // namespace cobra
