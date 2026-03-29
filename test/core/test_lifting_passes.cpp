#include "LiftingPasses.h"
#include "OrchestratorPasses.h"

#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/SignatureEval.h"

#include <gtest/gtest.h>

using namespace cobra;

namespace {

    OrchestratorContext
    MakeLiftCtx(const Options &opts, const std::vector< std::string > &vars) {
        OrchestratorContext ctx{
            .opts          = opts,
            .original_vars = vars,
            .bitwidth      = 64,
        };
        return ctx;
    }

} // namespace

// (x * x) & y -- arithmetic atom (x*x) under bitwise AND
TEST(ArithmeticAtomLifter, DetectsArithUnderBitwise) {
    auto expr =
        Expr::BitwiseAnd(Expr::Mul(Expr::Variable(0), Expr::Variable(0)), Expr::Variable(1));
    auto cls  = ClassifyStructural(*expr);
    auto vars = std::vector< std::string >{ "x", "y" };
    Options opts{ .bitwidth = 64, .max_vars = 16 };
    auto ctx = MakeLiftCtx(opts, vars);

    WorkItem item;
    item.payload = AstPayload{
        .expr           = std::move(expr),
        .classification = cls,
        .provenance     = Provenance::kOriginal,
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kOriginal;

    auto result = RunLiftArithmeticAtoms(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();
    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    ASSERT_EQ(pr.next.size(), 1);

    auto *skel = std::get_if< LiftedSkeletonPayload >(&pr.next[0].payload);
    ASSERT_NE(skel, nullptr);
    EXPECT_EQ(skel->bindings.size(), 1);
    EXPECT_EQ(skel->bindings[0].kind, LiftedValueKind::kArithmeticAtom);
    EXPECT_EQ(skel->outer_ctx.vars.size(), 3); // x, y, v0
    EXPECT_EQ(skel->original_var_count, 2);
}

TEST(ArithmeticAtomLifter, CarriesParentLocalSolveContext) {
    auto expr =
        Expr::BitwiseAnd(Expr::Mul(Expr::Variable(0), Expr::Variable(0)), Expr::Variable(1));
    auto cls = ClassifyStructural(*expr);

    auto root_vars  = std::vector< std::string >{ "x" };
    auto local_vars = std::vector< std::string >{ "x", "v0" };
    Options opts{ .bitwidth = 64, .max_vars = 16 };
    auto ctx = MakeLiftCtx(opts, root_vars);

    auto local_eval_expr =
        Expr::BitwiseAnd(Expr::Mul(Expr::Variable(0), Expr::Variable(0)), Expr::Variable(1));

    WorkItem item;
    item.payload = AstPayload{
        .expr           = std::move(expr),
        .classification = cls,
        .provenance     = Provenance::kRewritten,
        .solve_ctx =
            AstSolveContext{
                            .vars      = local_vars,
                            .evaluator = Evaluator::FromExpr(*local_eval_expr, 64),
                            },
    };
    item.features.classification = cls;
    item.features.provenance     = Provenance::kRewritten;

    auto result = RunLiftArithmeticAtoms(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();
    ASSERT_EQ(pr.next.size(), 1);

    auto *skel = std::get_if< LiftedSkeletonPayload >(&pr.next[0].payload);
    ASSERT_NE(skel, nullptr);
    EXPECT_EQ(skel->original_ctx.vars, local_vars);
    ASSERT_TRUE(skel->original_ctx.evaluator.has_value());
    EXPECT_EQ(skel->original_ctx.evaluator->InputArity(), 2u);
    EXPECT_EQ((*skel->original_ctx.evaluator)(std::vector< uint64_t >{ 3, 5 }), 1u);
}

// x & y -- no arithmetic atoms
TEST(ArithmeticAtomLifter, NoAtomsReturnsNotApplicable) {
    auto expr = Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1));
    auto vars = std::vector< std::string >{ "x", "y" };
    Options opts{ .bitwidth = 64, .max_vars = 16 };
    auto ctx = MakeLiftCtx(opts, vars);

    WorkItem item;
    item.payload = AstPayload{
        .expr       = std::move(expr),
        .provenance = Provenance::kOriginal,
    };

    auto result = RunLiftArithmeticAtoms(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNotApplicable);
}

// (x*x) & (x*x) -- deduplication
TEST(ArithmeticAtomLifter, DeduplicatesIdenticalAtoms) {
    auto expr = Expr::BitwiseAnd(
        Expr::Mul(Expr::Variable(0), Expr::Variable(0)),
        Expr::Mul(Expr::Variable(0), Expr::Variable(0))
    );
    auto vars = std::vector< std::string >{ "x" };
    Options opts{ .bitwidth = 64, .max_vars = 16 };
    auto ctx = MakeLiftCtx(opts, vars);

    WorkItem item;
    item.payload = AstPayload{
        .expr       = std::move(expr),
        .provenance = Provenance::kOriginal,
    };

    auto result = RunLiftArithmeticAtoms(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();
    EXPECT_EQ(pr.decision, PassDecision::kAdvance);

    auto *skel = std::get_if< LiftedSkeletonPayload >(&pr.next[0].payload);
    ASSERT_NE(skel, nullptr);
    EXPECT_EQ(skel->bindings.size(), 1);       // deduplicated
    EXPECT_EQ(skel->outer_ctx.vars.size(), 2); // x, v0
}

// f(x) XOR f(x) where f(x) = (x AND 3) — size 3 < threshold 4
TEST(RepeatedSubexprLifter, SmallRepeatsNotLifted) {
    auto f = [](uint32_t var) {
        return Expr::BitwiseAnd(Expr::Variable(var), Expr::Constant(3));
    };
    auto expr = Expr::BitwiseXor(f(0), f(0));
    auto vars = std::vector< std::string >{ "x" };
    Options opts{ .bitwidth = 64, .max_vars = 16 };
    auto ctx = MakeLiftCtx(opts, vars);

    WorkItem item;
    item.payload = AstPayload{
        .expr       = std::move(expr),
        .provenance = Provenance::kOriginal,
    };

    auto result = RunLiftRepeatedSubexpressions(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNotApplicable);
}

TEST(RepeatedSubexprLifter, LiftsLargeRepeatedSubtrees) {
    // f(x,y) = (x + y) * (x + y) — appears 2x
    auto f = []() {
        return Expr::Mul(
            Expr::Add(Expr::Variable(0), Expr::Variable(1)),
            Expr::Add(Expr::Variable(0), Expr::Variable(1))
        );
    };
    auto expr = Expr::BitwiseXor(f(), f());
    auto vars = std::vector< std::string >{ "x", "y" };
    Options opts{ .bitwidth = 64, .max_vars = 16 };
    auto ctx = MakeLiftCtx(opts, vars);

    WorkItem item;
    item.payload = AstPayload{
        .expr       = std::move(expr),
        .provenance = Provenance::kOriginal,
    };

    auto result = RunLiftRepeatedSubexpressions(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();
    EXPECT_EQ(pr.decision, PassDecision::kAdvance);

    auto *skel = std::get_if< LiftedSkeletonPayload >(&pr.next[0].payload);
    ASSERT_NE(skel, nullptr);
    EXPECT_EQ(skel->bindings.size(), 1);
    EXPECT_EQ(skel->bindings[0].kind, LiftedValueKind::kRepeatedSubexpression);
}

TEST(RepeatedSubexprLifter, MaximalNonOverlapping) {
    // outer = Mul(Add(x,y), Add(x,y)) — appears 2x
    // inner = Add(x,y) — appears 4x but size < 4
    auto outer = []() {
        return Expr::Mul(
            Expr::Add(Expr::Variable(0), Expr::Variable(1)),
            Expr::Add(Expr::Variable(0), Expr::Variable(1))
        );
    };
    auto expr = Expr::Add(outer(), outer());
    auto vars = std::vector< std::string >{ "x", "y" };
    Options opts{ .bitwidth = 64, .max_vars = 16 };
    auto ctx = MakeLiftCtx(opts, vars);

    WorkItem item;
    item.payload = AstPayload{
        .expr       = std::move(expr),
        .provenance = Provenance::kOriginal,
    };

    auto result = RunLiftRepeatedSubexpressions(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    if (pr.decision == PassDecision::kAdvance) {
        auto *skel = std::get_if< LiftedSkeletonPayload >(&pr.next[0].payload);
        ASSERT_NE(skel, nullptr);
        EXPECT_EQ(skel->bindings.size(), 1);
    }
}

// Too many vars after lifting
TEST(ArithmeticAtomLifter, BlocksOnMaxVars) {
    auto expr = Expr::BitwiseAnd(
        Expr::Mul(Expr::Variable(0), Expr::Variable(1)),
        Expr::Mul(Expr::Variable(2), Expr::Variable(2))
    );
    auto vars = std::vector< std::string >{ "a", "b", "c" };
    Options opts{ .bitwidth = 64, .max_vars = 3 };
    auto ctx = MakeLiftCtx(opts, vars);

    WorkItem item;
    item.payload = AstPayload{
        .expr       = std::move(expr),
        .provenance = Provenance::kOriginal,
    };

    auto result = RunLiftArithmeticAtoms(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kBlocked);
}
