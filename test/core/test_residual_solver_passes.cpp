#include "CompetitionGroup.h"
#include "DecompositionPassHelpers.h"
#include "Orchestrator.h"
#include "OrchestratorPasses.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Expr.h"
#include "cobra/core/SignatureEval.h"
#include <atomic>
#include <gtest/gtest.h>

using namespace cobra;

// -- ResidualGhost ----------------------------------------------------------

TEST(ResidualGhost, InapplicableOnNonResidual) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "x0" };
    OrchestratorContext ctx{
        .opts          = opts,
        .original_vars = vars,
        .bitwidth      = 64,
    };
    WorkItem item;
    item.payload = AstPayload{ .expr = Expr::Variable(0) };

    auto result = RunResidualGhost(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNotApplicable);
}

TEST(ResidualGhost, InapplicableWhenNotBooleanNull) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "x0" };
    OrchestratorContext ctx{
        .opts          = opts,
        .original_vars = vars,
        .bitwidth      = 64,
    };
    WorkItem item;
    item.payload = RemainderStatePayload{
        .origin          = RemainderOrigin::kProductCore,
        .is_boolean_null = false,
    };

    auto result = RunResidualGhost(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNotApplicable);
}

// -- ResidualFactoredGhost --------------------------------------------------

TEST(ResidualFactoredGhost, InapplicableOnNonResidual) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "x0" };
    OrchestratorContext ctx{
        .opts          = opts,
        .original_vars = vars,
        .bitwidth      = 64,
    };
    WorkItem item;
    item.payload = AstPayload{ .expr = Expr::Variable(0) };

    auto result = RunResidualFactoredGhost(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNotApplicable);
}

TEST(ResidualFactoredGhost, InapplicableWhenNotBooleanNull) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "x0" };
    OrchestratorContext ctx{
        .opts          = opts,
        .original_vars = vars,
        .bitwidth      = 64,
    };
    WorkItem item;
    item.payload = RemainderStatePayload{
        .origin          = RemainderOrigin::kProductCore,
        .is_boolean_null = false,
    };

    auto result = RunResidualFactoredGhost(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNotApplicable);
}

// -- ResidualFactoredGhostEscalated -----------------------------------------

TEST(ResidualFactoredGhostEscalated, InapplicableOnNonResidual) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "x0" };
    OrchestratorContext ctx{
        .opts          = opts,
        .original_vars = vars,
        .bitwidth      = 64,
    };
    WorkItem item;
    item.payload = AstPayload{ .expr = Expr::Variable(0) };

    auto result = RunResidualFactoredGhostEscalated(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNotApplicable);
}

TEST(ResidualFactoredGhostEscalated, InapplicableWhenNotBooleanNull) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "x0" };
    OrchestratorContext ctx{
        .opts          = opts,
        .original_vars = vars,
        .bitwidth      = 64,
    };
    WorkItem item;
    item.payload = RemainderStatePayload{
        .origin          = RemainderOrigin::kProductCore,
        .is_boolean_null = false,
    };

    auto result = RunResidualFactoredGhostEscalated(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNotApplicable);
}

// -- ResidualSupported ------------------------------------------------------

TEST(ResidualSupported, InapplicableOnNonResidual) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "x0" };
    OrchestratorContext ctx{
        .opts          = opts,
        .original_vars = vars,
        .bitwidth      = 64,
    };
    WorkItem item;
    item.payload = AstPayload{ .expr = Expr::Variable(0) };

    auto result = RunResidualSupported(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNotApplicable);
}

// -- ResidualPolyRecovery ---------------------------------------------------

TEST(ResidualPolyRecovery, InapplicableOnNonResidual) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "x0" };
    OrchestratorContext ctx{
        .opts          = opts,
        .original_vars = vars,
        .bitwidth      = 64,
    };
    WorkItem item;
    item.payload = AstPayload{ .expr = Expr::Variable(0) };

    auto result = RunResidualPolyRecovery(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNotApplicable);
}

