#include "cobra/core/Expr.h"
#include "cobra/core/PassContract.h"
#include <gtest/gtest.h>

using namespace cobra;

// --- SolverResult invariant tests ---

TEST(SolverResultTest, SuccessHasPayloadNoReason) {
    auto r = SolverResult< int >::Success(42);
    EXPECT_TRUE(r.Succeeded());
    EXPECT_EQ(r.Kind(), OutcomeKind::kSuccess);
    EXPECT_EQ(r.Payload(), 42);
}

TEST(SolverResultTest, InapplicableHasReasonNoPayload) {
    ReasonDetail reason{ .top = { .code = { .category = ReasonCategory::kInapplicable,
                                            .domain   = ReasonDomain::kWeightedPolyFit } } };
    auto r = SolverResult< int >::Inapplicable(std::move(reason));
    EXPECT_FALSE(r.Succeeded());
    EXPECT_EQ(r.Kind(), OutcomeKind::kInapplicable);
    EXPECT_EQ(r.Reason().top.code.category, ReasonCategory::kInapplicable);
    EXPECT_EQ(r.Reason().top.code.domain, ReasonDomain::kWeightedPolyFit);
}

TEST(SolverResultTest, BlockedHasReasonNoPayload) {
    ReasonDetail reason{ .top = { .code = { .category = ReasonCategory::kRepresentationGap,
                                            .domain   = ReasonDomain::kSemilinear } } };
    auto r = SolverResult< int >::Blocked(std::move(reason));
    EXPECT_FALSE(r.Succeeded());
    EXPECT_EQ(r.Kind(), OutcomeKind::kBlocked);
    EXPECT_EQ(r.Reason().top.code.category, ReasonCategory::kRepresentationGap);
    EXPECT_EQ(r.Reason().top.code.domain, ReasonDomain::kSemilinear);
}

TEST(SolverResultTest, VerifyFailedHasPayloadAndReason) {
    ReasonDetail reason{ .top = { .code = { .category = ReasonCategory::kVerifyFailed,
                                            .domain   = ReasonDomain::kVerifier } } };
    auto r = SolverResult< int >::VerifyFailed(99, std::move(reason));
    EXPECT_FALSE(r.Succeeded());
    EXPECT_EQ(r.Kind(), OutcomeKind::kVerifyFailed);
    EXPECT_EQ(r.Payload(), 99);
    EXPECT_EQ(r.Reason().top.code.category, ReasonCategory::kVerifyFailed);
    EXPECT_EQ(r.Reason().top.code.domain, ReasonDomain::kVerifier);
}

TEST(SolverResultTest, TakePayloadMovesOut) {
    auto r     = SolverResult< int >::Success(42);
    int result = r.TakePayload();
    EXPECT_EQ(result, 42);
}

TEST(SolverResultTest, CauseChainPreserved) {
    ReasonFrame solver_frame{
        .code = { .category = ReasonCategory::kNoSolution,
                 .domain   = ReasonDomain::kWeightedPolyFit,
                 .subcode  = 2 }
    };
    ReasonDetail reason{ .top    = { .code = { .category = ReasonCategory::kRepresentationGap,
                                               .domain   = ReasonDomain::kDecomposition,
                                               .subcode  = 1 } },
                         .causes = { solver_frame } };
    auto r = SolverResult< int >::Blocked(std::move(reason));
    ASSERT_EQ(r.Reason().causes.size(), 1);
    EXPECT_EQ(r.Reason().causes[0].code.domain, ReasonDomain::kWeightedPolyFit);
    EXPECT_EQ(r.Reason().causes[0].code.subcode, 2);
    EXPECT_EQ(r.Reason().causes[0].code.category, ReasonCategory::kNoSolution);
    EXPECT_EQ(r.Reason().top.code.subcode, 1);
}

// --- SolverResult death tests (assert-guarded, debug builds only) ---

