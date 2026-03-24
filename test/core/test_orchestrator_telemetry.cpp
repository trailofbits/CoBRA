#include "Orchestrator.h"
#include "OrchestratorPasses.h"
#include "cobra/core/Expr.h"
#include "cobra/core/SignatureEval.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(Telemetry, ExpansionsBounded) {
    auto expr = Expr::Constant(42);
    auto sig  = EvaluateBooleanSignature(*expr, 0, 64);
    Options opts;
    opts.bitwidth = 64;
    OrchestratorPolicy strict{ .allow_reroute = false, .strict_route_faithful = true };
    auto result = OrchestrateSimplify(expr.get(), sig, {}, opts, strict);
    ASSERT_TRUE(result.has_value());
    EXPECT_LE(result.value().telemetry.total_expansions, strict.max_expansions);
}

TEST(Telemetry, ConstantIsImmediate) {
    // Constants are resolved during seeding — zero loop expansions
    auto expr = Expr::Constant(42);
    auto sig  = EvaluateBooleanSignature(*expr, 0, 64);
    Options opts;
    opts.bitwidth = 64;
    OrchestratorPolicy strict{ .allow_reroute = false, .strict_route_faithful = true };
    // No-AST path with a constant signature
    auto result = OrchestrateSimplify(nullptr, sig, {}, opts, strict);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().telemetry.total_expansions, 0u);
    EXPECT_TRUE(result.value().outcome.Succeeded());
}

TEST(Telemetry, HighWaterMarkRecorded) {
    // Any non-trivial expression should produce a non-zero high water mark
    auto expr = Expr::Variable(0);
    auto sig  = EvaluateBooleanSignature(*expr, 1, 64);
    Options opts;
    opts.bitwidth = 64;
    OrchestratorPolicy strict{ .allow_reroute = false, .strict_route_faithful = true };
    auto result = OrchestrateSimplify(expr.get(), sig, { "x" }, opts, strict);
    ASSERT_TRUE(result.has_value());
    // The worklist should have had at least 1 item
    EXPECT_GE(result.value().telemetry.queue_high_water, 1u);
}

TEST(Telemetry, PassesAttemptedNonEmpty) {
    // A variable expression should attempt at least one pass
    auto expr = Expr::Variable(0);
    auto sig  = EvaluateBooleanSignature(*expr, 1, 64);
    Options opts;
    opts.bitwidth = 64;
    OrchestratorPolicy strict{ .allow_reroute = false, .strict_route_faithful = true };
    auto result = OrchestrateSimplify(expr.get(), sig, { "x" }, opts, strict);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result.value().telemetry.passes_attempted.empty());
}
