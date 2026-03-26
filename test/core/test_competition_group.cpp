#include "CompetitionGroup.h"
#include "ContinuationTypes.h"
#include "Orchestrator.h"
#include "OrchestratorPasses.h"
#include "SignaturePasses.h"
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

// --- Continuation field tests ---

TEST(CompetitionGroup, ContinuationDefaultsToNullopt) {
    std::unordered_map< GroupId, CompetitionGroup > groups;
    GroupId next_id = 0;
    auto id         = CreateGroup(groups, next_id);
    EXPECT_FALSE(groups.at(id).continuation.has_value());
}

TEST(CompetitionGroup, ContinuationMonostate) {
    std::unordered_map< GroupId, CompetitionGroup > groups;
    GroupId next_id            = 0;
    auto id                    = CreateGroup(groups, next_id);
    groups.at(id).continuation = ContinuationData{ std::monostate{} };
    ASSERT_TRUE(groups.at(id).continuation.has_value());
    EXPECT_TRUE(std::holds_alternative< std::monostate >(*groups.at(id).continuation));
}

TEST(CompetitionGroup, ContinuationBitwiseCompose) {
    std::unordered_map< GroupId, CompetitionGroup > groups;
    GroupId next_id            = 0;
    auto id                    = CreateGroup(groups, next_id);
    groups.at(id).continuation = ContinuationData{
        BitwiseComposeCont{
                           .var_k                  = 2,
                           .gate                   = GateKind::kXor,
                           .add_coeff              = 0,
                           .active_context_indices = { 0, 1 },
                           .parent_group_id        = 99,
                           }
    };
    ASSERT_TRUE(groups.at(id).continuation.has_value());
    EXPECT_TRUE(std::holds_alternative< BitwiseComposeCont >(*groups.at(id).continuation));
}

// --- RunResolveCompetition tests ---

namespace {

    Options MakeDefaultOpts() {
        Options opts;
        opts.bitwidth = 64;
        return opts;
    }

} // namespace

TEST(CompetitionGroup, ResolveMonostateWithWinner) {
    Options opts                    = MakeDefaultOpts();
    std::vector< std::string > vars = { "x0" };
    OrchestratorContext ctx{
        .opts          = opts,
        .original_vars = vars,
        .bitwidth      = 64,
    };

    auto gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);

    CandidateRecord rec;
    rec.expr         = Expr::Constant(42);
    rec.cost         = ExprCost{ .weighted_size = 3 };
    rec.verification = VerificationState::kVerified;
    rec.real_vars    = { "x0" };
    rec.source_pass  = PassId::kSupportedSolve;
    SubmitCandidate(ctx.competition_groups, gid, std::move(rec));

    WorkItem item;
    item.payload = CompetitionResolvedPayload{ .group_id = gid };

    auto result = RunResolveCompetition(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->decision, PassDecision::kSolvedCandidate);
    ASSERT_EQ(result->next.size(), 1);
    auto kind = GetStateKind(result->next[0].payload);
    EXPECT_EQ(kind, StateKind::kCandidateExpr);
    auto &cand = std::get< CandidatePayload >(result->next[0].payload);
    EXPECT_EQ(cand.cost.weighted_size, 3);
    EXPECT_EQ(cand.expr->constant_val, 42);

    // Group cleaned up
    EXPECT_EQ(ctx.competition_groups.count(gid), 0);
}

TEST(CompetitionGroup, ResolveMonostateNoWinner) {
    Options opts                    = MakeDefaultOpts();
    std::vector< std::string > vars = { "x0" };
    OrchestratorContext ctx{
        .opts          = opts,
        .original_vars = vars,
        .bitwidth      = 64,
    };

    auto gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    ReasonDetail failure;
    failure.top.message       = "technique A failed";
    failure.top.code.category = ReasonCategory::kNoSolution;
    ctx.competition_groups.at(gid).technique_failures.push_back(failure);

    ReasonDetail failure2;
    failure2.top.message       = "technique B failed";
    failure2.top.code.category = ReasonCategory::kSearchExhausted;
    ctx.competition_groups.at(gid).technique_failures.push_back(failure2);

    WorkItem item;
    item.payload = CompetitionResolvedPayload{ .group_id = gid };

    auto result = RunResolveCompetition(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->decision, PassDecision::kBlocked);
    EXPECT_EQ(result->reason.top.message, "technique A failed");
    EXPECT_EQ(result->reason.causes.size(), 1);
    EXPECT_EQ(result->reason.causes[0].message, "technique B failed");

    // Group cleaned up
    EXPECT_EQ(ctx.competition_groups.count(gid), 0);
}

