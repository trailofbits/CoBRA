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

// --- OrchestratorPolicy strict defaults ---

TEST(OrchestratorPolicy, StrictDefaults) {
    OrchestratorPolicy strict{
        .allow_reroute         = false,
        .strict_route_faithful = true,
    };
    EXPECT_FALSE(strict.allow_reroute);
    EXPECT_TRUE(strict.strict_route_faithful);
    EXPECT_EQ(strict.max_expansions, 64);
}

// --- Fingerprint tests ---

TEST(Fingerprint, SameExprSameFingerprint) {
    WorkItem a, b;
    a.payload = AstPayload{ .expr = Expr::Constant(42) };
    b.payload = AstPayload{ .expr = Expr::Constant(42) };
    auto fa   = ComputeFingerprint(a, 64, false);
    auto fb   = ComputeFingerprint(b, 64, false);
    EXPECT_EQ(fa, fb);
}

TEST(Fingerprint, DifferentExprDifferentHash) {
    WorkItem a, b;
    a.payload = AstPayload{ .expr = Expr::Constant(42) };
    b.payload = AstPayload{ .expr = Expr::Constant(99) };
    auto fa   = ComputeFingerprint(a, 64, false);
    auto fb   = ComputeFingerprint(b, 64, false);
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
    auto fa = ComputeFingerprint(a, 64, false);
    auto fb = ComputeFingerprint(b, 64, false);
    EXPECT_NE(fa.payload_hash, fb.payload_hash);
}

TEST(Fingerprint, StageCursorNormalization) {
    WorkItem a;
    a.payload      = AstPayload{ .expr = Expr::Constant(1) };
    a.stage_cursor = 3;
    auto strict    = ComputeFingerprint(a, 64, false);
    auto reroute   = ComputeFingerprint(a, 64, true);
    EXPECT_EQ(strict.stage_cursor, 3);
    EXPECT_EQ(reroute.stage_cursor, 0);
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
