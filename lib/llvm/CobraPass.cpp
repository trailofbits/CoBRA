#include "cobra/llvm/CobraPass.h"
#include "IRReconstructor.h"
#include "MBADetector.h"
#include "cobra/core/Simplifier.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Compiler.h"
#if LLVM_VERSION_MAJOR >= 22
    #include "llvm/Plugins/PassPlugin.h"
#else
    #include "llvm/Passes/PassPlugin.h"
#endif
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "cobra"

namespace cobra {

    llvm::PreservedAnalyses
    CobraPass::run(llvm::Function &f, llvm::FunctionAnalysisManager &AM) {
        bool changed = false;

        for (auto &bb : f) {
            auto candidates = DetectMbaCandidates(bb, min_ast_size_, max_vars_);

            for (auto &cand : candidates) {
                const Options opts{ .bitwidth   = cand.bitwidth,
                                    .max_vars   = max_vars_,
                                    .spot_check = true };

                auto result = Simplify(cand.sig, cand.var_names, nullptr, opts);
                if (!result.has_value()) {
                    LLVM_DEBUG(
                        llvm::dbgs()
                        << "CoBRA: skipping candidate: " << result.error().message << "\n"
                    );
                    continue;
                }

                llvm::IRBuilder<> builder(cand.root);
                auto *new_val = ReconstructIr(*result.value().expr, cand, builder);

                cand.root->replaceAllUsesWith(new_val);
                changed = true;

                LLVM_DEBUG(
                    llvm::dbgs()
                    << "CoBRA: simplified to "
                    << Render(*result.value().expr, result.value().real_vars, cand.bitwidth)
                    << "\n"
                );
            }
        }

        return changed ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
    }

} // namespace cobra

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return { .APIVersion                   = LLVM_PLUGIN_API_VERSION,
             .PluginName                   = "CobraPass",
             .PluginVersion                = LLVM_VERSION_STRING,
             .RegisterPassBuilderCallbacks = [](llvm::PassBuilder &pb) {
                 pb.registerPipelineParsingCallback(
                     [](llvm::StringRef name, llvm::FunctionPassManager &fpm,
                        llvm::ArrayRef< llvm::PassBuilder::PipelineElement >) {
                         if (name == "cobra-simplify") {
                             fpm.addPass(cobra::CobraPass());
                             return true;
                         }
                         return false;
                     }
                 );
             } };
}
