#include "CompetitionGroup.h"
#include "Orchestrator.h"
#include "OrchestratorPasses.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"

#include <gtest/gtest.h>

using namespace cobra;

// --- CreateGroup tests ---

TEST(CompetitionGroup, CreateGroupSetsHandleToOne) {
    std::unordered_map< GroupId, CompetitionGroup > groups;
    GroupId next_id = 0;
    auto id         = CreateGroup(groups, next_id);
    ASSERT_EQ(groups.count(id), 1);
    EXPECT_EQ(groups.at(id).open_handles, 1);
    EXPECT_FALSE(groups.at(id).best.has_value());
    EXPECT_FALSE(groups.at(id).baseline_cost.has_value());
}

TEST(CompetitionGroup, CreateGroupIncrementsId) {
    std::unordered_map< GroupId, CompetitionGroup > groups;
    GroupId next_id = 0;
    auto id0        = CreateGroup(groups, next_id);
    auto id1        = CreateGroup(groups, next_id);
    EXPECT_NE(id0, id1);
    EXPECT_EQ(id1, id0 + 1);
}

TEST(CompetitionGroup, CreateGroupWithBaselineCost) {
    std::unordered_map< GroupId, CompetitionGroup > groups;
    GroupId next_id = 0;
    ExprCost baseline{ .weighted_size = 10, .max_depth = 3 };
    auto id = CreateGroup(groups, next_id, baseline);
    ASSERT_TRUE(groups.at(id).baseline_cost.has_value());
    EXPECT_EQ(groups.at(id).baseline_cost->weighted_size, 10);
}

// --- SubmitCandidate tests ---

TEST(CompetitionGroup, SubmitToEmptyGroupAccepted) {
    std::unordered_map< GroupId, CompetitionGroup > groups;
    GroupId next_id = 0;
    auto id         = CreateGroup(groups, next_id);

    CandidateRecord rec;
    rec.expr        = Expr::Constant(42);
    rec.cost        = ExprCost{ .weighted_size = 5 };
    rec.source_pass = PassId::kSupportedSolve;

    EXPECT_TRUE(SubmitCandidate(groups, id, std::move(rec)));
    ASSERT_TRUE(groups.at(id).best.has_value());
    EXPECT_EQ(groups.at(id).best->cost.weighted_size, 5);
}

TEST(CompetitionGroup, SubmitWorseRejected) {
    std::unordered_map< GroupId, CompetitionGroup > groups;
    GroupId next_id = 0;
    auto id         = CreateGroup(groups, next_id);

    CandidateRecord good;
    good.expr        = Expr::Constant(1);
    good.cost        = ExprCost{ .weighted_size = 3 };
    good.source_pass = PassId::kSupportedSolve;
    EXPECT_TRUE(SubmitCandidate(groups, id, std::move(good)));

    CandidateRecord worse;
    worse.expr        = Expr::Constant(2);
    worse.cost        = ExprCost{ .weighted_size = 10 };
    worse.source_pass = PassId::kSupportedSolve;
    EXPECT_FALSE(SubmitCandidate(groups, id, std::move(worse)));
    EXPECT_EQ(groups.at(id).best->cost.weighted_size, 3);
}

TEST(CompetitionGroup, SubmitBetterAccepted) {
    std::unordered_map< GroupId, CompetitionGroup > groups;
    GroupId next_id = 0;
    auto id         = CreateGroup(groups, next_id);

    CandidateRecord first;
    first.expr        = Expr::Constant(1);
    first.cost        = ExprCost{ .weighted_size = 10 };
    first.source_pass = PassId::kSupportedSolve;
    EXPECT_TRUE(SubmitCandidate(groups, id, std::move(first)));

    CandidateRecord better;
    better.expr        = Expr::Constant(2);
    better.cost        = ExprCost{ .weighted_size = 3 };
    better.source_pass = PassId::kSupportedSolve;
    EXPECT_TRUE(SubmitCandidate(groups, id, std::move(better)));
    EXPECT_EQ(groups.at(id).best->cost.weighted_size, 3);
}

