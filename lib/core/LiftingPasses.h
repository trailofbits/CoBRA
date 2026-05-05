#pragma once

#include "OrchestratorPasses.h"

namespace cobra {

    Result< PassResult > RunLiftArithmeticAtoms(const WorkItem &item, OrchestratorContext &ctx);

    Result< PassResult >
    RunLiftRepeatedSubexpressions(const WorkItem &item, OrchestratorContext &ctx);

} // namespace cobra
