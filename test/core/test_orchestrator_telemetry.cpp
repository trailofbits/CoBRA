#include "cobra/core/Expr.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/Simplifier.h"
#include "cobra/core/SimplifyOutcome.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(Telemetry, ExpansionsBounded) {
    auto expr = Expr::Constant(42);
    auto sig  = EvaluateBooleanSignature(*expr, 0, 64);
    Options opts;
    opts.bitwidth = 64;
    auto result   = Simplify(sig, {}, expr.get(), opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_LE(result.value().telemetry.total_expansions, 64u);
}

TEST(Telemetry, ConstantIsImmediate) {
    auto expr = Expr::Constant(42);
    auto sig  = EvaluateBooleanSignature(*expr, 0, 64);
    Options opts;
    opts.bitwidth = 64;
    auto result   = Simplify(sig, {}, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().telemetry.total_expansions, 0u);
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
}

TEST(Telemetry, HighWaterMarkRecorded) {
    auto expr = Expr::Variable(0);
    auto sig  = EvaluateBooleanSignature(*expr, 1, 64);
    Options opts;
    opts.bitwidth = 64;
    auto result   = Simplify(sig, { "x" }, expr.get(), opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_GE(result.value().telemetry.queue_high_water, 1u);
}

TEST(Telemetry, NonTrivialExprHasExpansions) {
    auto expr = Expr::Variable(0);
    auto sig  = EvaluateBooleanSignature(*expr, 1, 64);
    Options opts;
    opts.bitwidth = 64;
    auto result   = Simplify(sig, { "x" }, expr.get(), opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_GT(result.value().telemetry.total_expansions, 0u);
}

TEST(Telemetry, SignatureOnlyUnivariatePolynomialFastPathIsImmediate) {
    auto expr = Expr::Mul(Expr::Mul(Expr::Variable(0), Expr::Variable(0)), Expr::Variable(0));
    auto sig  = EvaluateBooleanSignature(*expr, 1, 64);

    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    opts.evaluator = Evaluator::FromExpr(*expr, 64);

    auto result = Simplify(sig, { "x" }, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
    EXPECT_EQ(result.value().telemetry.total_expansions, 0u);
    EXPECT_TRUE(result.value().verified);
}
