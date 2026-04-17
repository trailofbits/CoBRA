add_library(cobra-core STATIC)
set_target_properties(cobra-core PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_sources(cobra-core PRIVATE
    CompiledExpr.cpp
    Expr.cpp
    SignatureVector.cpp
    AuxVarEliminator.cpp
    AnfTransform.cpp
    AnfCleanup.cpp
    PatternMatcher.cpp
    CoeffInterpolator.cpp
    SignatureChecker.cpp
    Simplifier.cpp
    CoBExprBuilder.cpp
    SemilinearIR.cpp
    SemilinearNormalizer.cpp
    Classifier.cpp
    AtomSimplifier.cpp
    ExprUtils.cpp
    BitPartitioner.cpp
    MaskedAtomReconstructor.cpp
    TermRefiner.cpp
    SelfCheck.cpp
    CoefficientSplitter.cpp
    ArithmeticLowering.cpp
    PolyNormalizer.cpp
    PolyExprBuilder.cpp
    BasisTransform.cpp
    NullPolyGenerator.cpp
    SingletonPowerRecovery.cpp
    SingletonPowerExprBuilder.cpp
    MultivarPolyRecovery.cpp
    SignatureEval.cpp
    DynamicMask.cpp
    MixedProductRewriter.cpp
    ExprCost.cpp
    ExtensionLowering.cpp
    SignatureSimplifier.cpp
    BitwiseDecomposer.cpp
    HybridDecomposer.cpp
    TemplateDecomposer.cpp
    DecompositionEngine.cpp
    GhostBasis.cpp
    GhostResidualSolver.cpp
    WeightedPolyFit.cpp
    SemilinearSignature.cpp
    StructureRecovery.cpp
    PassContract.cpp
    CompetitionGroup.cpp
    JoinState.cpp
    Orchestrator.cpp
    OrchestratorPasses.cpp
    DecompositionPassHelpers.cpp
    DecompositionPasses.cpp
    SemilinearPasses.cpp
    SignaturePasses.cpp
    VerifyCandidate.cpp
    LiftingPasses.cpp
)

target_include_directories(cobra-core PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

find_package(absl REQUIRED)
find_package(hwy REQUIRED CONFIG)
target_link_libraries(cobra-core PUBLIC absl::flat_hash_map PRIVATE hwy::hwy)
