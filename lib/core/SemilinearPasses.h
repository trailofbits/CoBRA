#pragma once

#include "Orchestrator.h"
#include "cobra/core/Result.h"
#include "cobra/core/SemilinearIR.h"

#include <cstdint>

namespace cobra {

    namespace semilinear_pass {
        enum Subcode : uint16_t {
            kTooManyVars      = 1,
            kNormalizeFailed  = 2,
            kSelfCheckFailed  = 3,
            kPostRewriteProbe = 4,
            kFinalVerifyFail  = 5,
        };
    } // namespace semilinear_pass

    std::unique_ptr< Expr >
    CanonicalizeScaledBooleanSum(std::unique_ptr< Expr > expr, uint32_t bitwidth);

    std::unique_ptr< Expr >
    NormalizeLateCandidateExpr(std::unique_ptr< Expr > expr, uint32_t bitwidth);

    SemilinearIR CloneSemilinearIR(const SemilinearIR &src);

    Result< PassResult > RunSemilinearNormalize(const WorkItem &, OrchestratorContext &);
    Result< PassResult > RunSemilinearCheck(const WorkItem &, OrchestratorContext &);
    Result< PassResult > RunSemilinearRewrite(const WorkItem &, OrchestratorContext &);
    Result< PassResult > RunSemilinearReconstruct(const WorkItem &, OrchestratorContext &);

} // namespace cobra
