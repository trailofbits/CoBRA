#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureSimplifier.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(SignatureSimplifierTest, ConstantSig) {
    // sig = [42, 42, 42, 42] -> constant 42
    std::vector< uint64_t > sig = { 42, 42, 42, 42 };
    SignatureContext ctx;
    ctx.vars             = { "x", "y" };
    ctx.original_indices = { 0, 1 };
    Options opts{ .bitwidth = 64, .max_vars = 12, .spot_check = true };

    auto result = SimplifyFromSignature(sig, ctx, opts, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->expr->kind, Expr::Kind::kConstant);
    EXPECT_EQ(result->expr->constant_val, 42u);
}

TEST(SignatureSimplifierTest, XorSig) {
    // sig = [0, 1, 1, 0] -> x ^ y
    std::vector< uint64_t > sig = { 0, 1, 1, 0 };
    SignatureContext ctx;
    ctx.vars             = { "x", "y" };
    ctx.original_indices = { 0, 1 };
    Options opts{ .bitwidth = 64, .max_vars = 12, .spot_check = true };

    auto result = SimplifyFromSignature(sig, ctx, opts, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->expr->kind, Expr::Kind::kXor);
}

TEST(SignatureSimplifierTest, XPlusYSig) {
    // sig = [0, 1, 1, 2] -> x + y
    std::vector< uint64_t > sig = { 0, 1, 1, 2 };
    SignatureContext ctx;
    ctx.vars             = { "x", "y" };
    ctx.original_indices = { 0, 1 };
    Options opts{ .bitwidth = 64, .max_vars = 12, .spot_check = true };

    auto result = SimplifyFromSignature(sig, ctx, opts, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->expr->kind, Expr::Kind::kAdd);
}

TEST(SignatureSimplifierTest, IsBooleanValued) {
    EXPECT_TRUE(IsBooleanValued({ 0, 1, 1, 0 }));
    EXPECT_TRUE(IsBooleanValued({ 0, 0, 0, 0 }));
    EXPECT_TRUE(IsBooleanValued({ 1, 1, 1, 1 }));
    EXPECT_FALSE(IsBooleanValued({ 0, 1, 1, 2 }));
    EXPECT_FALSE(IsBooleanValued({ 0, 1, 3, 0 }));
}

TEST(SignatureSimplifierTest, FeatureFlagDisablesDecomposition) {
    // x & y: sig = [0, 0, 0, 1] — decomposable via cofactor
    std::vector< uint64_t > sig = { 0, 0, 0, 1 };
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] & v[1]; };
    SignatureContext ctx;
    ctx.vars             = { "x", "y" };
    ctx.original_indices = { 0, 1 };
    ctx.eval             = eval;

    // With decomposition enabled — should find And
    Options opts_on{
        .bitwidth = 64, .max_vars = 12, .spot_check = true, .enable_bitwise_decomposition = true
    };
    opts_on.evaluator = eval;
    auto r1           = SimplifyFromSignature(sig, ctx, opts_on, 0);
    ASSERT_TRUE(r1.has_value());

    // With decomposition disabled — should still produce a result
    // (pattern matcher handles pure bitwise), but the path through
    // the decomposer is not taken
    Options opts_off{ .bitwidth                     = 64,
                      .max_vars                     = 12,
                      .spot_check                   = true,
                      .enable_bitwise_decomposition = false };
    opts_off.evaluator = eval;
    auto r2            = SimplifyFromSignature(sig, ctx, opts_off, 0);
    ASSERT_TRUE(r2.has_value());
}

// --- kPolynomial recovery with singleton power interaction ---

TEST(SignatureSimplifierTest, CrossTermRecoveredWithSingleton) {
    // f(d,e) = d - d^2 + d*e  (= (1-d+e)*d)
    // On {0,1}: d&e.  Without the singleton-aware splitter,
    // polynomial recovery fails and the result is the bitwise
    // d&e which is wrong at full width.
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] - v[0] * v[0] + v[0] * v[1];
    };

    std::vector< uint64_t > sig = { 0, 0, 0, 1 };
    SignatureContext ctx;
    ctx.vars             = { "d", "e" };
    ctx.original_indices = { 0, 1 };
    ctx.eval             = eval;

    Options opts{ .bitwidth = 64, .max_vars = 12, .spot_check = true };
    opts.evaluator = eval;

    auto result = SimplifyFromSignature(sig, ctx, opts, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->verified);

    auto check = FullWidthCheckEval(eval, 2, *result->expr, 64);
    EXPECT_TRUE(check.passed);
}

