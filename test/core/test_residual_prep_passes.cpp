#include "DecompositionPassHelpers.h"
#include "Orchestrator.h"
#include "OrchestratorPasses.h"
#include "cobra/core/Expr.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(PrepareDirectResidual, InapplicableOnNonAst) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "x0" };
    OrchestratorContext ctx{
        .opts          = opts,
        .original_vars = vars,
        .bitwidth      = 64,
    };
    WorkItem item;
    item.payload = SignatureStatePayload{};

    auto result = RunPrepareDirectResidual(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNotApplicable);
}

TEST(PrepareDirectResidual, BlockedWithoutEvaluator) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "x0" };
    OrchestratorContext ctx{
        .opts          = opts,
        .original_vars = vars,
        .bitwidth      = 64,
    };
    WorkItem item;
    item.payload = AstPayload{
        .expr       = Expr::Variable(0),
        .provenance = Provenance::kLowered,
    };

    auto result = RunPrepareDirectResidual(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kBlocked);
}

TEST(PrepareResidualFromCore, InapplicableOnNonCore) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "x0" };
    OrchestratorContext ctx{
        .opts          = opts,
        .original_vars = vars,
        .bitwidth      = 64,
    };
    WorkItem item;
    item.payload = AstPayload{
        .expr       = Expr::Variable(0),
        .provenance = Provenance::kLowered,
    };

    auto result = RunPrepareResidualFromCore(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNotApplicable);
}

TEST(PrepareResidualFromCore, BlockedWithoutEvaluator) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "x0" };
    OrchestratorContext ctx{
        .opts          = opts,
        .original_vars = vars,
        .bitwidth      = 64,
    };
    WorkItem item;
    item.payload = CoreCandidatePayload{
        .core_expr      = Expr::Constant(42),
        .extractor_kind = ExtractorKind::kProductAST,
    };

    auto result = RunPrepareResidualFromCore(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kBlocked);
}