TEST(CompetitionGroup, SubmitBelowBaselineRejected) {
    std::unordered_map< GroupId, CompetitionGroup > groups;
    GroupId next_id = 0;
    ExprCost baseline{ .weighted_size = 5 };
    auto id = CreateGroup(groups, next_id, baseline);

    CandidateRecord rec;
    rec.expr        = Expr::Constant(1);
    rec.cost        = ExprCost{ .weighted_size = 8 };
    rec.source_pass = PassId::kSupportedSolve;
    EXPECT_FALSE(SubmitCandidate(groups, id, std::move(rec)));
    EXPECT_FALSE(groups.at(id).best.has_value());
}

TEST(CompetitionGroup, SubmitBetterThanBaselineAccepted) {
    std::unordered_map< GroupId, CompetitionGroup > groups;
    GroupId next_id = 0;
    ExprCost baseline{ .weighted_size = 10 };
    auto id = CreateGroup(groups, next_id, baseline);

    CandidateRecord rec;
    rec.expr        = Expr::Constant(1);
    rec.cost        = ExprCost{ .weighted_size = 3 };
    rec.source_pass = PassId::kSupportedSolve;
    EXPECT_TRUE(SubmitCandidate(groups, id, std::move(rec)));
    ASSERT_TRUE(groups.at(id).best.has_value());
    EXPECT_EQ(groups.at(id).best->cost.weighted_size, 3);
}

TEST(CompetitionGroup, SubmitEqualCostRejected) {
    std::unordered_map< GroupId, CompetitionGroup > groups;
    GroupId next_id = 0;
    auto id         = CreateGroup(groups, next_id);

    CandidateRecord first;
    first.expr        = Expr::Constant(1);
    first.cost        = ExprCost{ .weighted_size = 5 };
    first.source_pass = PassId::kSupportedSolve;
    EXPECT_TRUE(SubmitCandidate(groups, id, std::move(first)));

    CandidateRecord equal;
    equal.expr        = Expr::Constant(2);
    equal.cost        = ExprCost{ .weighted_size = 5 };
    equal.source_pass = PassId::kSupportedSolve;
    EXPECT_FALSE(SubmitCandidate(groups, id, std::move(equal)));
}

// --- ReleaseHandle tests ---

TEST(CompetitionGroup, ReleaseLastHandleReturnsResolved) {
    std::unordered_map< GroupId, CompetitionGroup > groups;
    GroupId next_id = 0;
    auto id         = CreateGroup(groups, next_id);

    auto result = ReleaseHandle(groups, id);
    ASSERT_TRUE(result.has_value());
    auto kind = GetStateKind(result->payload);
    EXPECT_EQ(kind, StateKind::kCompetitionResolved);
    auto &resolved = std::get< CompetitionResolvedPayload >(result->payload);
    EXPECT_EQ(resolved.group_id, id);
}

TEST(CompetitionGroup, ReleaseNonLastHandleReturnsNullopt) {
    std::unordered_map< GroupId, CompetitionGroup > groups;
    GroupId next_id = 0;
    auto id         = CreateGroup(groups, next_id);
    AcquireHandle(groups, id);
    EXPECT_EQ(groups.at(id).open_handles, 2);

    auto result = ReleaseHandle(groups, id);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(groups.at(id).open_handles, 1);
}

TEST(CompetitionGroup, AcquireAndReleaseMultipleHandles) {
    std::unordered_map< GroupId, CompetitionGroup > groups;
    GroupId next_id = 0;
    auto id         = CreateGroup(groups, next_id);
    AcquireHandle(groups, id);
    AcquireHandle(groups, id);
    EXPECT_EQ(groups.at(id).open_handles, 3);

    EXPECT_FALSE(ReleaseHandle(groups, id).has_value());
    EXPECT_EQ(groups.at(id).open_handles, 2);
    EXPECT_FALSE(ReleaseHandle(groups, id).has_value());
    EXPECT_EQ(groups.at(id).open_handles, 1);
    EXPECT_TRUE(ReleaseHandle(groups, id).has_value());
}

// --- State model integration tests ---

TEST(CompetitionGroup, GetStateKindForResolved) {
    StateData data = CompetitionResolvedPayload{ .group_id = 7 };
    EXPECT_EQ(GetStateKind(data), StateKind::kCompetitionResolved);
}

