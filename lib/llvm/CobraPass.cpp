#include "cobra/llvm/CobraPass.h"
#include "IRReconstructor.h"
#include "MBADetector.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/Simplifier.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
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

#include <cstdint>
#include <string>
#include <vector>

#define DEBUG_TYPE "cobra"

namespace cobra {

    llvm::PreservedAnalyses
    CobraPass::run(llvm::Function &f, llvm::FunctionAnalysisManager &AM) {
        bool changed = false;

        for (auto &bb : f) {
            auto candidates = DetectMbaCandidates(bb, min_ast_size_, max_vars_);

            for (auto &cand : candidates) {
                Options opts{ .bitwidth   = cand.bitwidth,
                              .max_vars   = max_vars_,
                              .spot_check = true,
                              .evaluator  = cand.evaluator };

                // Pass AST when available — unlocks semilinear,
                // MixedRewrite, and decomposition pipelines.
                const Expr *ast = cand.expr.get();

                auto result = Simplify(cand.sig, cand.var_names, ast, opts);
                if (!result.has_value()) {
                    LLVM_DEBUG(
                        llvm::dbgs()
                        << "CoBRA: skipping candidate: " << result.error().message << "\n"
                    );
                    continue;
                }

                if (result.value().kind != SimplifyOutcome::Kind::kSimplified) {
                    LLVM_DEBUG(
                        llvm::dbgs()
                        << "CoBRA: not simplified: " << result.value().diag.reason << "\n"
                    );
                    continue;
                }

                // Cost gate: don't replace if simplified form is not
                // smaller. nuw/nsw flags are intentionally dropped —
                // CoBRA's Expr model is modular arithmetic and we
                // cannot soundly preserve wrapping guarantees.
                if (cand.expr != nullptr) {
                    auto original_cost   = ComputeCost(*cand.expr);
                    auto simplified_cost = ComputeCost(*result.value().expr);
                    if (!IsBetter(simplified_cost.cost, original_cost.cost)) {
                        LLVM_DEBUG(
                            llvm::dbgs() << "CoBRA: skipping — simplified form is not smaller\n"
                        );
                        continue;
                    }
                }

                // Build variable index map for aux var elimination.
                // real_vars may be a subset of var_names with
                // reindexed positions.
                std::vector< uint32_t > var_map;
                const auto &real_vars = result.value().real_vars;
                if (!real_vars.empty() && real_vars.size() != cand.var_names.size()) {
                    var_map.reserve(real_vars.size());
                    for (const auto &rv : real_vars) {
                        for (uint32_t j = 0; j < cand.var_names.size(); ++j) {
                            if (cand.var_names[j] == rv) {
                                var_map.push_back(j);
                                break;
                            }
                        }
                    }
                }

                llvm::IRBuilder<> builder(cand.root);
                auto *new_val = ReconstructIr(*result.value().expr, cand, builder, var_map);

                cand.root->replaceAllUsesWith(new_val);
                changed = true;

                LLVM_DEBUG(
                    llvm::dbgs()
                    << "CoBRA: simplified to "
                    << Render(*result.value().expr, result.value().real_vars, cand.bitwidth)
                    << "\n"
                );
            }

            // DCE: iteratively erase dead instructions from replaced
            // trees. Needs multiple passes because erasing %mul may
            // make its operand %and dead.
            if (changed) {
                bool erased = true;
                while (erased) {
                    erased = false;
                    llvm::SmallVector< llvm::Instruction *, 16 > dead;
                    for (auto &inst : bb) {
                        if (inst.use_empty() && !inst.isTerminator()
                            && !inst.mayHaveSideEffects())
                        {
                            dead.push_back(&inst);
                        }
                    }
                    for (auto *inst : dead) {
                        inst->eraseFromParent();
                        erased = true;
                    }
                }
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
