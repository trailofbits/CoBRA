#include "Orchestrator.h"
#include "OrchestratorPasses.h"
#include "cobra/core/Classification.h"
#include "cobra/core/Expr.h"
#include "cobra/core/SemilinearIR.h"
#include <gtest/gtest.h>

using namespace cobra;

namespace {

    Classification MakeClassification(
        StructuralFlag flags, SemanticClass semantic = SemanticClass::kNonPolynomial
    ) {
        return Classification{
            .semantic = semantic,
            .flags    = flags,
            .route    = DeriveRoute(flags),
        };
    }

} // namespace

// --- StateKind dispatch tests ---

TEST(StateKind, GetStateKindFromVariant) {
    StateData ast = AstPayload{ .expr = Expr::Variable(0) };
    EXPECT_EQ(GetStateKind(ast), StateKind::kFoldedAst);

    StateData sig = SignatureStatePayload{};
    EXPECT_EQ(GetStateKind(sig), StateKind::kSignatureState);

    StateData cand = CandidatePayload{ .expr = Expr::Constant(42) };
    EXPECT_EQ(GetStateKind(cand), StateKind::kCandidateExpr);
}

// --- WorkItem default value tests ---

TEST(WorkItem, DefaultValuesAreZero) {
    WorkItem item;
    item.payload = AstPayload{ .expr = Expr::Constant(0) };
    EXPECT_EQ(item.depth, 0);
    EXPECT_EQ(item.rewrite_gen, 0);
    EXPECT_EQ(item.attempted_mask, 0);
    EXPECT_TRUE(item.history.empty());
}

TEST(WorkItem, DefaultAttemptedMaskIsZero) {
    WorkItem item;
    item.payload = AstPayload{ .expr = Expr::Constant(0) };
    EXPECT_EQ(item.attempted_mask, 0);
}

TEST(Fingerprint, AttemptedMaskDistinct) {
    WorkItem a, b;
    a.payload        = AstPayload{ .expr = Expr::Constant(1) };
    a.attempted_mask = 0;
    b.payload        = AstPayload{ .expr = Expr::Constant(1) };
    b.attempted_mask = (1ULL << static_cast< uint8_t >(PassId::kExtractProductCore));
    auto fa          = ComputeFingerprint(a, 64);
    auto fb          = ComputeFingerprint(b, 64);
    EXPECT_NE(fa, fb);
}

// --- ItemMetadata default state tests ---

TEST(ItemMetadata, DefaultState) {
    ItemMetadata meta;
    EXPECT_EQ(meta.verification, VerificationState::kUnverified);
    EXPECT_EQ(meta.attempted_route, Route::kBitwiseOnly);
    EXPECT_EQ(meta.structural_transform_rounds, 0);
    EXPECT_FALSE(meta.transform_produced_candidate);
    EXPECT_FALSE(meta.candidate_failed_verification);
}

TEST(ItemMetadata, TransformTerminalSignalDefaultEmpty) {
    ItemMetadata meta;
    EXPECT_FALSE(meta.structural_transform_terminal.has_value());
    EXPECT_FALSE(meta.transform_produced_candidate);
}

// --- OrchestratorPolicy defaults ---

TEST(OrchestratorPolicy, Defaults) {
    OrchestratorPolicy policy;
    EXPECT_EQ(policy.max_expansions, 64);
    EXPECT_EQ(policy.max_rewrite_gen, 3);
    EXPECT_EQ(policy.max_candidates, 8);
}

// --- Fingerprint tests ---

TEST(Fingerprint, SameExprSameFingerprint) {
    WorkItem a, b;
    a.payload = AstPayload{ .expr = Expr::Constant(42) };
    b.payload = AstPayload{ .expr = Expr::Constant(42) };
    auto fa   = ComputeFingerprint(a, 64);
    auto fb   = ComputeFingerprint(b, 64);
    EXPECT_EQ(fa, fb);
}

TEST(Fingerprint, DifferentExprDifferentHash) {
    WorkItem a, b;
    a.payload = AstPayload{ .expr = Expr::Constant(42) };
    b.payload = AstPayload{ .expr = Expr::Constant(99) };
    auto fa   = ComputeFingerprint(a, 64);
    auto fb   = ComputeFingerprint(b, 64);
    EXPECT_NE(fa.payload_hash, fb.payload_hash);
}

