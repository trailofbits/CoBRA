#pragma once

#include "Orchestrator.h"
#include "cobra/core/Result.h"

namespace cobra {

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

} // namespace cobra