TEST(CompetitionGroup, FingerprintUsesGroupId) {
    WorkItem a;
    a.payload = CompetitionResolvedPayload{ .group_id = 1 };
    WorkItem b;
    b.payload = CompetitionResolvedPayload{ .group_id = 2 };
    auto fa   = ComputeFingerprint(a, 64);
    auto fb   = ComputeFingerprint(b, 64);
    EXPECT_NE(fa.payload_hash, fb.payload_hash);
}

TEST(CompetitionGroup, FingerprintSameGroupIdSameHash) {
    WorkItem a;
    a.payload = CompetitionResolvedPayload{ .group_id = 5 };
    WorkItem b;
    b.payload = CompetitionResolvedPayload{ .group_id = 5 };
    auto fa   = ComputeFingerprint(a, 64);
    auto fb   = ComputeFingerprint(b, 64);
    EXPECT_EQ(fa, fb);
}

// --- Band ordering tests ---

TEST(CompetitionGroup, ResolvedIsBandZero) {
    Worklist wl;
    WorkItem ast;
    ast.payload = AstPayload{ .expr = Expr::Constant(0) };
    WorkItem resolved;
    resolved.payload = CompetitionResolvedPayload{ .group_id = 0 };
    wl.Push(std::move(ast));
    wl.Push(std::move(resolved));
    auto first = wl.Pop();
    EXPECT_EQ(GetStateKind(first.payload), StateKind::kCompetitionResolved);
}

TEST(CompetitionGroup, CandidateOutranksResolved) {
    Worklist wl;
    WorkItem resolved;
    resolved.payload = CompetitionResolvedPayload{ .group_id = 0 };
    WorkItem cand;
    cand.payload = CandidatePayload{ .expr = Expr::Constant(1) };
    wl.Push(std::move(resolved));
    wl.Push(std::move(cand));
    auto first = wl.Pop();
    EXPECT_EQ(GetStateKind(first.payload), StateKind::kCandidateExpr);
}

TEST(CompetitionGroup, TwoResolvedOrderedByDepth) {
    Worklist wl;
    WorkItem deep;
    deep.payload = CompetitionResolvedPayload{ .group_id = 0 };
    deep.depth   = 5;
    WorkItem shallow;
    shallow.payload = CompetitionResolvedPayload{ .group_id = 1 };
    shallow.depth   = 1;
    wl.Push(std::move(deep));
    wl.Push(std::move(shallow));
    auto first = wl.Pop();
    EXPECT_EQ(first.depth, 1);
}

// --- Nested group tests ---

TEST(CompetitionGroup, NestedGroupReleasesParentHandle) {
    std::unordered_map< GroupId, CompetitionGroup > groups;
    GroupId next_id = 0;

    auto parent_id = CreateGroup(groups, next_id);
    AcquireHandle(groups, parent_id);
    EXPECT_EQ(groups.at(parent_id).open_handles, 2);

    auto child_id = CreateGroup(groups, next_id);
    EXPECT_EQ(groups.at(child_id).open_handles, 1);

    auto child_resolved = ReleaseHandle(groups, child_id);
    ASSERT_TRUE(child_resolved.has_value());
    EXPECT_EQ(GetStateKind(child_resolved->payload), StateKind::kCompetitionResolved);

    auto parent_partial = ReleaseHandle(groups, parent_id);
    EXPECT_FALSE(parent_partial.has_value());
    EXPECT_EQ(groups.at(parent_id).open_handles, 1);

    auto parent_resolved = ReleaseHandle(groups, parent_id);
    ASSERT_TRUE(parent_resolved.has_value());
    EXPECT_EQ(GetStateKind(parent_resolved->payload), StateKind::kCompetitionResolved);
}

// --- Technique failure tracking ---

TEST(CompetitionGroup, TechniqueFailuresAccumulate) {
    std::unordered_map< GroupId, CompetitionGroup > groups;
    GroupId next_id = 0;
    auto id         = CreateGroup(groups, next_id);

    ReasonDetail failure;
    failure.top.message = "solver failed";
    groups.at(id).technique_failures.push_back(std::move(failure));
    EXPECT_EQ(groups.at(id).technique_failures.size(), 1);
    EXPECT_EQ(groups.at(id).technique_failures[0].top.message, "solver failed");
}
