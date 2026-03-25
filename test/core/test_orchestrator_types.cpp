#include "Orchestrator.h"
#include "OrchestratorPasses.h"
#include "cobra/core/Expr.h"
#include <gtest/gtest.h>

using namespace cobra;

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
    EXPECT_EQ(item.stage_cursor, 0);
    EXPECT_FALSE(item.reentry_pending);
    EXPECT_EQ(item.resume_stage, 0);
    EXPECT_TRUE(item.history.empty());
}

// --- ItemMetadata default state tests ---

TEST(ItemMetadata, DefaultState) {
    ItemMetadata meta;
    EXPECT_EQ(meta.verification, VerificationState::kUnverified);
    EXPECT_EQ(meta.attempted_route, Route::kBitwiseOnly);
    EXPECT_EQ(meta.rewrite_rounds, 0);
    EXPECT_FALSE(meta.rewrite_produced_candidate);
    EXPECT_FALSE(meta.candidate_failed_verification);
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

TEST(Fingerprint, ReentryPendingDistinct) {
    WorkItem a, b;
    a.payload         = AstPayload{ .expr = Expr::Constant(1) };
    a.reentry_pending = true;
    b.payload         = AstPayload{ .expr = Expr::Constant(1) };
    b.reentry_pending = false;
    auto fa           = ComputeFingerprint(a, 64);
    auto fb           = ComputeFingerprint(b, 64);
    EXPECT_NE(fa, fb);
}

TEST(Fingerprint, ResumeStageDistinct) {
    WorkItem a, b;
    a.payload      = AstPayload{ .expr = Expr::Constant(1) };
    a.resume_stage = 3;
    b.payload      = AstPayload{ .expr = Expr::Constant(1) };
    b.resume_stage = 4;
    auto fa        = ComputeFingerprint(a, 64);
    auto fb        = ComputeFingerprint(b, 64);
    EXPECT_NE(fa, fb);
}

TEST(Fingerprint, StageCursorDistinct) {
    WorkItem a, b;
    a.payload      = AstPayload{ .expr = Expr::Constant(1) };
    a.stage_cursor = 3;
    b.payload      = AstPayload{ .expr = Expr::Constant(1) };
    b.stage_cursor = 0;
    auto fa        = ComputeFingerprint(a, 64);
    auto fb        = ComputeFingerprint(b, 64);
    EXPECT_NE(fa.stage_cursor, fb.stage_cursor);
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
    UnsupportedCandidate a{ .depth = 5, .last_pass = PassId::kDecompose };
    UnsupportedCandidate b{ .depth = 3, .last_pass = PassId::kDecompose };
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
    EXPECT_FALSE(cache.HasAttempted(fp, PassId::kDecompose));
    cache.Record(fp, PassId::kDecompose);
    EXPECT_TRUE(cache.HasAttempted(fp, PassId::kDecompose));
    EXPECT_FALSE(cache.HasAttempted(fp, PassId::kSupportedSolve));
}

// --- Scheduler tests ---

TEST(Scheduler, CandidateGetsVerify) {
    WorkItem item;
    item.payload = CandidatePayload{
        .expr                              = Expr::Constant(1),
        .needs_original_space_verification = true,
    };
    item.features.provenance = Provenance::kOriginal;
    PassAttemptCache cache;
    auto passes = SchedulePasses(item, cache);
    ASSERT_EQ(passes.size(), 1);
    EXPECT_EQ(passes[0], PassId::kVerifyCandidate);
}

TEST(Scheduler, VerifiedCandidateGetsNothing) {
    WorkItem item;
    item.payload = CandidatePayload{
        .expr                              = Expr::Constant(1),
        .needs_original_space_verification = false,
    };
    item.features.provenance = Provenance::kOriginal;
    PassAttemptCache cache;
    auto passes = SchedulePasses(item, cache);
    EXPECT_TRUE(passes.empty());
}

TEST(Scheduler, SignatureGetsSupported) {
    WorkItem item;
    item.payload             = SignatureStatePayload{};
    item.features.provenance = Provenance::kOriginal;
    PassAttemptCache cache;
    auto passes = SchedulePasses(item, cache);
    ASSERT_EQ(passes.size(), 1);
    EXPECT_EQ(passes[0], PassId::kSupportedSolve);
}

TEST(Scheduler, OriginalAstSemilinearGetsSemilinear) {
    WorkItem item;
    Classification cls{
        .semantic = SemanticClass::kSemilinear,
        .flags    = kSfNone,
        .route    = Route::kBitwiseOnly,
    };
    item.payload = AstPayload{
        .expr           = Expr::Constant(0),
        .classification = cls,
        .provenance     = Provenance::kOriginal,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kOriginal;
    PassAttemptCache cache;
    auto passes = SchedulePasses(item, cache);
    ASSERT_EQ(passes.size(), 1);
    EXPECT_EQ(passes[0], PassId::kTrySemilinearPass);
}

TEST(Scheduler, OriginalAstNonSemilinearGetsNothing) {
    WorkItem item;
    Classification cls{
        .semantic = SemanticClass::kLinear,
        .flags    = kSfNone,
        .route    = Route::kBitwiseOnly,
    };
    item.payload = AstPayload{
        .expr           = Expr::Constant(0),
        .classification = cls,
        .provenance     = Provenance::kOriginal,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kOriginal;
    PassAttemptCache cache;
    auto passes = SchedulePasses(item, cache);
    EXPECT_TRUE(passes.empty());
}

TEST(Scheduler, MixedStage0GetsSignatureBuild) {
    WorkItem item;
    Classification cls{
        .semantic = SemanticClass::kNonPolynomial,
        .flags    = kSfHasMixedProduct,
        .route    = Route::kMixedRewrite,
    };
    item.payload = AstPayload{
        .expr           = Expr::Constant(0),
        .classification = cls,
        .provenance     = Provenance::kLowered,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kLowered;
    item.stage_cursor            = 0;
    PassAttemptCache cache;
    auto passes = SchedulePasses(item, cache);
    ASSERT_FALSE(passes.empty());
    EXPECT_EQ(passes[0], PassId::kBuildSignatureState);
}

TEST(Scheduler, MixedStage1GetsDecompose) {
    WorkItem item;
    Classification cls{
        .semantic = SemanticClass::kNonPolynomial,
        .flags    = kSfHasMixedProduct,
        .route    = Route::kMixedRewrite,
    };
    item.payload = AstPayload{
        .expr           = Expr::Constant(0),
        .classification = cls,
        .provenance     = Provenance::kLowered,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kLowered;
    item.stage_cursor            = 1;
    PassAttemptCache cache;
    auto passes = SchedulePasses(item, cache);
    ASSERT_EQ(passes.size(), 1);
    EXPECT_EQ(passes[0], PassId::kDecompose);
}

TEST(Scheduler, MixedStage2GetsOperandSimplify) {
    WorkItem item;
    Classification cls{
        .semantic = SemanticClass::kNonPolynomial,
        .flags    = kSfHasMixedProduct,
        .route    = Route::kMixedRewrite,
    };
    item.payload = AstPayload{
        .expr           = Expr::Constant(0),
        .classification = cls,
        .provenance     = Provenance::kLowered,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kLowered;
    item.stage_cursor            = 2;
    PassAttemptCache cache;
    auto passes = SchedulePasses(item, cache);
    ASSERT_EQ(passes.size(), 1);
    EXPECT_EQ(passes[0], PassId::kOperandSimplify);
}

TEST(Scheduler, MixedStage3GetsProductIdentity) {
    WorkItem item;
    Classification cls{
        .semantic = SemanticClass::kNonPolynomial,
        .flags    = kSfHasMixedProduct,
        .route    = Route::kMixedRewrite,
    };
    item.payload = AstPayload{
        .expr           = Expr::Constant(0),
        .classification = cls,
        .provenance     = Provenance::kLowered,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kLowered;
    item.stage_cursor            = 3;
    PassAttemptCache cache;
    auto passes = SchedulePasses(item, cache);
    ASSERT_EQ(passes.size(), 1);
    EXPECT_EQ(passes[0], PassId::kProductIdentityCollapse);
}

TEST(Scheduler, MixedStage4GetsDecompose) {
    WorkItem item;
    Classification cls{
        .semantic = SemanticClass::kNonPolynomial,
        .flags    = kSfHasMixedProduct,
        .route    = Route::kMixedRewrite,
    };
    item.payload = AstPayload{
        .expr           = Expr::Constant(0),
        .classification = cls,
        .provenance     = Provenance::kLowered,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kLowered;
    item.stage_cursor            = 4;
    PassAttemptCache cache;
    auto passes = SchedulePasses(item, cache);
    ASSERT_EQ(passes.size(), 1);
    EXPECT_EQ(passes[0], PassId::kDecompose);
}

TEST(Scheduler, MixedStage5GetsXorLowering) {
    WorkItem item;
    Classification cls{
        .semantic = SemanticClass::kNonPolynomial,
        .flags    = kSfHasMixedProduct,
        .route    = Route::kMixedRewrite,
    };
    item.payload = AstPayload{
        .expr           = Expr::Constant(0),
        .classification = cls,
        .provenance     = Provenance::kLowered,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kLowered;
    item.stage_cursor            = 5;
    PassAttemptCache cache;
    auto passes = SchedulePasses(item, cache);
    ASSERT_EQ(passes.size(), 1);
    EXPECT_EQ(passes[0], PassId::kXorLowering);
}

TEST(Scheduler, MixedBeyondStage5GetsNothing) {
    WorkItem item;
    Classification cls{
        .semantic = SemanticClass::kNonPolynomial,
        .flags    = kSfHasMixedProduct,
        .route    = Route::kMixedRewrite,
    };
    item.payload = AstPayload{
        .expr           = Expr::Constant(0),
        .classification = cls,
        .provenance     = Provenance::kLowered,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kLowered;
    item.stage_cursor            = 6;
    PassAttemptCache cache;
    auto passes = SchedulePasses(item, cache);
    EXPECT_TRUE(passes.empty());
}

TEST(Scheduler, LoweredNonMixedGetsSignatureBuild) {
    WorkItem item;
    Classification cls{
        .semantic = SemanticClass::kLinear,
        .flags    = kSfNone,
        .route    = Route::kBitwiseOnly,
    };
    item.payload = AstPayload{
        .expr           = Expr::Constant(0),
        .classification = cls,
        .provenance     = Provenance::kLowered,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kLowered;
    PassAttemptCache cache;
    auto passes = SchedulePasses(item, cache);
    ASSERT_EQ(passes.size(), 1);
    EXPECT_EQ(passes[0], PassId::kBuildSignatureState);
}

TEST(Scheduler, UnsupportedRouteGetsNothing) {
    WorkItem item;
    Classification cls{
        .semantic = SemanticClass::kNonPolynomial,
        .flags    = kSfHasUnknownShape,
        .route    = Route::kUnsupported,
    };
    item.payload = AstPayload{
        .expr           = Expr::Constant(0),
        .classification = cls,
        .provenance     = Provenance::kLowered,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kLowered;
    PassAttemptCache cache;
    auto passes = SchedulePasses(item, cache);
    EXPECT_TRUE(passes.empty());
}

TEST(Scheduler, AttemptedPassesFiltered) {
    WorkItem item;
    item.payload             = SignatureStatePayload{};
    item.features.provenance = Provenance::kOriginal;
    PassAttemptCache cache;
    auto fp = ComputeFingerprint(item, 64);
    cache.Record(fp, PassId::kSupportedSolve);
    auto passes = SchedulePasses(item, cache);
    EXPECT_TRUE(passes.empty());
}

TEST(Scheduler, RewrittenReentryPendingGetsSigBuild) {
    WorkItem item;
    Classification cls{
        .semantic = SemanticClass::kNonPolynomial,
        .flags    = kSfHasMixedProduct,
        .route    = Route::kMixedRewrite,
    };
    item.payload = AstPayload{
        .expr           = Expr::Constant(0),
        .classification = cls,
        .provenance     = Provenance::kRewritten,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kRewritten;
    item.reentry_pending         = true;
    item.resume_stage            = 3;
    PassAttemptCache cache;
    auto passes = SchedulePasses(item, cache);
    ASSERT_EQ(passes.size(), 1);
    EXPECT_EQ(passes[0], PassId::kBuildSignatureState);
}

TEST(Scheduler, RewrittenResumesSuffix) {
    WorkItem item;
    Classification cls{
        .semantic = SemanticClass::kNonPolynomial,
        .flags    = kSfHasMixedProduct,
        .route    = Route::kMixedRewrite,
    };
    item.payload = AstPayload{
        .expr           = Expr::Constant(0),
        .classification = cls,
        .provenance     = Provenance::kRewritten,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kRewritten;
    item.reentry_pending         = false;
    item.resume_stage            = 4;
    item.stage_cursor            = 4;
    PassAttemptCache cache;
    auto passes = SchedulePasses(item, cache);
    ASSERT_EQ(passes.size(), 1);
    EXPECT_EQ(passes[0], PassId::kDecompose);
}

TEST(Scheduler, RewrittenUnsupportedRouteEmpty) {
    WorkItem item;
    Classification cls{
        .semantic = SemanticClass::kNonPolynomial,
        .flags    = kSfHasUnknownShape,
        .route    = Route::kUnsupported,
    };
    item.payload = AstPayload{
        .expr           = Expr::Constant(0),
        .classification = cls,
        .provenance     = Provenance::kRewritten,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kRewritten;
    item.reentry_pending         = true;
    PassAttemptCache cache;
    auto passes = SchedulePasses(item, cache);
    EXPECT_TRUE(passes.empty());
}

TEST(Scheduler, RewrittenSupportedRouteAfterReentryEmpty) {
    WorkItem item;
    Classification cls{
        .semantic = SemanticClass::kLinear,
        .flags    = kSfNone,
        .route    = Route::kBitwiseOnly,
    };
    item.payload = AstPayload{
        .expr           = Expr::Constant(0),
        .classification = cls,
        .provenance     = Provenance::kRewritten,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kRewritten;
    item.reentry_pending         = false;
    PassAttemptCache cache;
    auto passes = SchedulePasses(item, cache);
    EXPECT_TRUE(passes.empty());
}

TEST(Scheduler, RewrittenResumeBeyondMaxGetsNothing) {
    WorkItem item;
    Classification cls{
        .semantic = SemanticClass::kNonPolynomial,
        .flags    = kSfHasMixedProduct,
        .route    = Route::kMixedRewrite,
    };
    item.payload = AstPayload{
        .expr           = Expr::Constant(0),
        .classification = cls,
        .provenance     = Provenance::kRewritten,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kRewritten;
    item.reentry_pending         = false;
    item.resume_stage            = 6;
    item.stage_cursor            = 6;
    PassAttemptCache cache;
    auto passes = SchedulePasses(item, cache);
    EXPECT_TRUE(passes.empty());
}
