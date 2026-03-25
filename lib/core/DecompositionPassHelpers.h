#pragma once

#include "Orchestrator.h"
#include "cobra/core/Result.h"

#include <cstdint>
#include <vector>

namespace cobra {

    namespace decomposition {
        enum Subcode : uint16_t {
            kNoExpr           = 1,
            kNoProducts       = 2,
            kNoEvaluator      = 3,
            kTooManyVars      = 4,
            kPolyRecovFailed  = 5,
            kExprBuildFailed  = 6,
            kNoTemplateMatch  = 7,
            kCoreRejected     = 10,
            kDirectFwFailed   = 11,
            kResidualFailed   = 12,
            kAllSolversFailed = 13,
            kLoopExhausted    = 20,
        };
    } // namespace decomposition

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

    // Decomposition residual prep passes: prepare ResidualStatePayload
    // for downstream solver passes.
    Result< PassResult > RunPrepareDirectResidual(const WorkItem &, OrchestratorContext &);
    Result< PassResult > RunPrepareResidualFromCore(const WorkItem &, OrchestratorContext &);

    // Decomposition residual solver passes: each consumes a
    // ResidualStatePayload, calls a solver, recombines with the core,
    // and emits a CandidatePayload on success.
    Result< PassResult > RunResidualSupported(const WorkItem &, OrchestratorContext &);
    Result< PassResult > RunResidualPolyRecovery(const WorkItem &, OrchestratorContext &);
    Result< PassResult > RunResidualGhost(const WorkItem &, OrchestratorContext &);
    Result< PassResult > RunResidualFactoredGhost(const WorkItem &, OrchestratorContext &);
    Result< PassResult >
    RunResidualFactoredGhostEscalated(const WorkItem &, OrchestratorContext &);
    Result< PassResult > RunResidualTemplate(const WorkItem &, OrchestratorContext &);

} // namespace cobra
