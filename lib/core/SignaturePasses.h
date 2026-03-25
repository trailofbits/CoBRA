#pragma once

#include "Orchestrator.h"
#include "cobra/core/Result.h"

namespace cobra {

    Result< PassResult > RunResolveCompetition(const WorkItem &item, OrchestratorContext &ctx);

} // namespace cobra