#ifdef NDEBUG
TEST(SolverResultDeathTest, PayloadOnBlockedAsserts) {
    GTEST_SKIP() << "assert() compiled out in Release builds";
}

TEST(SolverResultDeathTest, ReasonOnSuccessAsserts) {
    GTEST_SKIP() << "assert() compiled out in Release builds";
}
#else
TEST(SolverResultDeathTest, PayloadOnBlockedAsserts) {
    ReasonDetail reason{ .top = { .code = { .category = ReasonCategory::kRepresentationGap,
                                            .domain   = ReasonDomain::kSemilinear } } };
    auto r = SolverResult< int >::Blocked(std::move(reason));
    EXPECT_DEATH(r.Payload(), "");
}

TEST(SolverResultDeathTest, ReasonOnSuccessAsserts) {
    auto r = SolverResult< int >::Success(42);
    EXPECT_DEATH(r.Reason(), "");
}
#endif

// --- PassOutcome invariant tests ---

TEST(PassOutcomeTest, SuccessCarriesExprAndVars) {
    auto o =
        PassOutcome::Success(Expr::Constant(42), { "x", "y" }, VerificationState::kVerified);
    EXPECT_TRUE(o.Succeeded());
    EXPECT_EQ(o.Kind(), OutcomeKind::kSuccess);
    EXPECT_EQ(o.RealVars().size(), 2);
    EXPECT_EQ(o.RealVars()[0], "x");
    EXPECT_EQ(o.RealVars()[1], "y");
    EXPECT_EQ(o.Verification(), VerificationState::kVerified);
    EXPECT_EQ(o.GetExpr().kind, Expr::Kind::kConstant);
    EXPECT_EQ(o.GetExpr().constant_val, 42);
}

TEST(PassOutcomeTest, InapplicableHasNoExpr) {
    ReasonDetail reason{ .top = { .code = { .category = ReasonCategory::kGuardFailed,
                                            .domain   = ReasonDomain::kSemilinear } } };
    auto o = PassOutcome::Inapplicable(std::move(reason));
    EXPECT_FALSE(o.Succeeded());
    EXPECT_EQ(o.Kind(), OutcomeKind::kInapplicable);
    EXPECT_EQ(o.Reason().top.code.category, ReasonCategory::kGuardFailed);
}

TEST(PassOutcomeTest, SigVectorThreadsThrough) {
    auto o = PassOutcome::Success(Expr::Constant(0), {}, VerificationState::kUnverified);
    o.SetSigVector({ 1, 2, 3, 4 });
    ASSERT_EQ(o.SigVector().size(), 4);
    EXPECT_EQ(o.SigVector()[0], 1);
    EXPECT_EQ(o.SigVector()[1], 2);
    EXPECT_EQ(o.SigVector()[2], 3);
    EXPECT_EQ(o.SigVector()[3], 4);
}

// --- PassOutcome death tests (assert-guarded, debug builds only) ---

#ifdef NDEBUG
TEST(PassOutcomeDeathTest, ExprOnBlockedAsserts) {
    GTEST_SKIP() << "assert() compiled out in Release builds";
}

TEST(PassOutcomeDeathTest, PendingOnNonPartialAsserts) {
    GTEST_SKIP() << "assert() compiled out in Release builds";
}
#else
TEST(PassOutcomeDeathTest, ExprOnBlockedAsserts) {
    ReasonDetail reason{ .top = { .code = { .category = ReasonCategory::kRepresentationGap,
                                            .domain   = ReasonDomain::kSemilinear } } };
    auto o = PassOutcome::Blocked(std::move(reason));
    EXPECT_DEATH(o.GetExpr(), "");
}

TEST(PassOutcomeDeathTest, PendingOnNonPartialAsserts) {
    auto o = PassOutcome::Success(Expr::Constant(0), {}, VerificationState::kVerified);
    EXPECT_DEATH(o.Pending(), "");
}
#endif
