#pragma once

#include "Orchestrator.h"
#include "cobra/core/Result.h"

#include <cstdint>
#include <vector>

namespace cobra {

    // Compute the Boolean signature to feed into decomposition.
    // Uses the parser-computed input_sig when the expression is fresh
    // (rewrite_gen == 0 and lowering has not fired). Otherwise re-evaluates
    // from the AST, since the stored signature would be stale.
    std::vector< uint64_t > ComputeDecompositionSignature(
        const AstPayload &ast, const OrchestratorContext &ctx, uint32_t rewrite_gen
    );

    // Decomposition extractor passes: each extracts an arithmetic core
    // from an MBA expression using a different strategy.
    Result< PassResult > RunExtractProductCore(const WorkItem &, OrchestratorContext &);
    Result< PassResult > RunExtractPolyCoreD2(const WorkItem &, OrchestratorContext &);
    Result< PassResult > RunExtractTemplateCore(const WorkItem &, OrchestratorContext &);
    Result< PassResult > RunExtractPolyCoreD3(const WorkItem &, OrchestratorContext &);
    Result< PassResult > RunExtractPolyCoreD4(const WorkItem &, OrchestratorContext &);

} // namespace cobra
