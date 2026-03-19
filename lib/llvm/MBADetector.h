#pragma once

#include <cstdint>
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
    };

    // Find MBA candidates in a basic block.
    std::vector< MBACandidate >
    DetectMbaCandidates(llvm::BasicBlock &bb, uint32_t min_ast_size, uint32_t max_vars);

} // namespace cobra
