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

    auto result = RunPrepareDirectRemainder(item, ctx);
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

    auto result = RunPrepareDirectRemainder(item, ctx);
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

    auto result = RunPrepareRemainderFromCore(item, ctx);
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

    auto result = RunPrepareRemainderFromCore(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kBlocked);
}

TEST(PrepareResidualFromCore, UsesTargetLocalEvaluatorWithoutGlobalEvaluator) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "g0", "g1" };
    OrchestratorContext ctx{
        .opts          = opts,
        .original_vars = vars,
        .bitwidth      = 64,
    };

    auto local_eval = Evaluator{ [](const std::vector< uint64_t > &vals) -> uint64_t {
        return vals[0] + 1;
    } };

    WorkItem item;
    item.payload = CoreCandidatePayload{
        .core_expr      = Expr::Variable(0),
        .extractor_kind = ExtractorKind::kPolynomial,
        .target =
            RemainderTargetContext{
                                   .eval = local_eval,
                                   .vars = { "x" },
                                   },
    };
    item.group_id = 9;

    auto result = RunPrepareRemainderFromCore(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    ASSERT_EQ(pr.next.size(), 1u);
    ASSERT_EQ(GetStateKind(pr.next[0].payload), StateKind::kRemainderState);
    auto &residual = std::get< RemainderStatePayload >(pr.next[0].payload);
    EXPECT_EQ(residual.target.vars, std::vector< std::string >({ "x" }));
    EXPECT_EQ(residual.target.eval(std::vector< uint64_t >{ 7 }), 8u);
    ASSERT_TRUE(pr.next[0].group_id.has_value());
    EXPECT_EQ(*pr.next[0].group_id, 9u);
}
