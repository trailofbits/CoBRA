#include "DecompositionPassHelpers.h"
#include "Orchestrator.h"
#include "OrchestratorPasses.h"
#include "cobra/core/Expr.h"
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
    item.payload = ResidualStatePayload{
        .origin          = ResidualOrigin::kProductCore,
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
    item.payload = ResidualStatePayload{
        .origin          = ResidualOrigin::kProductCore,
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
    item.payload = ResidualStatePayload{
        .origin          = ResidualOrigin::kProductCore,
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
    item.payload = ResidualStatePayload{
        .origin = ResidualOrigin::kProductCore,
        .residual_elim =
            EliminationResult{
                              .real_vars = { "x0", "x1", "x2", "x3", "x4", "x5", "x6" },
                              },
        .residual_support = { 0, 1, 2, 3, 4, 5, 6 },
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
