#pragma once

#include "Orchestrator.h"
#include "cobra/core/Result.h"

namespace cobra {

    // Optional narrow shortcut for reduced polynomial subproblems.
    // Returns a verified direct candidate when the existing polynomial
    // passes can solve the reduced problem inline without entering the
    // signature competition worklist.
    //
    // Coverage: num_vars == 1 (univariate fast-path) and 2 <= num_vars <= 4
    // (multivar fast-path). num_vars in [5, 6] falls through to the worklist
    // pipeline (RunSignatureMultivarPolyRecovery), which handles those vars
    // with full classification context. Widening the fast-path to 5-6 vars
    // would require matching that envelope here.
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
