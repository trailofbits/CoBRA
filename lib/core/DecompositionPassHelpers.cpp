#include "DecompositionPassHelpers.h"
#include "OrchestratorPasses.h"
#include "cobra/core/SignatureEval.h"

namespace cobra {

    std::vector< uint64_t > ComputeDecompositionSignature(
        const WorkItem &item, const OrchestratorContext &ctx, uint32_t rewrite_gen
    ) {
        const auto &ast         = std::get< AstPayload >(item.payload);
        const auto *active_sig  = ActiveAstInputSig(item, ctx);
        const auto &active_vars = ActiveAstVars(item, ctx);
        const auto num_vars     = static_cast< uint32_t >(active_vars.size());
        bool use_input_sig = active_sig != nullptr && !ctx.lowering_fired && rewrite_gen == 0;
        if (use_input_sig) { return *active_sig; }
        return EvaluateBooleanSignature(*ast.expr, num_vars, ctx.bitwidth);
    }

} // namespace cobra