TEST(ResidualPolyRecovery, InapplicableWhenTooManyVars) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7" };
    OrchestratorContext ctx{
        .opts          = opts,
        .original_vars = vars,
        .bitwidth      = 64,
    };
    WorkItem item;
    item.payload = RemainderStatePayload{
        .origin = RemainderOrigin::kProductCore,
        .remainder_elim =
            EliminationResult{
                              .real_vars = { "x0", "x1", "x2", "x3", "x4", "x5", "x6" },
                              },
        .remainder_support = { 0, 1, 2, 3, 4, 5, 6 },
    };

    auto result = RunResidualPolyRecovery(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNotApplicable);
}

// -- ResidualTemplate -------------------------------------------------------

TEST(ResidualTemplate, InapplicableOnNonResidual) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "x0" };
    OrchestratorContext ctx{
        .opts          = opts,
        .original_vars = vars,
        .bitwidth      = 64,
    };
    WorkItem item;
    item.payload = AstPayload{ .expr = Expr::Variable(0) };

    auto result = RunResidualTemplate(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNotApplicable);
}

// -- Target-local regression tests ------------------------------------------

TEST(ResidualGhost, UsesTargetContextNotGlobal) {
    // Build a 2-variable target subspace embedded in a 3-variable context.
    // f(a, b) = a*b - (a & b) is a boolean-null residual (zero on {0,1}).
    // The global context has 3 vars, but target describes the 2-var space.
    // Before the fix, RunResidualGhost would use ctx.original_vars.size()
    // (3) as num_vars, but the residual_eval is a 2-var function, so the
    // solver sends 3-element vectors that the evaluator misinterprets.
    //
    // We make the 2-var evaluator abort if called with the wrong arity
    // so the test catches the pre-fix behavior.

    std::atomic< bool > arity_violation{ false };

    // Target-local evaluator: operates in the 2-var subspace.
    Evaluator target_eval =
        [&arity_violation](const std::vector< uint64_t > &vals) -> uint64_t {
        if (vals.size() != 2) {
            arity_violation.store(true);
            return 0;
        }
        return vals[0] * vals[1] - (vals[0] & vals[1]);
    };

    // Global evaluator: operates in the 3-var space.
    Evaluator global_eval = [](const std::vector< uint64_t > &vals) -> uint64_t {
        return vals[1] * vals[2] - (vals[1] & vals[2]);
    };

    std::vector< std::string > global_vars   = { "x0", "x1", "x2" };
    std::vector< std::string > target_vars   = { "x1", "x2" };
    std::vector< uint32_t > target_remap     = { 1, 2 };
    std::vector< uint32_t > residual_support = { 0, 1 };

    Options opts;
    opts.bitwidth = 64;
    OrchestratorContext ctx{
        .opts          = opts,
        .original_vars = global_vars,
        .evaluator     = global_eval,
        .bitwidth      = 64,
    };

    auto target_sig = EvaluateBooleanSignature(target_eval, 2, 64);
    auto elim       = EliminateAuxVars(target_sig, target_vars);

    WorkItem item;
    item.payload = RemainderStatePayload{
        .origin            = RemainderOrigin::kDirectBooleanNull,
        .prefix_expr       = nullptr,
        .prefix_degree     = 0,
        .remainder_eval    = target_eval,
        .source_sig        = target_sig,
        .remainder_sig     = target_sig,
        .remainder_elim    = elim,
        .remainder_support = residual_support,
        .is_boolean_null   = true,
        .degree_floor      = 2,
        .target =
            RemainderTargetContext{
                                   .eval          = target_eval,
                                   .vars          = target_vars,
                                   .remap_support = target_remap,
                                   },
    };

    auto result = RunResidualGhost(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    // The solver must not call the evaluator with the wrong arity.
    EXPECT_FALSE(arity_violation.load())
        << "Evaluator called with wrong arity (3 instead of 2)";

    // The ghost solver should run in the 2-var target space. If it
    // succeeds, recombination should verify against the target evaluator
    // and emit a candidate with real_vars == target_vars.
    if (pr.decision == PassDecision::kSolvedCandidate) {
        ASSERT_EQ(pr.next.size(), 1);
        auto &cand = std::get< CandidatePayload >(pr.next[0].payload);
        EXPECT_EQ(cand.real_vars, target_vars);
    } else {
        // Even if the solver doesn't find a solution, it must not crash
        // or produce incorrect arity. kBlocked is acceptable.
        EXPECT_EQ(pr.decision, PassDecision::kBlocked);
    }
}
