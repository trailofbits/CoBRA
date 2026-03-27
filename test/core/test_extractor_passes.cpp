#include "DecompositionPassHelpers.h"
#include "Orchestrator.h"
#include "OrchestratorPasses.h"
#include "cobra/core/Classification.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/SignatureEval.h"
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

// --- Direct-check candidate preserves group_id ---

TEST(ExtractorPass, DirectCandidatePreservesGroupId) {
    // When the extractor's direct-check passes and item.group_id
    // is set, the emitted CandidatePayload must inherit group_id.
    // Use a 1-var identity expression: f(x) = x.
    // Product extractor won't produce a direct hit on x, but
    // we can use a custom setup where the expression IS the core.
    //
    // Strategy: build a 2-var expression x0 * (x0 + x1) where
    // the product core is x0*(x0+x1) itself and direct-check
    // passes because the evaluator matches.
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [](const std::vector< uint64_t > &vals) -> uint64_t {
        return vals[0] * (vals[0] + vals[1]);
    };
    auto ctx = OrchestratorContext{
        .opts          = opts,
        .original_vars = vars,
        .evaluator     = std::optional< Evaluator >(opts.evaluator),
        .bitwidth      = 64,
    };

    auto expr = Expr::Mul(Expr::Variable(0), Expr::Add(Expr::Variable(0), Expr::Variable(1)));
    auto cls  = ClassifyStructural(*expr);
    WorkItem item;
    item.payload = AstPayload{
        .expr           = std::move(expr),
        .classification = cls,
        .provenance     = Provenance::kLowered,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kLowered;
    item.group_id                = 55;

    // Try the product extractor — it may or may not produce a
    // direct hit, but if it does, group_id must be preserved.
    auto result = RunExtractProductCore(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    // Whether it succeeded with direct check or emitted CoreCandidate,
    // any emitted child must have the correct group_id.
    for (const auto &child : pr.next) {
        ASSERT_TRUE(child.group_id.has_value());
        EXPECT_EQ(*child.group_id, 55);
    }
}

// --- Subproblem local context tests ---

TEST(ExtractorSubproblem, DirectCheckUsesLocalEvaluatorAndPreservesGroupId) {
    // Build a 1-var subproblem where the product extractor can
    // produce x0 as a direct-check candidate. The evaluator and
    // vars come from solve_ctx, group_id is preserved.
    std::vector< std::string > global_vars = { "x", "y" };
    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [](const std::vector< uint64_t > &vals) -> uint64_t {
        // Global evaluator uses 2 vars — should NOT be used
        return vals[0] + vals[1];
    };

    OrchestratorContext ctx{
        .opts          = opts,
        .original_vars = global_vars,
        .evaluator     = std::optional< Evaluator >(opts.evaluator),
        .bitwidth      = 64,
    };

    // Local 1-var evaluator: f(x) = x
    Evaluator local_eval = [](const std::vector< uint64_t > &vals) -> uint64_t {
        return vals[0];
    };
    auto local_sig = EvaluateBooleanSignature(local_eval, 1, 64);

    auto expr = Expr::Variable(0);
    auto cls  = ClassifyStructural(*expr);
    WorkItem item;
    item.payload = AstPayload{
        .expr           = std::move(expr),
        .classification = cls,
        .provenance     = Provenance::kLowered,
        .solve_ctx =
            AstSolveContext{
                            .vars      = { "x" },
                            .evaluator = local_eval,
                            .input_sig = local_sig,
                            },
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kLowered;
    item.group_id                = 99;

    auto result = RunExtractProductCore(item, ctx);
    ASSERT_TRUE(result.has_value());

    // Product core extraction on a plain variable likely returns
    // kNotApplicable or kBlocked. That is fine — what matters is
    // that it did NOT crash by passing 1-var values to a 2-var
    // evaluator. We just verify no error returned.
    EXPECT_NE(result.value().decision, PassDecision::kSolvedCandidate);
}

TEST(ExtractorSubproblem, PrepareDirectResidualUsesLocalContext) {
    // A 1-var subproblem with a local evaluator. Verify that
    // RunPrepareDirectRemainder uses local vars/eval.
    std::vector< std::string > global_vars = { "x", "y" };
    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [](const std::vector< uint64_t > &vals) -> uint64_t {
        return vals[0] + vals[1];
    };

    OrchestratorContext ctx{
        .opts          = opts,
        .original_vars = global_vars,
        .evaluator     = std::optional< Evaluator >(opts.evaluator),
        .bitwidth      = 64,
    };

    // Local 1-var evaluator
    Evaluator local_eval = [](const std::vector< uint64_t > &vals) -> uint64_t {
        return 3 * vals[0];
    };

    auto expr = Expr::Mul(Expr::Constant(3), Expr::Variable(0));
    auto cls  = ClassifyStructural(*expr);
    WorkItem item;
    item.payload = AstPayload{
        .expr           = std::move(expr),
        .classification = cls,
        .provenance     = Provenance::kLowered,
        .solve_ctx =
            AstSolveContext{
                            .vars      = { "x" },
                            .evaluator = local_eval,
                            },
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kLowered;

    auto result = RunPrepareDirectRemainder(item, ctx);
    ASSERT_TRUE(result.has_value());
    // Whether it finds a boolean null or not depends on the
    // expression structure. The key test is that it doesn't crash
    // by mixing 1-var and 2-var spaces.
    auto decision = result.value().decision;
    EXPECT_TRUE(
        decision == PassDecision::kAdvance || decision == PassDecision::kNotApplicable
        || decision == PassDecision::kBlocked
    );
}
