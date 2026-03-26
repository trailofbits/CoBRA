#pragma once

#include "Orchestrator.h"
#include "cobra/core/Result.h"

#include <vector>

namespace cobra {

    // ---------------------------------------------------------------
    // PassId — identifies each pass in the registry
    // ---------------------------------------------------------------

    enum class PassId : uint8_t {
        kLowerNotOverArith,
        kClassifyAst,
        kBuildSignatureState,
        kSupportedSolve,
        // Semilinear passes
        kSemilinearNormalize,
        kSemilinearCheck,
        kSemilinearRewrite,
        kSemilinearReconstruct,
        // Decomposition extractors
        kExtractProductCore,
        kExtractPolyCoreD2,
        kExtractTemplateCore,
        kExtractPolyCoreD3,
        kExtractPolyCoreD4,
        // Decomposition residual prep
        kPrepareDirectResidual,
        kPrepareResidualFromCore,
        // Decomposition residual solvers
        kResidualSupported,
        kResidualPolyRecovery,
        kResidualGhost,
        kResidualFactoredGhost,
        kResidualFactoredGhostEscalated,
        kResidualTemplate,
        // Competition resolution
        kResolveCompetition,
        // Signature technique passes
        kSignaturePatternMatch,
        kSignatureAnf,
        kPrepareCoeffModel,
        kSignatureCobCandidate,
        kSignatureSingletonPolyRecovery,
        kSignatureMultivarPolyRecovery,
        kSignatureBitwiseDecompose,
        kSignatureHybridDecompose,
        // Structural rewrites
        kOperandSimplify,
        kProductIdentityCollapse,
        kXorLowering,
        kVerifyCandidate,
    };

    inline bool IsDecompositionFamilyPass(PassId id) {
        return id >= PassId::kExtractProductCore && id <= PassId::kResidualTemplate;
    }

    // ---------------------------------------------------------------
    // PassTag — broad category for each pass
    // ---------------------------------------------------------------

    enum class PassTag {
        kAnalysis,
        kRewrite,
        kSolver,
        kVerifier,
    };

    // ---------------------------------------------------------------
    // Function pointer types for pass dispatch
    // ---------------------------------------------------------------

    using ApplicabilityFn = bool (*)(const WorkItem &, const OrchestratorContext &);
    using PassFn          = Result< PassResult > (*)(const WorkItem &, OrchestratorContext &);

    // ---------------------------------------------------------------
    // PassDescriptor — static metadata for one pass
    // ---------------------------------------------------------------

    struct PassDescriptor
    {
        PassId id;
        StateKind consumes;
        PassTag tag;
        ApplicabilityFn applicable;
        PassFn run;
    };

    // ---------------------------------------------------------------
    // Registry
    // ---------------------------------------------------------------

    const std::vector< PassDescriptor > &GetPassRegistry();

    // ---------------------------------------------------------------
    // Pass adapter declarations
    // ---------------------------------------------------------------

    Result< PassResult > RunLowerNotOverArith(const WorkItem &, OrchestratorContext &);
    Result< PassResult > RunClassifyAst(const WorkItem &, OrchestratorContext &);
    Result< PassResult > RunBuildSignatureState(const WorkItem &, OrchestratorContext &);
    Result< PassResult > RunSupportedSolve(const WorkItem &, OrchestratorContext &);

    Result< PassResult > RunOperandSimplify(const WorkItem &, OrchestratorContext &);
    Result< PassResult > RunProductIdentityCollapse(const WorkItem &, OrchestratorContext &);
    Result< PassResult > RunXorLowering(const WorkItem &, OrchestratorContext &);
    Result< PassResult > RunVerifyCandidate(const WorkItem &, OrchestratorContext &);

} // namespace cobra
