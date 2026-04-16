#pragma once

#include "Orchestrator.h"
#include "cobra/core/Result.h"

namespace cobra {

    // Optional narrow shortcut for reduced polynomial subproblems.
    // Returns a verified direct candidate when the existing polynomial
    // passes can solve the reduced problem inline without entering the
    // signature competition worklist.
    std::optional< CandidateRecord >
    TryReducedPolynomialFastPath(const WorkItem &item, OrchestratorContext &ctx);

    Result< PassResult > RunResolveCompetition(const WorkItem &item, OrchestratorContext &ctx);

    Result< PassResult >
    RunSignaturePatternMatch(const WorkItem &item, OrchestratorContext &ctx);

    Result< PassResult > RunSignatureAnf(const WorkItem &item, OrchestratorContext &ctx);

    Result< PassResult > RunPrepareCoeffModel(const WorkItem &item, OrchestratorContext &ctx);

    Result< PassResult >
    RunSignatureCobCandidate(const WorkItem &item, OrchestratorContext &ctx);

    Result< PassResult >
    RunSignatureSingletonPolyRecovery(const WorkItem &item, OrchestratorContext &ctx);

    Result< PassResult >
    RunSignatureMultivarPolyRecovery(const WorkItem &item, OrchestratorContext &ctx);

    Result< PassResult >
    RunSignatureBitwiseDecompose(const WorkItem &item, OrchestratorContext &ctx);

    Result< PassResult >
    RunSignatureHybridDecompose(const WorkItem &item, OrchestratorContext &ctx);

} // namespace cobra
