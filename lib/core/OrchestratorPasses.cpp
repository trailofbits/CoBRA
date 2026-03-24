#include "OrchestratorPasses.h"

namespace cobra {

    const std::vector< PassDescriptor > &GetPassRegistry() {
        static const std::vector< PassDescriptor > kRegistry;
        return kRegistry;
    }

    // Stub adapters — all return kNotApplicable for now

    Result< PassResult >
    RunLowerNotOverArith(const WorkItem & /*item*/, OrchestratorContext & /*ctx*/) {
        return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
    }

    Result< PassResult >
    RunClassifyAst(const WorkItem & /*item*/, OrchestratorContext & /*ctx*/) {
        return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
    }

    Result< PassResult >
    RunBuildSignatureState(const WorkItem & /*item*/, OrchestratorContext & /*ctx*/) {
        return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
    }

    Result< PassResult >
    RunSupportedSolve(const WorkItem & /*item*/, OrchestratorContext & /*ctx*/) {
        return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
    }

    Result< PassResult >
    RunTrySemilinearPass(const WorkItem & /*item*/, OrchestratorContext & /*ctx*/) {
        return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
    }

    Result< PassResult >
    RunDecompose(const WorkItem & /*item*/, OrchestratorContext & /*ctx*/) {
        return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
    }

    Result< PassResult >
    RunOperandSimplify(const WorkItem & /*item*/, OrchestratorContext & /*ctx*/) {
        return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
    }

    Result< PassResult >
    RunProductIdentityCollapse(const WorkItem & /*item*/, OrchestratorContext & /*ctx*/) {
        return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
    }

    Result< PassResult >
    RunXorLowering(const WorkItem & /*item*/, OrchestratorContext & /*ctx*/) {
        return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
    }

    Result< PassResult >
    RunVerifyCandidate(const WorkItem & /*item*/, OrchestratorContext & /*ctx*/) {
        return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
    }

} // namespace cobra
