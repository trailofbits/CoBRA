#pragma once

#include "llvm/IR/PassManager.h"

namespace cobra {

    class CobraPass : public llvm::PassInfoMixin< CobraPass >
    {
      public:
        explicit CobraPass(uint32_t max_vars = 12, uint32_t min_ast_size = 4)
            : max_vars_(max_vars), min_ast_size_(min_ast_size) {}

        // NOLINTNEXTLINE(readability-identifier-naming) - LLVM PassInfoMixin requires 'run'
        llvm::PreservedAnalyses run(llvm::Function &f, llvm::FunctionAnalysisManager &am);

        static bool isRequired() {
            return false;
        } // NOLINT(readability-identifier-naming) - LLVM interface

      private:
        uint32_t max_vars_;
        uint32_t min_ast_size_;
    };

} // namespace cobra
