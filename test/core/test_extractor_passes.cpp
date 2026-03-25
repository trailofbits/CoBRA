#include "DecompositionPassHelpers.h"
#include "Orchestrator.h"
#include "OrchestratorPasses.h"
#include "cobra/core/Classification.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include <gtest/gtest.h>

using namespace cobra;

namespace {

    OrchestratorContext MakeCtx(const Options &opts, const std::vector< std::string > &vars) {
        return OrchestratorContext{
            .opts          = opts,
            .original_vars = vars,
            .bitwidth      = 64,
        };
    }

    WorkItem MakeAstItem(std::unique_ptr< Expr > expr) {
        auto cls = ClassifyStructural(*expr);
        WorkItem item;
        item.payload = AstPayload{
            .expr           = std::move(expr),
            .classification = cls,
            .provenance     = Provenance::kLowered,
        };
        item.features.classification = cls;
        item.features.provenance     = Provenance::kLowered;
        return item;
    }

} // namespace

TEST(ExtractProductCore, InapplicableOnConstant) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "x0" };
    auto ctx                        = MakeCtx(opts, vars);
    auto item                       = MakeAstItem(Expr::Constant(42));

    auto result = RunExtractProductCore(item, ctx);
    ASSERT_TRUE(result.has_value());
    // Constants don't have product cores - should be inapplicable or blocked
    EXPECT_NE(result.value().decision, PassDecision::kSolvedCandidate);
}

TEST(ExtractProductCore, BlockedWithoutEvaluator) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "x0" };
    auto ctx                        = MakeCtx(opts, vars);
    auto item                       = MakeAstItem(Expr::Variable(0));

    auto result = RunExtractProductCore(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kBlocked);
}

TEST(ExtractProductCore, NotApplicableOnSignatureState) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "x0" };
    auto ctx                        = MakeCtx(opts, vars);

    WorkItem item;
    item.payload = SignatureStatePayload{};
    auto result  = RunExtractProductCore(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNotApplicable);
}

TEST(ExtractPolyCoreD2, NotApplicableOnSignatureState) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "x0" };
    auto ctx                        = MakeCtx(opts, vars);

    WorkItem item;
    item.payload = SignatureStatePayload{};
    auto result  = RunExtractPolyCoreD2(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNotApplicable);
}

TEST(ExtractTemplateCore, NotApplicableOnSignatureState) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "x0" };
    auto ctx                        = MakeCtx(opts, vars);

    WorkItem item;
    item.payload = SignatureStatePayload{};
    auto result  = RunExtractTemplateCore(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNotApplicable);
}

TEST(ExtractPolyCoreD3, NotApplicableOnSignatureState) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "x0" };
    auto ctx                        = MakeCtx(opts, vars);

    WorkItem item;
    item.payload = SignatureStatePayload{};
    auto result  = RunExtractPolyCoreD3(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNotApplicable);
}

TEST(ExtractPolyCoreD4, NotApplicableOnSignatureState) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "x0" };
    auto ctx                        = MakeCtx(opts, vars);

    WorkItem item;
    item.payload = SignatureStatePayload{};
    auto result  = RunExtractPolyCoreD4(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNotApplicable);
}

TEST(ExtractPolyCoreD2, BlockedWithoutEvaluator) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "x0" };
    auto ctx                        = MakeCtx(opts, vars);
    auto item                       = MakeAstItem(Expr::Variable(0));

    auto result = RunExtractPolyCoreD2(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kBlocked);
}

TEST(ExtractTemplateCore, BlockedWithoutEvaluator) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "x0" };
    auto ctx                        = MakeCtx(opts, vars);
    auto item                       = MakeAstItem(Expr::Variable(0));

    auto result = RunExtractTemplateCore(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kBlocked);
}

TEST(ExtractPolyCoreD3, BlockedWithoutEvaluator) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "x0" };
    auto ctx                        = MakeCtx(opts, vars);
    auto item                       = MakeAstItem(Expr::Variable(0));

    auto result = RunExtractPolyCoreD3(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kBlocked);
}

TEST(ExtractPolyCoreD4, BlockedWithoutEvaluator) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "x0" };
    auto ctx                        = MakeCtx(opts, vars);
    auto item                       = MakeAstItem(Expr::Variable(0));

    auto result = RunExtractPolyCoreD4(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kBlocked);
}

TEST(ExtractProductCore, BlockedReasonHasMessage) {
    Options opts;
    opts.bitwidth                   = 64;
    std::vector< std::string > vars = { "x0" };
    auto ctx                        = MakeCtx(opts, vars);
    auto item                       = MakeAstItem(Expr::Variable(0));

    auto result = RunExtractProductCore(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kBlocked);
    EXPECT_FALSE(result.value().reason.top.message.empty());
    EXPECT_EQ(result.value().reason.top.code.category, ReasonCategory::kGuardFailed);
    EXPECT_EQ(result.value().reason.top.code.domain, ReasonDomain::kDecomposition);
}
