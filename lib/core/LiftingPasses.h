#pragma once

#include "OrchestratorPasses.h"

namespace cobra {

    Result< PassResult > RunLiftArithmeticAtoms(const WorkItem &item, OrchestratorContext &ctx);

    Result< PassResult >
    RunLiftRepeatedSubexpressions(const WorkItem &item, OrchestratorContext &ctx);

    /// Evaluate an Expr tree at a full-width input point.
    uint64_t
    EvaluateExpr(const Expr &e, const std::vector< uint64_t > &vals, uint32_t bitwidth);

} // namespace cobra