TEST(Fingerprint, VerifiedVsUnverifiedCandidateDistinct) {
    WorkItem a, b;
    a.payload = CandidatePayload{
        .expr                              = Expr::Constant(1),
        .needs_original_space_verification = true,
    };
    b.payload = CandidatePayload{
        .expr                              = Expr::Constant(1),
        .needs_original_space_verification = false,
    };
    auto fa = ComputeFingerprint(a, 64);
    auto fb = ComputeFingerprint(b, 64);
    EXPECT_NE(fa.payload_hash, fb.payload_hash);
}

// --- UnsupportedRank ordering tests ---

TEST(UnsupportedRank, CandidateBeatsNonCandidate) {
    UnsupportedCandidate cand{ .last_pass          = PassId::kVerifyCandidate,
                               .is_candidate_state = true };
    UnsupportedCandidate ast{ .last_pass          = PassId::kBuildSignatureState,
                              .is_candidate_state = false };
    EXPECT_TRUE(UnsupportedRankBetter(cand, ast));
    EXPECT_FALSE(UnsupportedRankBetter(ast, cand));
}

TEST(UnsupportedRank, DeeperDepthWins) {
    UnsupportedCandidate a{ .depth = 5, .last_pass = PassId::kExtractProductCore };
    UnsupportedCandidate b{ .depth = 3, .last_pass = PassId::kExtractProductCore };
    EXPECT_TRUE(UnsupportedRankBetter(a, b));
}

// --- Worklist tests ---

TEST(Worklist, CandidatePopsFirst) {
    Worklist wl;
    WorkItem ast, cand;
    ast.payload  = AstPayload{ .expr = Expr::Constant(0) };
    cand.payload = CandidatePayload{ .expr = Expr::Constant(1) };
    wl.Push(std::move(ast));
    wl.Push(std::move(cand));
    auto first = wl.Pop();
    EXPECT_EQ(GetStateKind(first.payload), StateKind::kCandidateExpr);
}

TEST(Worklist, ShallowerDepthFirst) {
    Worklist wl;
    WorkItem deep, shallow;
    deep.payload    = AstPayload{ .expr = Expr::Constant(0) };
    deep.depth      = 5;
    shallow.payload = AstPayload{ .expr = Expr::Constant(1) };
    shallow.depth   = 1;
    wl.Push(std::move(deep));
    wl.Push(std::move(shallow));
    auto first = wl.Pop();
    EXPECT_EQ(first.depth, 1);
}

TEST(Worklist, HighWaterMark) {
    Worklist wl;
    WorkItem a, b, c;
    a.payload = AstPayload{ .expr = Expr::Constant(0) };
    b.payload = AstPayload{ .expr = Expr::Constant(1) };
    c.payload = AstPayload{ .expr = Expr::Constant(2) };
    wl.Push(std::move(a));
    wl.Push(std::move(b));
    wl.Push(std::move(c));
    EXPECT_EQ(wl.HighWaterMark(), 3);
    wl.Pop();
    EXPECT_EQ(wl.HighWaterMark(), 3); // Stays at peak
}

// --- PassAttemptCache tests ---

TEST(PassAttemptCache, RecordAndQuery) {
    PassAttemptCache cache;
    StateFingerprint fp{ .kind = StateKind::kFoldedAst };
    EXPECT_FALSE(cache.HasAttempted(fp, PassId::kExtractProductCore));
    cache.Record(fp, PassId::kExtractProductCore);
    EXPECT_TRUE(cache.HasAttempted(fp, PassId::kExtractProductCore));
    EXPECT_FALSE(cache.HasAttempted(fp, PassId::kSupportedSolve));
}

// --- SelectNextPass tests ---

