#include "DecompositionPassHelpers.h"
#include "cobra/core/SignatureEval.h"

namespace cobra {

    std::vector< uint64_t > ComputeDecompositionSignature(
        const AstPayload &ast, const OrchestratorContext &ctx, uint32_t rewrite_gen
    ) {
        const auto num_vars = static_cast< uint32_t >(ctx.original_vars.size());
        bool use_input_sig  = !ctx.input_sig.empty() && !ctx.lowering_fired && rewrite_gen == 0;
        if (use_input_sig) { return ctx.input_sig; }
        return EvaluateBooleanSignature(*ast.expr, num_vars, ctx.bitwidth);
    }

} // namespace cobra