TEST(SignatureSimplifierTest, kLinearPlusCrossTermRecovered) {
    // f(d,e) = d + d*e
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] + v[0] * v[1]; };

    std::vector< uint64_t > sig = { 0, 1, 0, 2 };
    SignatureContext ctx;
    ctx.vars             = { "d", "e" };
    ctx.original_indices = { 0, 1 };
    ctx.eval             = eval;

    Options opts{ .bitwidth = 64, .max_vars = 12, .spot_check = true };
    opts.evaluator = eval;

    auto result = SimplifyFromSignature(sig, ctx, opts, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->verified);

    auto check = FullWidthCheckEval(eval, 2, *result->expr, 64);
    EXPECT_TRUE(check.passed);
}

TEST(SignatureSimplifierTest, PureMulStillWorks) {
    // f(d,e) = d*e — should still be recovered correctly
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] * v[1]; };

    std::vector< uint64_t > sig = { 0, 0, 0, 1 };
    SignatureContext ctx;
    ctx.vars             = { "d", "e" };
    ctx.original_indices = { 0, 1 };
    ctx.eval             = eval;

    Options opts{ .bitwidth = 64, .max_vars = 12, .spot_check = true };
    opts.evaluator = eval;

    auto result = SimplifyFromSignature(sig, ctx, opts, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->verified);

    auto check = FullWidthCheckEval(eval, 2, *result->expr, 64);
    EXPECT_TRUE(check.passed);
}

// --- Word-valued cofactor decomposition through full pipeline ---

TEST(SignatureSimplifierTest, WordValuedOrFullPipeline) {
    // d | (a + b): the decomposer peels d, producing sub-problem
    // g = a+b with word-valued signature [0, 1, 1, 2].
    // The sub-pipeline must handle this non-boolean signature
    // correctly (ANF skipped, CoB/polynomial recovery used).
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] | (v[1] + v[2]);
    };

    std::vector< uint64_t > sig = { 0, 1, 1, 1, 1, 1, 2, 3 };
    SignatureContext ctx;
    ctx.vars             = { "d", "a", "b" };
    ctx.original_indices = { 0, 1, 2 };
    ctx.eval             = eval;

    Options opts{
        .bitwidth = 64, .max_vars = 12, .spot_check = true, .enable_bitwise_decomposition = true
    };
    opts.evaluator = eval;

    auto result = SimplifyFromSignature(sig, ctx, opts, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->verified);

    auto check = FullWidthCheckEval(eval, 3, *result->expr, 64);
    EXPECT_TRUE(check.passed);
}

TEST(SignatureSimplifierTest, WordValuedXorFullPipeline) {
    // d ^ (a + b): sub-problem g = a+b is word-valued.
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] ^ (v[1] + v[2]);
    };

    std::vector< uint64_t > sig = { 0, 1, 1, 0, 1, 0, 2, 3 };
    SignatureContext ctx;
    ctx.vars             = { "d", "a", "b" };
    ctx.original_indices = { 0, 1, 2 };
    ctx.eval             = eval;

    Options opts{
        .bitwidth = 64, .max_vars = 12, .spot_check = true, .enable_bitwise_decomposition = true
    };
    opts.evaluator = eval;

    auto result = SimplifyFromSignature(sig, ctx, opts, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->verified);

    auto check = FullWidthCheckEval(eval, 3, *result->expr, 64);
    EXPECT_TRUE(check.passed);
}

TEST(SignatureSimplifierTest, QuadraticOnlyNoSpuriousMul) {
    // f(d) = d - d^2, 1 variable.  On {0,1}: identically 0.
    // Should recover the polynomial without creating spurious MUL.
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] - v[0] * v[0]; };

    std::vector< uint64_t > sig = { 0, 0 };
    SignatureContext ctx;
    ctx.vars             = { "d" };
    ctx.original_indices = { 0 };
    ctx.eval             = eval;

    Options opts{ .bitwidth = 64, .max_vars = 12, .spot_check = true };
    opts.evaluator = eval;

    auto result = SimplifyFromSignature(sig, ctx, opts, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->verified);

    auto check = FullWidthCheckEval(eval, 1, *result->expr, 64);
    EXPECT_TRUE(check.passed);
}