TEST(SelectNextPass, FreshFoldedAstGetsBuildSigFirst) {
    WorkItem item;
    auto cls     = MakeClassification(kSfHasMixedProduct);
    item.payload = AstPayload{
        .expr           = Expr::Constant(0),
        .classification = cls,
        .provenance     = Provenance::kLowered,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kLowered;
    item.attempted_mask          = 0;

    OrchestratorPolicy policy;
    PassAttemptCache cache;
    auto pass = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(pass.has_value());
    EXPECT_EQ(*pass, PassId::kBuildSignatureState);
}

TEST(SelectNextPass, AfterBuildSigGetsPrepareDirectResidual) {
    WorkItem item;
    auto cls     = MakeClassification(kSfHasMixedProduct);
    item.payload = AstPayload{
        .expr           = Expr::Constant(0),
        .classification = cls,
        .provenance     = Provenance::kLowered,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kLowered;
    item.attempted_mask = (1ULL << static_cast< uint8_t >(PassId::kBuildSignatureState));

    OrchestratorPolicy policy;
    PassAttemptCache cache;
    auto pass = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(pass.has_value());
    EXPECT_EQ(*pass, PassId::kPrepareDirectResidual);
}

TEST(SelectNextPass, PrereqBlocksOperandSimplifyBeforeExtract) {
    WorkItem item;
    auto cls     = MakeClassification(kSfHasMixedProduct);
    item.payload = AstPayload{
        .expr           = Expr::Constant(0),
        .classification = cls,
        .provenance     = Provenance::kLowered,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kLowered;
    item.attempted_mask = (1ULL << static_cast< uint8_t >(PassId::kBuildSignatureState));

    OrchestratorPolicy policy;
    PassAttemptCache cache;
    auto pass = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(pass.has_value());
    EXPECT_EQ(*pass, PassId::kPrepareDirectResidual);
}

TEST(SelectNextPass, AllAttemptedReturnsNullopt) {
    WorkItem item;
    auto cls     = MakeClassification(kSfHasMixedProduct);
    item.payload = AstPayload{
        .expr           = Expr::Constant(0),
        .classification = cls,
        .provenance     = Provenance::kLowered,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kLowered;
    item.attempted_mask          = ~uint64_t(0);

    OrchestratorPolicy policy;
    PassAttemptCache cache;
    auto pass = SelectNextPass(item, policy, 0, cache);
    EXPECT_FALSE(pass.has_value());
}

TEST(SelectNextPass, RewriteBudgetBlocksTransforms) {
    WorkItem item;
    auto cls     = MakeClassification(kSfHasMixedProduct);
    item.payload = AstPayload{
        .expr           = Expr::Constant(0),
        .classification = cls,
        .provenance     = Provenance::kLowered,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kLowered;
    item.attempted_mask = (1ULL << static_cast< uint8_t >(PassId::kBuildSignatureState))
        | (1ULL << static_cast< uint8_t >(PassId::kPrepareDirectResidual))
        | (1ULL << static_cast< uint8_t >(PassId::kExtractProductCore))
        | (1ULL << static_cast< uint8_t >(PassId::kExtractPolyCoreD2))
        | (1ULL << static_cast< uint8_t >(PassId::kExtractTemplateCore))
        | (1ULL << static_cast< uint8_t >(PassId::kExtractPolyCoreD3))
        | (1ULL << static_cast< uint8_t >(PassId::kExtractPolyCoreD4));
    item.rewrite_gen = 3;

    OrchestratorPolicy policy;
    policy.max_rewrite_gen = 3;
    PassAttemptCache cache;
    auto pass = SelectNextPass(item, policy, 0, cache);
    EXPECT_FALSE(pass.has_value());
}

TEST(SelectNextPass, CandidateBudgetBlocksVerify) {
    WorkItem item;
    item.payload = CandidatePayload{
        .expr                              = Expr::Constant(1),
        .needs_original_space_verification = true,
    };
    item.features.provenance = Provenance::kRewritten;

    OrchestratorPolicy policy;
    policy.max_candidates = 4;
    PassAttemptCache cache;
    auto pass = SelectNextPass(item, policy, 4, cache);
    EXPECT_FALSE(pass.has_value());
}

TEST(SelectNextPass, CacheBlocksSameState) {
    WorkItem item;
    auto cls     = MakeClassification(kSfHasMixedProduct);
    item.payload = AstPayload{
        .expr           = Expr::Constant(0),
        .classification = cls,
        .provenance     = Provenance::kLowered,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kLowered;
    item.attempted_mask          = 0;

    OrchestratorPolicy policy;
    PassAttemptCache cache;
    auto fp = ComputeFingerprint(item, 64);
    cache.Record(fp, PassId::kBuildSignatureState);

    auto pass = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(pass.has_value());
    EXPECT_EQ(*pass, PassId::kPrepareDirectResidual);
}

TEST(SelectNextPass, SignatureStateGetsSupported) {
    WorkItem item;
    item.payload             = SignatureStatePayload{};
    item.features.provenance = Provenance::kOriginal;

    OrchestratorPolicy policy;
    PassAttemptCache cache;
    auto pass = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(pass.has_value());
    EXPECT_EQ(*pass, PassId::kSupportedSolve);
}

TEST(SelectNextPass, CandidateGetsVerify) {
    WorkItem item;
    item.payload = CandidatePayload{
        .expr                              = Expr::Constant(1),
        .needs_original_space_verification = true,
    };
    item.features.provenance = Provenance::kRewritten;

    OrchestratorPolicy policy;
    PassAttemptCache cache;
    auto pass = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(pass.has_value());
    EXPECT_EQ(*pass, PassId::kVerifyCandidate);
}

TEST(SelectNextPass, UnknownShapeReturnsNullopt) {
    WorkItem item;
    auto cls     = MakeClassification(kSfHasUnknownShape);
    item.payload = AstPayload{
        .expr           = Expr::Constant(0),
        .classification = cls,
        .provenance     = Provenance::kLowered,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kLowered;

    OrchestratorPolicy policy;
    PassAttemptCache cache;
    auto pass = SelectNextPass(item, policy, 0, cache);
    EXPECT_FALSE(pass.has_value());
}

TEST(SelectNextPass, NonExplorationLoweredGetsOnlyBuildSig) {
    WorkItem item;
    auto cls     = MakeClassification(kSfNone, SemanticClass::kLinear);
    item.payload = AstPayload{
        .expr           = Expr::Constant(0),
        .classification = cls,
        .provenance     = Provenance::kLowered,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kLowered;

    OrchestratorPolicy policy;
    PassAttemptCache cache;
    auto pass = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(pass.has_value());
    EXPECT_EQ(*pass, PassId::kBuildSignatureState);

    item.attempted_mask = (1ULL << static_cast< uint8_t >(PassId::kBuildSignatureState));
    pass                = SelectNextPass(item, policy, 0, cache);
    EXPECT_FALSE(pass.has_value());
}

TEST(SelectNextPass, OriginalSemilinearGetsSemilinear) {
    WorkItem item;
    auto cls     = MakeClassification(kSfNone, SemanticClass::kSemilinear);
    item.payload = AstPayload{
        .expr           = Expr::Constant(0),
        .classification = cls,
        .provenance     = Provenance::kOriginal,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kOriginal;

    OrchestratorPolicy policy;
    PassAttemptCache cache;
    auto pass = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(pass.has_value());
    EXPECT_EQ(*pass, PassId::kSemilinearNormalize);
}

TEST(SelectNextPass, CoreCandidateGetsPrepareResidual) {
    WorkItem item;
    item.payload = CoreCandidatePayload{
        .core_expr      = Expr::Constant(1),
        .extractor_kind = ExtractorKind::kProductAST,
    };
    OrchestratorPolicy policy;
    PassAttemptCache cache;
    auto pass = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(pass.has_value());
    EXPECT_EQ(*pass, PassId::kPrepareResidualFromCore);
}

TEST(SelectNextPass, ResidualStateGetsFirstSolver) {
    WorkItem item;
    item.payload = ResidualStatePayload{
        .origin          = ResidualOrigin::kProductCore,
        .is_boolean_null = false,
    };
    OrchestratorPolicy policy;
    PassAttemptCache cache;
    auto pass = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(pass.has_value());
    EXPECT_EQ(*pass, PassId::kResidualSupported);
}

TEST(SelectNextPass, BooleanNullResidualGetsGhostFirst) {
    WorkItem item;
    item.payload = ResidualStatePayload{
        .origin          = ResidualOrigin::kDirectBooleanNull,
        .is_boolean_null = true,
    };
    OrchestratorPolicy policy;
    PassAttemptCache cache;
    auto pass = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(pass.has_value());
    EXPECT_EQ(*pass, PassId::kResidualGhost);
}

TEST(SelectNextPass, CoreDerivedBooleanNullGetsPolyFirst) {
    WorkItem item;
    item.payload = ResidualStatePayload{
        .origin          = ResidualOrigin::kProductCore,
        .is_boolean_null = true,
    };
    OrchestratorPolicy policy;
    PassAttemptCache cache;
    auto pass = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(pass.has_value());
    EXPECT_EQ(*pass, PassId::kResidualPolyRecovery);
}

TEST(StateKind, SemilinearStateKinds) {
    SemilinearIR ir;
    ir.bitwidth = 64;

    StateData norm = NormalizedSemilinearPayload{
        .ctx = SemilinearContext{ .ir = std::move(ir) },
    };
    EXPECT_EQ(GetStateKind(norm), StateKind::kSemilinearNormalizedIr);

    SemilinearIR ir2;
    ir2.bitwidth      = 64;
    StateData checked = CheckedSemilinearPayload{
        .ctx = SemilinearContext{ .ir = std::move(ir2) },
    };
    EXPECT_EQ(GetStateKind(checked), StateKind::kSemilinearCheckedIr);

    SemilinearIR ir3;
    ir3.bitwidth        = 64;
    StateData rewritten = RewrittenSemilinearPayload{
        .ctx = SemilinearContext{ .ir = std::move(ir3) },
    };
    EXPECT_EQ(GetStateKind(rewritten), StateKind::kSemilinearRewrittenIr);
}

TEST(Fingerprint, SemilinearSameIrSameHash) {
    auto make_ir = []() {
        SemilinearIR ir;
        ir.constant = 42;
        ir.bitwidth = 64;
        return ir;
    };
    WorkItem a;
    WorkItem b;
    a.payload = NormalizedSemilinearPayload{
        .ctx = SemilinearContext{ .ir = make_ir() },
    };
    b.payload = NormalizedSemilinearPayload{
        .ctx = SemilinearContext{ .ir = make_ir() },
    };
    auto fa = ComputeFingerprint(a, 64);
    auto fb = ComputeFingerprint(b, 64);
    EXPECT_EQ(fa, fb);
}

TEST(Fingerprint, SemilinearDifferentStateKindDifferentFingerprint) {
    auto make_ir = []() {
        SemilinearIR ir;
        ir.constant = 42;
        ir.bitwidth = 64;
        return ir;
    };
    WorkItem a;
    WorkItem b;
    a.payload = NormalizedSemilinearPayload{
        .ctx = SemilinearContext{ .ir = make_ir() },
    };
    b.payload = CheckedSemilinearPayload{
        .ctx = SemilinearContext{ .ir = make_ir() },
    };
    auto fa = ComputeFingerprint(a, 64);
    auto fb = ComputeFingerprint(b, 64);
    EXPECT_NE(fa, fb);
}

TEST(SelectNextPass, NormalizedSemilinearGetsCheck) {
    WorkItem item;
    SemilinearIR ir;
    ir.bitwidth  = 64;
    item.payload = NormalizedSemilinearPayload{
        .ctx = SemilinearContext{ .ir = std::move(ir) },
    };
    OrchestratorPolicy policy;
    PassAttemptCache cache;
    auto pass = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(pass.has_value());
    EXPECT_EQ(*pass, PassId::kSemilinearCheck);
}

TEST(SelectNextPass, CheckedSemilinearGetsRewrite) {
    WorkItem item;
    SemilinearIR ir;
    ir.bitwidth  = 64;
    item.payload = CheckedSemilinearPayload{
        .ctx = SemilinearContext{ .ir = std::move(ir) },
    };
    OrchestratorPolicy policy;
    PassAttemptCache cache;
    auto pass = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(pass.has_value());
    EXPECT_EQ(*pass, PassId::kSemilinearRewrite);
}

TEST(SelectNextPass, RewrittenSemilinearGetsReconstruct) {
    WorkItem item;
    SemilinearIR ir;
    ir.bitwidth  = 64;
    item.payload = RewrittenSemilinearPayload{
        .ctx = SemilinearContext{ .ir = std::move(ir) },
    };
    OrchestratorPolicy policy;
    PassAttemptCache cache;
    auto pass = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(pass.has_value());
    EXPECT_EQ(*pass, PassId::kSemilinearReconstruct);
}

// --- SignatureSubproblemContext round-trip through SignatureStatePayload ---

TEST(SignatureSubproblemContext, RoundTripThroughSignatureStatePayload) {
    SignatureSubproblemContext ctx;
    ctx.sig                               = { 1, 2, 3 };
    ctx.real_vars                         = { "x0", "x1" };
    ctx.original_indices                  = { 0, 1 };
    ctx.needs_original_space_verification = true;

    SignatureStatePayload payload{ .ctx = std::move(ctx) };
    EXPECT_EQ(payload.ctx.sig.size(), 3);
    EXPECT_EQ(payload.ctx.sig[0], 1);
    EXPECT_EQ(payload.ctx.sig[2], 3);
    EXPECT_EQ(payload.ctx.real_vars.size(), 2);
    EXPECT_EQ(payload.ctx.real_vars[0], "x0");
    EXPECT_EQ(payload.ctx.original_indices.size(), 2);
    EXPECT_TRUE(payload.ctx.needs_original_space_verification);
}

// --- SignatureCoeffStatePayload fingerprint differs from SignatureStatePayload ---

TEST(Fingerprint, SignatureCoeffDiffersFromSignatureState) {
    std::vector< uint64_t > sig     = { 10, 20, 30 };
    std::vector< std::string > vars = { "x0" };

    WorkItem sig_item;
    sig_item.payload = SignatureStatePayload{
        .ctx = { .sig = sig, .real_vars = vars },
    };

    WorkItem coeff_item;
    coeff_item.payload = SignatureCoeffStatePayload{
        .ctx    = { .sig = sig, .real_vars = vars },
        .coeffs = { 5 },
    };

    auto fp_sig   = ComputeFingerprint(sig_item, 64);
    auto fp_coeff = ComputeFingerprint(coeff_item, 64);
    // Different StateKind guarantees different fingerprints
    EXPECT_NE(fp_sig, fp_coeff);
    // Also verify the payload hashes differ (coeffs included)
    EXPECT_NE(fp_sig.payload_hash, fp_coeff.payload_hash);
}

// --- kSignatureCoeffState routes to band 1 ---

TEST(StateKind, SignatureCoeffStateKind) {
    StateData data = SignatureCoeffStatePayload{
        .ctx    = { .sig = { 1 } },
        .coeffs = { 42 },
    };
    EXPECT_EQ(GetStateKind(data), StateKind::kSignatureCoeffState);
}

TEST(SelectNextPass, SignatureCoeffStateReturnsNullopt) {
    WorkItem item;
    item.payload = SignatureCoeffStatePayload{
        .ctx    = { .sig = { 1, 2 } },
        .coeffs = { 10 },
    };
    OrchestratorPolicy policy;
    PassAttemptCache cache;
    auto pass = SelectNextPass(item, policy, 0, cache);
    EXPECT_FALSE(pass.has_value());
}

TEST(Worklist, SignatureCoeffStatePopsAfterCandidate) {
    Worklist wl;
    WorkItem coeff_item;
    coeff_item.payload = SignatureCoeffStatePayload{
        .ctx    = { .sig = { 1 } },
        .coeffs = { 7 },
    };
    WorkItem cand_item;
    cand_item.payload = CandidatePayload{ .expr = Expr::Constant(1) };

    wl.Push(std::move(coeff_item));
    wl.Push(std::move(cand_item));
    auto first = wl.Pop();
    EXPECT_EQ(GetStateKind(first.payload), StateKind::kCandidateExpr);
}

// --- WorkItem default fields for competition ---

TEST(WorkItem, DefaultCompetitionFields) {
    WorkItem item;
    item.payload = AstPayload{ .expr = Expr::Constant(0) };
    EXPECT_EQ(item.signature_recursion_depth, 0);
    EXPECT_FALSE(item.group_id.has_value());
}