TEST(CompetitionGroup, ResolveMonostateNoWinnerNoFailures) {
    Options opts                    = MakeDefaultOpts();
    std::vector< std::string > vars = { "x0" };
    OrchestratorContext ctx{
        .opts          = opts,
        .original_vars = vars,
        .bitwidth      = 64,
    };

    auto gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);

    WorkItem item;
    item.payload = CompetitionResolvedPayload{ .group_id = gid };

    auto result = RunResolveCompetition(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->decision, PassDecision::kBlocked);
    EXPECT_EQ(result->reason.top.code.category, ReasonCategory::kSearchExhausted);

    EXPECT_EQ(ctx.competition_groups.count(gid), 0);
}

TEST(CompetitionGroup, ResolveCleansUpGroup) {
    Options opts                    = MakeDefaultOpts();
    std::vector< std::string > vars = { "x0" };
    OrchestratorContext ctx{
        .opts          = opts,
        .original_vars = vars,
        .bitwidth      = 64,
    };

    auto gid0 = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    auto gid1 = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    EXPECT_EQ(ctx.competition_groups.size(), 2);

    WorkItem item;
    item.payload = CompetitionResolvedPayload{ .group_id = gid0 };
    auto result  = RunResolveCompetition(item, ctx);
    ASSERT_TRUE(result.has_value());

    // Only gid0 removed; gid1 still present
    EXPECT_EQ(ctx.competition_groups.size(), 1);
    EXPECT_EQ(ctx.competition_groups.count(gid0), 0);
    EXPECT_EQ(ctx.competition_groups.count(gid1), 1);
}

TEST(CompetitionGroup, ResolveHybridComposeContinuation) {
    Options opts                    = MakeDefaultOpts();
    std::vector< std::string > vars = { "x0" };
    OrchestratorContext ctx{
        .opts          = opts,
        .original_vars = vars,
        .bitwidth      = 64,
    };

    // Create parent group (receives composed result).
    // Handle-counted model: parent acquires handle per child.
    auto parent_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    AcquireHandle(ctx.competition_groups, parent_gid);

    // Create child group with HybridComposeCont
    auto child_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    ctx.competition_groups.at(child_gid).continuation = ContinuationData{
        HybridComposeCont{
                          .var_k                   = 0,
                          .op                      = ExtractOp::kXor,
                          .parent_group_id         = parent_gid,
                          .parent_real_vars        = vars,
                          .parent_original_indices = { 0 },
                          .parent_num_vars         = 1,
                          }
    };

    CandidateRecord rec;
    rec.expr        = Expr::Constant(1);
    rec.cost        = ExprCost{ .weighted_size = 2 };
    rec.source_pass = PassId::kSupportedSolve;
    SubmitCandidate(ctx.competition_groups, child_gid, std::move(rec));

    WorkItem item;
    item.payload = CompetitionResolvedPayload{ .group_id = child_gid };

    auto result = RunResolveCompetition(item, ctx);
    ASSERT_TRUE(result.has_value());

    // Fire-and-forget: resolution composes x0 ^ 1 and submits to parent.
    EXPECT_EQ(result->decision, PassDecision::kAdvance);

    // Child group should be erased
    EXPECT_EQ(ctx.competition_groups.count(child_gid), 0);
    // Parent group should have a candidate (x0 ^ 1)
    EXPECT_TRUE(ctx.competition_groups.at(parent_gid).best.has_value());
    // Handle-counted: resolution released the child's parent handle (2→1)
    EXPECT_EQ(ctx.competition_groups.at(parent_gid).open_handles, 1);
}

TEST(CompetitionGroup, ResolveNotApplicableForNonResolved) {
    Options opts                    = MakeDefaultOpts();
    std::vector< std::string > vars = { "x0" };
    OrchestratorContext ctx{
        .opts          = opts,
        .original_vars = vars,
        .bitwidth      = 64,
    };

    WorkItem item;
    item.payload = AstPayload{ .expr = Expr::Constant(0) };

    auto result = RunResolveCompetition(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->decision, PassDecision::kNotApplicable);
}

// --- SelectNextPass integration ---

TEST(CompetitionGroup, SchedulerRoutesResolvedToResolveCompetition) {
    OrchestratorPolicy policy;
    PassAttemptCache cache;

    WorkItem item;
    item.payload = CompetitionResolvedPayload{ .group_id = 0 };

    auto pass = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(pass.has_value());
    EXPECT_EQ(*pass, PassId::kResolveCompetition);
}

TEST(CompetitionGroup, SchedulerNoRepeatAfterAttemptMask) {
    OrchestratorPolicy policy;
    PassAttemptCache cache;

    WorkItem item;
    item.payload        = CompetitionResolvedPayload{ .group_id = 0 };
    item.attempted_mask = static_cast< uint64_t >(1)
        << static_cast< uint8_t >(PassId::kResolveCompetition);

    auto pass = SelectNextPass(item, policy, 0, cache);
    EXPECT_FALSE(pass.has_value());
}
