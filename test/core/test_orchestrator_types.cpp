#include "Orchestrator.h"
#include "OrchestratorPasses.h"
#include "cobra/core/Classification.h"
#include "cobra/core/Expr.h"
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

TEST(SelectNextPass, DISABLED_AfterBuildSigGetsPrepareDirectResidual) {
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
    EXPECT_EQ(*pass, PassId::kExtractProductCore);
}

TEST(SelectNextPass, DISABLED_PrereqBlocksOperandSimplifyBeforeExtract) {
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
    EXPECT_EQ(*pass, PassId::kExtractProductCore);
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

TEST(SelectNextPass, DISABLED_RewriteBudgetBlocksTransforms) {
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
        | (1ULL << static_cast< uint8_t >(PassId::kExtractProductCore));
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
    EXPECT_EQ(*pass, PassId::kExtractProductCore);
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
    EXPECT_EQ(*pass, PassId::kTrySemilinearPass);
}
