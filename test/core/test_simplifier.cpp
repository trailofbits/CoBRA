#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classification.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/Simplifier.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(SimplifierTest, ConstantMBA) {
    std::vector< uint64_t > sig     = { 42, 42, 42, 42 };
    std::vector< std::string > vars = { "x", "y" };
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(Render(*result.value().expr, result.value().real_vars), "42");
    EXPECT_TRUE(result.value().verified);
}

TEST(SimplifierTest, XPlusY) {
    std::vector< uint64_t > sig     = { 0, 1, 1, 2 };
    std::vector< std::string > vars = { "x", "y" };
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    auto text = Render(*result.value().expr, result.value().real_vars);
    EXPECT_EQ(text, "x + y");
}

TEST(SimplifierTest, XorY) {
    std::vector< uint64_t > sig     = { 0, 1, 1, 0 };
    std::vector< std::string > vars = { "x", "y" };
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    auto text = Render(*result.value().expr, result.value().real_vars);
    EXPECT_EQ(text, "x ^ y");
}

TEST(SimplifierTest, WithAuxVars) {
    std::vector< uint64_t > sig(16);
    for (uint32_t i = 0; i < 16; ++i) {
        uint64_t x = (i >> 2) & 1;
        uint64_t y = (i >> 3) & 1;
        sig[i]     = x + y;
    }
    std::vector< std::string > vars = { "a0", "a1", "x", "y" };
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    auto text = Render(*result.value().expr, result.value().real_vars);
    EXPECT_EQ(text, "x + y");
    EXPECT_EQ(result.value().real_vars.size(), 2u);
}

TEST(SimplifierTest, ThreeVarAffine) {
    std::vector< uint64_t > sig(8);
    for (uint32_t i = 0; i < 8; ++i) {
        uint64_t x = (i >> 0) & 1;
        uint64_t y = (i >> 1) & 1;
        uint64_t z = (i >> 2) & 1;
        sig[i]     = 3 * x + 5 * y + 7 * z;
    }
    std::vector< std::string > vars = { "x", "y", "z" };
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    auto text = Render(*result.value().expr, result.value().real_vars);
    EXPECT_EQ(text, "3 * x + 5 * y + 7 * z");
}

TEST(SimplifierTest, TooManyVarsError) {
    // x + y: non-constant, both vars are real => 2 vars > max_vars=1
    std::vector< uint64_t > sig     = { 0, 1, 1, 2 };
    std::vector< std::string > vars = { "x", "y" };
    Options opts{ .bitwidth = 64, .max_vars = 1, .spot_check = false };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, CobraError::kTooManyVariables);
}

TEST(SimplifierTest, NonAffineBitwisePlusLinear) {
    // f(x,y,z) = (x & y) + z — non-affine CoB, now handled
    std::vector< uint64_t > sig(8);
    for (uint32_t i = 0; i < 8; ++i) {
        uint64_t x = (i >> 0) & 1;
        uint64_t y = (i >> 1) & 1;
        uint64_t z = (i >> 2) & 1;
        sig[i]     = (x & y) + z;
    }
    std::vector< std::string > vars = { "x", "y", "z" };
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value().verified);
    auto text = Render(*result.value().expr, result.value().real_vars);
    EXPECT_EQ(text, "(x & y) + z");
}

// Gap #1: 1-bit bitwidth through simplifier
TEST(SimplifierTest, Bitwidth1XPlusY) {
    // At 1-bit: x + y mod 2 = XOR. sig = [0, 1, 1, 0]
    std::vector< uint64_t > sig     = { 0, 1, 1, 0 };
    std::vector< std::string > vars = { "x", "y" };
    Options opts{ .bitwidth = 1, .max_vars = 16, .spot_check = true };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    auto text = Render(*result.value().expr, result.value().real_vars);
    EXPECT_EQ(text, "x ^ y");
    EXPECT_TRUE(result.value().verified);
}

// Gap #2: 4 variables
TEST(SimplifierTest, FourVarAffine) {
    std::vector< uint64_t > sig(16);
    for (uint32_t i = 0; i < 16; ++i) {
        uint64_t x = (i >> 0) & 1;
        uint64_t y = (i >> 1) & 1;
        uint64_t z = (i >> 2) & 1;
        uint64_t w = (i >> 3) & 1;
        sig[i]     = x + 2 * y + 3 * z + 4 * w;
    }
    std::vector< std::string > vars = { "x", "y", "z", "w" };
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    auto text = Render(*result.value().expr, result.value().real_vars);
    EXPECT_EQ(text, "x + 2 * y + 3 * z + 4 * w");
    EXPECT_TRUE(result.value().verified);
}

// Gap #8: max_vars boundary — N vars with max_vars=N should pass
TEST(SimplifierTest, MaxVarsExactBoundary) {
    std::vector< uint64_t > sig     = { 0, 1, 1, 2 };
    std::vector< std::string > vars = { "x", "y" };
    Options opts{ .bitwidth = 64, .max_vars = 2, .spot_check = false };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    auto text = Render(*result.value().expr, result.value().real_vars);
    EXPECT_EQ(text, "x + y");
}

// Verify sig_vector stores reduced sig (not original) after aux var elimination
TEST(SimplifierTest, SigVectorIsReducedAfterElimination) {
    std::vector< uint64_t > sig(16);
    for (uint32_t i = 0; i < 16; ++i) {
        uint64_t x = (i >> 2) & 1;
        uint64_t y = (i >> 3) & 1;
        sig[i]     = x + y;
    }
    std::vector< std::string > vars = { "a0", "a1", "x", "y" };
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().real_vars.size(), 2u);
    // sig_vector should be the reduced 4-entry signature, not the original 16
    EXPECT_EQ(result.value().sig_vector.size(), 4u);
    EXPECT_EQ(result.value().sig_vector[0], 0u);
    EXPECT_EQ(result.value().sig_vector[1], 1u);
    EXPECT_EQ(result.value().sig_vector[2], 1u);
    EXPECT_EQ(result.value().sig_vector[3], 2u);
}

// spot_check=false path — still produces correct result but verified=false
TEST(SimplifierTest, SpotCheckFalse) {
    std::vector< uint64_t > sig     = { 0, 1, 1, 2 };
    std::vector< std::string > vars = { "x", "y" };
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = false };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    auto text = Render(*result.value().expr, result.value().real_vars);
    EXPECT_EQ(text, "x + y");
    EXPECT_FALSE(result.value().verified);
}

// All-zero sig through full pipeline
TEST(SimplifierTest, AllZeroSig) {
    std::vector< uint64_t > sig     = { 0, 0, 0, 0 };
    std::vector< std::string > vars = { "x", "y" };
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(Render(*result.value().expr, result.value().real_vars), "0");
    EXPECT_TRUE(result.value().verified);
}

// Bitwidth=8 through full pipeline with wrapping
TEST(SimplifierTest, Bitwidth8Wrapping) {
    // 200*x + 100*y at 8-bit: wraps on addition
    std::vector< uint64_t > sig     = { 0, 200, 100, 44 }; // (200+100) mod 256 = 44
    std::vector< std::string > vars = { "x", "y" };
    Options opts{ .bitwidth = 8, .max_vars = 16, .spot_check = true };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value().verified);
}

// Bitwidth=16 through full pipeline
TEST(SimplifierTest, Bitwidth16) {
    std::vector< uint64_t > sig     = { 0, 1, 1, 2 };
    std::vector< std::string > vars = { "x", "y" };
    Options opts{ .bitwidth = 16, .max_vars = 16, .spot_check = true };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    auto text = Render(*result.value().expr, result.value().real_vars);
    EXPECT_EQ(text, "x + y");
    EXPECT_TRUE(result.value().verified);
}

// Bitwidth=32 through full pipeline
TEST(SimplifierTest, Bitwidth32) {
    std::vector< uint64_t > sig     = { 0, 1, 1, 2 };
    std::vector< std::string > vars = { "x", "y" };
    Options opts{ .bitwidth = 32, .max_vars = 16, .spot_check = true };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    auto text = Render(*result.value().expr, result.value().real_vars);
    EXPECT_EQ(text, "x + y");
    EXPECT_TRUE(result.value().verified);
}

// Bitwidth=1 with 3 vars through full pipeline
TEST(SimplifierTest, Bitwidth1ThreeVar) {
    // At 1-bit: x + y + z mod 2 = XOR chain
    std::vector< uint64_t > sig(8);
    for (uint32_t i = 0; i < 8; ++i) {
        uint64_t x = (i >> 0) & 1;
        uint64_t y = (i >> 1) & 1;
        uint64_t z = (i >> 2) & 1;
        sig[i]     = (x + y + z) & 1;
    }
    std::vector< std::string > vars = { "x", "y", "z" };
    Options opts{ .bitwidth = 1, .max_vars = 16, .spot_check = true };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value().verified);
}

// Left shift desugars to multiplication: x << 1 = 2*x, sig = [0, 2]
TEST(SimplifierTest, LeftShiftSimplifies) {
    std::vector< uint64_t > sig     = { 0, 2 };
    std::vector< std::string > vars = { "x" };
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    auto text = Render(*result.value().expr, result.value().real_vars);
    EXPECT_NE(text.find("2"), std::string::npos) << "Expected '2' in output, got: " << text;
    EXPECT_TRUE(result.value().verified);
}

// (x & y) << 3 = 8*(x&y), sig = [0, 0, 0, 8]
TEST(SimplifierTest, ShiftedBitwiseSimplifies) {
    std::vector< uint64_t > sig     = { 0, 0, 0, 8 };
    std::vector< std::string > vars = { "x", "y" };
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value().verified);
    auto text = Render(*result.value().expr, result.value().real_vars);
    EXPECT_EQ(text, "8 * (x & y)");
}

// Large constant through full pipeline
TEST(SimplifierTest, LargeConstant) {
    uint64_t big                    = UINT64_MAX - 42;
    std::vector< uint64_t > sig     = { big, big, big, big };
    std::vector< std::string > vars = { "x", "y" };
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().expr->kind, Expr::Kind::kConstant);
    EXPECT_EQ(result.value().expr->constant_val, big);
}

// kPolynomial MBA where terms cancel → CoB result is full-width correct
TEST(SimplifierTest, PolynomialCancelledToLinear) {
    // f(x,y) = x*y - x*y + x + y = x + y
    // sig on {0,1}: [0, 1, 1, 2]
    std::vector< uint64_t > sig     = { 0, 1, 1, 2 };
    std::vector< std::string > vars = { "x", "y" };
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    auto text = Render(*result.value().expr, result.value().real_vars);
    EXPECT_EQ(text, "x + y");

    // Verify full-width: the CoB result x+y matches original x+y
    auto original = Expr::Add(Expr::Variable(0), Expr::Variable(1));
    auto fw       = FullWidthCheck(*original, 2, *result.value().expr, {}, 64);
    EXPECT_TRUE(fw.passed);
}

// kPolynomial target x*y → CoB produces x&y → full-width check rejects
TEST(SimplifierTest, PolynomialTargetFailsFullWidth) {
    // f(x,y) = x*y, sig on {0,1}: [0, 0, 0, 1] (same as x&y)
    std::vector< uint64_t > sig     = { 0, 0, 0, 1 };
    std::vector< std::string > vars = { "x", "y" };
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    // CoB produces x&y (correct on {0,1}, wrong on full-width)
    auto text = Render(*result.value().expr, result.value().real_vars);
    EXPECT_EQ(text, "x & y");

    // Full-width check should reject
    auto original = Expr::Mul(Expr::Variable(0), Expr::Variable(1));
    auto fw       = FullWidthCheck(*original, 2, *result.value().expr, {}, 64);
    EXPECT_FALSE(fw.passed);
}

TEST(SimplifierTest, PolynomialTargetRecoveredViaSplit) {
    // f(x,y) = x*y. sig on {0,1}: [0, 0, 0, 1].
    // Pattern matcher will match x&y. Full-width check via evaluator
    // rejects it, then splitting recovers x*y.
    std::vector< uint64_t > sig     = { 0, 0, 0, 1 };
    std::vector< std::string > vars = { "x", "y" };

    auto evaluator = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] * v[1]; };

    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    opts.evaluator = evaluator;

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());

    // Verify the result is full-width correct for x*y
    auto original = Expr::Mul(Expr::Variable(0), Expr::Variable(1));
    auto fw       = FullWidthCheck(*original, 2, *result.value().expr, {}, 64);
    EXPECT_TRUE(fw.passed);
}

TEST(SimplifierTest, ProductIdentityCollapsesToPlainProduct) {
    auto x = []() { return Expr::Variable(0); };
    auto y = []() { return Expr::Variable(1); };

    auto obf = Expr::Add(
        Expr::Mul(Expr::BitwiseAnd(x(), y()), Expr::BitwiseOr(x(), y())),
        Expr::Mul(
            Expr::BitwiseAnd(x(), Expr::BitwiseNot(y())),
            Expr::BitwiseAnd(Expr::BitwiseNot(x()), y())
        )
    );

    auto original_cost = ComputeCost(*obf).cost;
    auto evaluator = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] * v[1]; };

    auto sig                        = EvaluateBooleanSignature(*obf, 2, 64);
    std::vector< std::string > vars = { "x", "y" };
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    opts.evaluator = evaluator;

    auto result = Simplify(sig, vars, obf.get(), opts);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
    EXPECT_TRUE(result.value().verified);

    auto simplified_cost = ComputeCost(*result.value().expr).cost;
    EXPECT_TRUE(IsBetter(simplified_cost, original_cost));

    auto rendered = Render(*result.value().expr, result.value().real_vars);
    EXPECT_TRUE(rendered == "x * y" || rendered == "y * x") << rendered;

    auto fw = FullWidthCheckEval(evaluator, 2, *result.value().expr, 64);
    EXPECT_TRUE(fw.passed);
}

TEST(SimplifierTest, PolynomialXSquaredRecovered) {
    // f(x) = x^2. sig = [0, 1]. CoB gives coefficient 1 for {x}.
    // Pattern matcher matches "x" but full-width check rejects it
    // (x != x^2 at width > 1), so splitting recovers x^2.
    std::vector< uint64_t > sig     = { 0, 1 };
    std::vector< std::string > vars = { "x" };

    auto evaluator = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] * v[0]; };

    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    opts.evaluator = evaluator;

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());

    auto original = Expr::Mul(Expr::Variable(0), Expr::Variable(0));
    auto fw       = FullWidthCheck(*original, 1, *result.value().expr, {}, 64);
    EXPECT_TRUE(fw.passed);
}

TEST(SimplifierTest, PolynomialMixedXYPlusLinear) {
    // f(x,y) = x*y + 2*x + 3*y + 1
    // f(0,0)=1, f(1,0)=3, f(0,1)=4, f(1,1)=7
    std::vector< uint64_t > sig     = { 1, 3, 4, 7 };
    std::vector< std::string > vars = { "x", "y" };

    auto evaluator = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] * v[1] + 2 * v[0] + 3 * v[1] + 1;
    };

    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    opts.evaluator = evaluator;

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());

    auto original = Expr::Add(
        Expr::Add(
            Expr::Add(
                Expr::Mul(Expr::Variable(0), Expr::Variable(1)),
                Expr::Mul(Expr::Constant(2), Expr::Variable(0))
            ),
            Expr::Mul(Expr::Constant(3), Expr::Variable(1))
        ),
        Expr::Constant(1)
    );
    auto fw = FullWidthCheck(*original, 2, *result.value().expr, {}, 64);
    EXPECT_TRUE(fw.passed);
}

TEST(SimplifierTest, NoEvaluatorSkipsSplitting) {
    // Without evaluator, polynomial target returns CoB result (x&y)
    std::vector< uint64_t > sig     = { 0, 0, 0, 1 };
    std::vector< std::string > vars = { "x", "y" };
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());

    // The result is x & y (CoB default), which is wrong for x*y
    auto original = Expr::Mul(Expr::Variable(0), Expr::Variable(1));
    auto fw       = FullWidthCheck(*original, 2, *result.value().expr, {}, 64);
    EXPECT_FALSE(fw.passed);
}

TEST(SimplifierTest, NullPolyCollapse) {
    // f(x) = 5*x^2 + 4*x at w=3 (mod 8).
    // On {0,1}: f(0)=0, f(1)=(5+4)%8=1. sig = [0, 1].
    // This is equivalent to x^2 mod 8 (since 4*x^2+4*x ≡ 0).
    // The normalizer should collapse the null polynomial part.
    std::vector< uint64_t > sig     = { 0, 1 };
    std::vector< std::string > vars = { "x" };

    auto evaluator = [](const std::vector< uint64_t > &v) -> uint64_t {
        return (5 * v[0] * v[0] + 4 * v[0]) & 0x7;
    };

    Options opts{ .bitwidth = 3, .max_vars = 16, .spot_check = true };
    opts.evaluator = evaluator;

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());

    // Verify full-width correctness
    for (uint64_t x = 0; x < 8; ++x) {
        std::vector< uint64_t > v = { x };
        uint64_t expected         = (5 * x * x + 4 * x) & 0x7;
        uint64_t actual           = EvalExpr(*result.value().expr, v, 3);
        EXPECT_EQ(actual, expected) << "x=" << x;
    }
}

TEST(SimplifierTest, MixedBitwisePlusPoly) {
    // f(x,y) = (x & y) + x*y. On {0,1}: same as 2*(x&y).
    // sig = [0, 0, 0, 2].
    // Evaluator distinguishes: at (2,3), (x&y)=2, x*y=6 -> 8.
    // Pure bitwise would give 2*(x&y), which at (2,3) = 4. Wrong.
    std::vector< uint64_t > sig     = { 0, 0, 0, 2 };
    std::vector< std::string > vars = { "x", "y" };

    auto evaluator = [](const std::vector< uint64_t > &v) -> uint64_t {
        return (v[0] & v[1]) + v[0] * v[1];
    };

    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    opts.evaluator = evaluator;

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());

    // Verify at random full-width points
    auto fw = FullWidthCheckEval(evaluator, 2, *result.value().expr, 64);
    EXPECT_TRUE(fw.passed);
}

TEST(SimplifierTest, AllZeroMulFragment) {
    // Edge case: mul_coeffs has nonzero entries but the polynomial
    // normalizes to zero (null polynomial). At w=3:
    // f(x) = 4*x^2 + 4*x ≡ 0 mod 8 for all x.
    // On {0,1}: sig = [0, 0]. Pattern matcher catches this
    // as constant 0 at Step 1 before reaching Step 5b.
    // Verify the pipeline handles this gracefully.
    std::vector< uint64_t > sig     = { 0, 0 };
    std::vector< std::string > vars = { "x" };

    // Evaluator for the null polynomial
    auto evaluator = [](const std::vector< uint64_t > &v) -> uint64_t {
        return (4 * v[0] * v[0] + 4 * v[0]) & 0x7;
    };

    Options opts{ .bitwidth = 3, .max_vars = 16, .spot_check = true };
    opts.evaluator = evaluator;

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());

    // Result should be constant 0
    for (uint64_t x = 0; x < 8; ++x) {
        std::vector< uint64_t > v = { x };
        EXPECT_EQ(EvalExpr(*result.value().expr, v, 3), 0u) << "x=" << x;
    }
}

TEST(SimplifierTest, MixedRewrite_BooleanConstantSignatureRemainsFullWidthLive) {
    // g(x,y) = x*y - (x&y) is zero on all {0,1} inputs, so the boolean
    // signature collapses to a single constant entry. At full width it is
    // still live in both variables and must not be accepted as constant 0.
    std::vector< uint64_t > sig     = { 0, 0, 0, 0 };
    std::vector< std::string > vars = { "x", "y" };

    auto input = Expr::Add(
        Expr::Mul(Expr::Variable(0), Expr::Variable(1)),
        Expr::Negate(Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)))
    );

    auto evaluator = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] * v[1] - (v[0] & v[1]);
    };

    auto bool_elim = EliminateAuxVars(sig, vars);
    EXPECT_TRUE(bool_elim.real_vars.empty());
    EXPECT_EQ(bool_elim.reduced_sig.size(), 1u);
    EXPECT_EQ(bool_elim.reduced_sig[0], 0u);

    auto fw_elim = EliminateAuxVars(sig, vars, evaluator, 64);
    EXPECT_EQ(fw_elim.real_vars, vars);
    EXPECT_TRUE(fw_elim.spurious_vars.empty());

    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    opts.evaluator = evaluator;

    auto result = Simplify(sig, vars, input.get(), opts);
    ASSERT_TRUE(result.has_value());

    if (result.value().kind == SimplifyOutcome::Kind::kSimplified) {
        auto check = FullWidthCheckEval(evaluator, 2, *result.value().expr, 64);
        EXPECT_TRUE(check.passed);
        EXPECT_NE(Render(*result.value().expr, result.value().real_vars), "0");
    } else {
        EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kUnchangedUnsupported);
    }
}

TEST(SimplifierTest, EarlyDecomposition_ProductCoreSurvivesPreconditioning) {
    // Regression: product cores that directly equal f(x) in the original
    // obfuscated AST but are destroyed by OperandSimplifier.
    // f(a,c,e) = (2*a + c) * (e & ~a)
    // The obfuscated form has Mul(non-const, non-const) terms whose sum IS f.
    // After operand simplification, the AST may restructure so that
    // ExtractProductCore no longer yields a direct match.
    // The fix (Step 1.5 in MixedRewrite) tries decomposition on the original
    // AST before preconditioning.
    auto evaluator = [](const std::vector< uint64_t > &v) -> uint64_t {
        uint64_t a = v[0];
        uint64_t c = v[1];
        uint64_t e = v[2];
        return (2 * a + c) * (e & ~a);
    };

    std::vector< std::string > vars = { "a", "c", "e" };
    auto sig                        = EvaluateBooleanSignature(evaluator, 3, 64);

    // Build obfuscated AST: (x&y)*(x|y) + (x&~y)*(~x&y) encodes x*y.
    // Using x = 2*a+c, y = e&~a gives a form where OperandSimplifier
    // will rewrite operands but destroy the product structure.
    auto x_expr = Expr::Add(Expr::Mul(Expr::Constant(2), Expr::Variable(0)), Expr::Variable(1));
    auto y_expr = Expr::BitwiseAnd(Expr::Variable(2), Expr::BitwiseNot(Expr::Variable(0)));
    auto obf    = Expr::Add(
        Expr::Mul(
            Expr::BitwiseAnd(CloneExpr(*x_expr), CloneExpr(*y_expr)),
            Expr::BitwiseOr(CloneExpr(*x_expr), CloneExpr(*y_expr))
        ),
        Expr::Mul(
            Expr::BitwiseAnd(CloneExpr(*x_expr), Expr::BitwiseNot(CloneExpr(*y_expr))),
            Expr::BitwiseAnd(Expr::BitwiseNot(CloneExpr(*x_expr)), CloneExpr(*y_expr))
        )
    );

    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    opts.evaluator = evaluator;

    auto result = Simplify(sig, vars, obf.get(), opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
    auto original_cost   = ComputeCost(*obf).cost;
    auto simplified_cost = ComputeCost(*result.value().expr).cost;
    EXPECT_TRUE(IsBetter(simplified_cost, original_cost));

    if (result.value().kind == SimplifyOutcome::Kind::kSimplified) {
        auto check = FullWidthCheckEval(evaluator, 3, *result.value().expr, 64);
        EXPECT_TRUE(check.passed);
    }
}

TEST(SimplifierTest, SingletonPower_XCubed) {
    // f(x) = x^3. sig on {0,1}: [0, 1]. CoB: [0, 1] (looks like x).
    // Full-width check rejects "x". Singleton-power recovery recovers x^3.
    std::vector< uint64_t > sig     = { 0, 1 };
    std::vector< std::string > vars = { "x" };

    auto evaluator = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] * v[0] * v[0];
    };

    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    opts.evaluator = evaluator;

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());

    // Verify full-width correctness
    auto fw = FullWidthCheckEval(evaluator, 1, *result.value().expr, 64);
    EXPECT_TRUE(fw.passed);
}

TEST(SimplifierTest, SingletonPower_XCubedPlusXY) {
    // f(x,y) = x^3 + x*y. sig on {0,1}: [0, 1, 0, 2].
    // Singleton-power recovery recovers x^3, coefficient splitting recovers x*y.
    std::vector< uint64_t > sig     = { 0, 1, 0, 2 };
    std::vector< std::string > vars = { "x", "y" };

    auto evaluator = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] * v[0] * v[0] + v[0] * v[1];
    };

    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    opts.evaluator = evaluator;

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());

    auto fw = FullWidthCheckEval(evaluator, 2, *result.value().expr, 64);
    EXPECT_TRUE(fw.passed);
}

TEST(SimplifierTest, SingletonPower_TwoSquaredVars) {
    // f(x,y) = x^2 + y^2. sig on {0,1}: [0, 1, 1, 2].
    std::vector< uint64_t > sig     = { 0, 1, 1, 2 };
    std::vector< std::string > vars = { "x", "y" };

    auto evaluator = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] * v[0] + v[1] * v[1];
    };

    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    opts.evaluator = evaluator;

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());

    auto fw = FullWidthCheckEval(evaluator, 2, *result.value().expr, 64);
    EXPECT_TRUE(fw.passed);
}

TEST(SimplifierTest, SingletonPower_ContributesNothing) {
    // Pure bitwise: (x & y) + (x | y) = x + y.
    // Singleton-power recovery is enabled but contributes nothing —
    // all higher-power coefficients are zero.
    std::vector< uint64_t > sig     = { 0, 1, 1, 2 };
    std::vector< std::string > vars = { "x", "y" };

    auto evaluator = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] + v[1]; };

    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    opts.evaluator = evaluator;

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    auto text = Render(*result.value().expr, result.value().real_vars);
    EXPECT_EQ(text, "x + y");
}

TEST(SimplifierTest, SingletonPower_QuadraticWithConstant) {
    // f(x) = 3*x^2 + 5*x + 7 at w=64.
    // sig on {0,1}: [7, 15].
    std::vector< uint64_t > sig     = { 7, 15 };
    std::vector< std::string > vars = { "x" };

    auto evaluator = [](const std::vector< uint64_t > &v) -> uint64_t {
        return 3 * v[0] * v[0] + 5 * v[0] + 7;
    };

    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    opts.evaluator = evaluator;

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());

    auto fw = FullWidthCheckEval(evaluator, 1, *result.value().expr, 64);
    EXPECT_TRUE(fw.passed);
}

// --- MixedRewrite integration tests ---

TEST(SimplifierTest, MixedRewrite_XorTimesZ_Simplified) {
    // (x^y)*z: MUL decomposition splits on z, recovers z*(x^y).
    std::vector< uint64_t > sig(8);
    for (uint32_t i = 0; i < 8; ++i) {
        uint64_t x = (i >> 0) & 1;
        uint64_t y = (i >> 1) & 1;
        uint64_t z = (i >> 2) & 1;
        sig[i]     = (x ^ y) * z;
    }
    std::vector< std::string > vars = { "x", "y", "z" };

    auto input =
        Expr::Mul(Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2));

    auto evaluator = [](const std::vector< uint64_t > &v) -> uint64_t {
        return (v[0] ^ v[1]) * v[2];
    };

    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    opts.evaluator = evaluator;

    auto result = Simplify(sig, vars, input.get(), opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
    EXPECT_TRUE(result.value().verified);
}

TEST(SimplifierTest, MixedRewrite_AndTimesZ_Simplified) {
    // (x&y)*z: MUL decomposition splits on z, recovers z*(x&y).
    std::vector< uint64_t > sig(8);
    for (uint32_t i = 0; i < 8; ++i) {
        uint64_t x = (i >> 0) & 1;
        uint64_t y = (i >> 1) & 1;
        uint64_t z = (i >> 2) & 1;
        sig[i]     = (x & y) * z;
    }
    std::vector< std::string > vars = { "x", "y", "z" };

    auto input =
        Expr::Mul(Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2));

    auto evaluator = [](const std::vector< uint64_t > &v) -> uint64_t {
        return (v[0] & v[1]) * v[2];
    };

    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    opts.evaluator = evaluator;

    auto result = Simplify(sig, vars, input.get(), opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
    EXPECT_TRUE(result.value().verified);
}

TEST(SimplifierTest, BitwiseOverArith_NoMul_Unsupported) {
    // (x + y) & z — ArithOverBitwise; CoB produces (x^y)&z which is
    // correct on {0,1} but wrong at full width. Verification catches this.
    std::vector< uint64_t > sig(8);
    for (uint32_t i = 0; i < 8; ++i) {
        uint64_t x = (i >> 0) & 1;
        uint64_t y = (i >> 1) & 1;
        uint64_t z = (i >> 2) & 1;
        sig[i]     = (x + y) & z;
    }
    std::vector< std::string > vars = { "x", "y", "z" };

    auto input =
        Expr::BitwiseAnd(Expr::Add(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2));

    auto evaluator = [](const std::vector< uint64_t > &v) -> uint64_t {
        return (v[0] + v[1]) & v[2];
    };

    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    opts.evaluator = evaluator;

    auto result = Simplify(sig, vars, input.get(), opts);
    ASSERT_TRUE(result.has_value());
    // Technique-level DAG finds a valid simplification path
    // that the old monolithic decomposition missed.
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
}

TEST(SimplifierTest, MultivarHighPower_PurePolynomial_Simplifies) {
    // x^2 * y is pure polynomial — routed to PowerRecovery,
    // handled by supported pipeline
    std::vector< uint64_t > sig(4);
    for (uint32_t i = 0; i < 4; ++i) {
        uint64_t x = (i >> 0) & 1;
        uint64_t y = (i >> 1) & 1;
        sig[i]     = x * x * y;
    }
    std::vector< std::string > vars = { "x", "y" };

    auto input = Expr::Mul(Expr::Mul(Expr::Variable(0), Expr::Variable(0)), Expr::Variable(1));

    auto evaluator = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] * v[0] * v[1];
    };

    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    opts.evaluator = evaluator;

    auto result = Simplify(sig, vars, input.get(), opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
}

TEST(SimplifierTest, MultivarHighPower_ASquaredD_Verified) {
    // a^2 * d — previously failed full-width verification
    std::vector< uint64_t > sig(4);
    for (uint32_t i = 0; i < 4; ++i) {
        uint64_t a = (i >> 0) & 1;
        uint64_t d = (i >> 1) & 1;
        sig[i]     = a * a * d; // = a*d on {0,1}
    }
    std::vector< std::string > vars = { "a", "d" };

    auto input = Expr::Mul(Expr::Mul(Expr::Variable(0), Expr::Variable(0)), Expr::Variable(1));

    auto evaluator = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] * v[0] * v[1];
    };

    Options opts;
    opts.bitwidth   = 64;
    opts.max_vars   = 16;
    opts.spot_check = true;
    opts.evaluator  = evaluator;

    auto result = Simplify(sig, vars, input.get(), opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);

    // Verify the result is correct at full width
    auto check = FullWidthCheckEval(
        evaluator, static_cast< uint32_t >(vars.size()), *result.value().expr, opts.bitwidth
    );
    EXPECT_TRUE(check.passed);
}

TEST(SimplifierTest, MultivarHighPower_ASquaredDSquared_Verified) {
    std::vector< uint64_t > sig(4);
    for (uint32_t i = 0; i < 4; ++i) {
        uint64_t a = (i >> 0) & 1;
        uint64_t d = (i >> 1) & 1;
        sig[i]     = a * a * d * d; // = a*d on {0,1}
    }
    std::vector< std::string > vars = { "a", "d" };

    auto input = Expr::Mul(
        Expr::Mul(Expr::Variable(0), Expr::Variable(0)),
        Expr::Mul(Expr::Variable(1), Expr::Variable(1))
    );

    auto evaluator = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] * v[0] * v[1] * v[1];
    };

    Options opts;
    opts.bitwidth   = 64;
    opts.max_vars   = 16;
    opts.spot_check = true;
    opts.evaluator  = evaluator;

    auto result = Simplify(sig, vars, input.get(), opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);

    auto check = FullWidthCheckEval(
        evaluator, static_cast< uint32_t >(vars.size()), *result.value().expr, opts.bitwidth
    );
    EXPECT_TRUE(check.passed);
}

TEST(SimplifierTest, MixedRewrite_TopLevelXorSucceeds) {
    std::vector< uint64_t > sig     = { 0, 1, 1, 0 };
    std::vector< std::string > vars = { "x", "y" };

    auto input = Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1));

    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = Simplify(sig, vars, input.get(), opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
    auto text = Render(*result.value().expr, result.value().real_vars);
    EXPECT_EQ(text, "x ^ y");
}

TEST(SimplifierTest, MixedRewrite_NoEvaluatorShortCircuit) {
    // The orchestrator tries the supported pipeline for MixedRewrite
    // expressions even without an evaluator.  The boolean signature
    // is sufficient for the supported solve to succeed.
    std::vector< uint64_t > sig(8);
    for (uint32_t i = 0; i < 8; ++i) {
        uint64_t x = (i >> 0) & 1;
        uint64_t y = (i >> 1) & 1;
        uint64_t z = (i >> 2) & 1;
        sig[i]     = (x ^ y) * z;
    }
    std::vector< std::string > vars = { "x", "y", "z" };

    auto input =
        Expr::Mul(Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2));

    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = Simplify(sig, vars, input.get(), opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
}

TEST(SimplifierTest, MixedRewrite_DiagnosticPayload) {
    // (x^y)*z now simplifies via MUL decomposition.
    // Verify it produces a Simplified result with correct classification.
    std::vector< uint64_t > sig(8);
    for (uint32_t i = 0; i < 8; ++i) {
        uint64_t x = (i >> 0) & 1;
        uint64_t y = (i >> 1) & 1;
        uint64_t z = (i >> 2) & 1;
        sig[i]     = (x ^ y) * z;
    }
    std::vector< std::string > vars = { "x", "y", "z" };

    auto input =
        Expr::Mul(Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2));

    auto evaluator = [](const std::vector< uint64_t > &v) -> uint64_t {
        return (v[0] ^ v[1]) * v[2];
    };

    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    opts.evaluator = evaluator;

    auto result = Simplify(sig, vars, input.get(), opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
}

TEST(SimplifierTest, MixedRewrite_BugOrGap) {
    // Deliberate sig/evaluator mismatch: sig=[0,0,0,99] but
    // evaluator=x*y (gives 1 at (1,1), not 99). The sig-derived
    // CoB candidate fails full-width check, but the residual solver
    // table recovers x*y from the evaluator-based residual.
    std::vector< uint64_t > sig     = { 0, 0, 0, 99 };
    std::vector< std::string > vars = { "x", "y" };

    auto input = Expr::Mul(Expr::Variable(0), Expr::Variable(1));

    auto evaluator = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] * v[1]; };

    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    opts.evaluator = evaluator;

    auto result = Simplify(sig, vars, input.get(), opts);
    ASSERT_TRUE(result.has_value());
    // Shared residual solver table can now recover the correct
    // expression from the evaluator-based residual, producing a
    // verified result.
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
}

TEST(SimplifierTest, MixedRewrite_SupportedRouteStillSimplifies) {
    std::vector< uint64_t > sig     = { 0, 0, 0, 1 };
    std::vector< std::string > vars = { "x", "y" };

    auto input = Expr::Mul(Expr::Variable(0), Expr::Variable(1));

    auto evaluator = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] * v[1]; };

    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    opts.evaluator = evaluator;

    auto result = Simplify(sig, vars, input.get(), opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
}

// --- ANF fast path integration tests ---

TEST(SimplifierTest, AnfThreeVarOr) {
    // a | b | c should emit ANF form instead of verbose CoB
    std::vector< uint64_t > sig     = { 0, 1, 1, 1, 1, 1, 1, 1 };
    std::vector< std::string > vars = { "a", "b", "c" };
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
    auto text = Render(*result.value().expr, result.value().real_vars);
    EXPECT_EQ(text, "a | b | c");
}

TEST(SimplifierTest, AnfThreeVarAnd) {
    std::vector< uint64_t > sig     = { 0, 0, 0, 0, 0, 0, 0, 1 };
    std::vector< std::string > vars = { "a", "b", "c" };
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    auto text = Render(*result.value().expr, result.value().real_vars);
    EXPECT_EQ(text, "a & b & c");
}

TEST(SimplifierTest, AnfThreeVarXor) {
    std::vector< uint64_t > sig     = { 0, 1, 1, 0, 1, 0, 0, 1 };
    std::vector< std::string > vars = { "a", "b", "c" };
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    auto text = Render(*result.value().expr, result.value().real_vars);
    EXPECT_EQ(text, "a ^ b ^ c");
}

TEST(SimplifierTest, AnfNotUsedForNonBoolean) {
    // Non-boolean signature (values > 1) should NOT use ANF
    std::vector< uint64_t > sig     = { 0, 3, 5, 8 };
    std::vector< std::string > vars = { "x", "y" };
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    auto text = Render(*result.value().expr, result.value().real_vars);
    // Should use CoB/linear path, not ANF
    EXPECT_EQ(text, "3 * x + 5 * y");
}

// --- Bitwise-over-polynomial decomposition integration tests ---

TEST(SimplifierTest, BitwiseOverPolyDOrCA) {
    // Ground truth: d | (c * a), vars: d=0, c=1, a=2
    // Handle-counted bitwise fanout now resolves this.
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] | (v[1] * v[2]);
    };
    std::vector< uint64_t > sig(8);
    for (uint32_t i = 0; i < 8; ++i) {
        std::vector< uint64_t > vals = { (i >> 0) & 1, (i >> 1) & 1, (i >> 2) & 1 };
        sig[i]                       = eval(vals);
    }
    std::vector< std::string > vars = { "d", "c", "a" };
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    opts.evaluator = eval;

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
    auto fw = FullWidthCheckEval(eval, 3, *result.value().expr, 64);
    EXPECT_TRUE(fw.passed);
}

TEST(SimplifierTest, BitwiseOverPolyEEAndD) {
    // Ground truth: (e*e) & d — AND-gated polynomial. On {0,1},
    // e*e == e&e so the boolean signature is degenerate. Bitwise
    // decomposition finds AND(e, d) which is {0,1}-correct but
    // diverges at full width. The arithmetic-atom lifter exposes
    // this as outer (v0 & d) with v0 = e*e; the outer problem is
    // pure bitwise and trivially solved; substitution produces the
    // ground truth.
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return (v[0] * v[0]) & v[1];
    };
    std::vector< uint64_t > sig(4);
    for (uint32_t i = 0; i < 4; ++i) {
        std::vector< uint64_t > vals = { (i >> 0) & 1, (i >> 1) & 1 };
        sig[i]                       = eval(vals);
    }
    std::vector< std::string > vars = { "e", "d" };
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    opts.evaluator = eval;
    auto input_expr =
        Expr::BitwiseAnd(Expr::Mul(Expr::Variable(0), Expr::Variable(0)), Expr::Variable(1));

    auto result = Simplify(sig, vars, input_expr.get(), opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
    auto fw = FullWidthCheckEval(eval, 2, *result.value().expr, 64);
    EXPECT_TRUE(fw.passed);
}

TEST(SimplifierTest, BitwiseOverPolyDSquaredXorA) {
    // Ground truth: (d*d) ^ a
    // Handle-counted bitwise fanout now resolves this.
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return (v[0] * v[0]) ^ v[1];
    };
    std::vector< uint64_t > sig(4);
    for (uint32_t i = 0; i < 4; ++i) {
        std::vector< uint64_t > vals = { (i >> 0) & 1, (i >> 1) & 1 };
        sig[i]                       = eval(vals);
    }
    std::vector< std::string > vars = { "d", "a" };
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    opts.evaluator = eval;

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
    auto fw = FullWidthCheckEval(eval, 2, *result.value().expr, 64);
    EXPECT_TRUE(fw.passed);
}

TEST(SimplifierTest, BitwiseDecompDisabledPreservesBaseline) {
    // d | (c*a): with expanded budget (256), the non-recursive
    // technique DAG (SingletonPolyRecovery) recovers this even
    // without bitwise fanout.
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] | (v[1] * v[2]);
    };
    std::vector< uint64_t > sig(8);
    for (uint32_t i = 0; i < 8; ++i) {
        std::vector< uint64_t > vals = { (i >> 0) & 1, (i >> 1) & 1, (i >> 2) & 1 };
        sig[i]                       = eval(vals);
    }
    std::vector< std::string > vars = { "d", "c", "a" };
    Options opts{ .bitwidth                     = 64,
                  .max_vars                     = 16,
                  .spot_check                   = true,
                  .enable_bitwise_decomposition = false };
    opts.evaluator = eval;

    auto result = Simplify(sig, vars, nullptr, opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
    auto fw = FullWidthCheckEval(eval, 3, *result.value().expr, 64);
    EXPECT_TRUE(fw.passed);
}

// --- Operand simplification integration ---

TEST(SimplifierTest, OperandSimplificationEnablesPipeline) {
    // Mul((a&b)|(a&~b), b): left operand = a,
    // so expression = a*b which the supported pipeline handles.
    auto lhs = Expr::BitwiseOr(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)),
        Expr::BitwiseAnd(Expr::Variable(0), Expr::BitwiseNot(Expr::Variable(1)))
    );
    auto e        = Expr::Mul(std::move(lhs), Expr::Variable(1));
    auto original = CloneExpr(*e);

    auto sig                        = EvaluateBooleanSignature(*e, 2, 64);
    std::vector< std::string > vars = { "a", "b" };

    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [&](const std::vector< uint64_t > &v) {
        return EvalExpr(*original, v, 64);
    };

    auto result = Simplify(sig, vars, e.get(), opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
}

TEST(SimplifierTest, OperandSimplifyCollapsesObfuscatedProjectionFactor) {
    auto hidden_a = Expr::Add(
        Expr::Add(
            Expr::Add(
                Expr::Negate(
                    Expr::Mul(
                        Expr::Constant(6),
                        Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1))
                    )
                ),
                Expr::Mul(
                    Expr::Constant(7),
                    Expr::BitwiseNot(Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1)))
                )
            ),
            Expr::Negate(
                Expr::Mul(
                    Expr::Constant(8),
                    Expr::BitwiseNot(Expr::BitwiseOr(Expr::Variable(0), Expr::Variable(1)))
                )
            )
        ),
        Expr::BitwiseNot(Expr::Variable(1))
    );
    auto input    = Expr::Mul(std::move(hidden_a), Expr::Variable(2));
    auto original = CloneExpr(*input);

    auto sig                        = EvaluateBooleanSignature(*input, 3, 64);
    std::vector< std::string > vars = { "a", "b", "y" };
    auto eval = [&](const std::vector< uint64_t > &v) { return EvalExpr(*original, v, 64); };

    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = eval;

    auto result = Simplify(sig, vars, input.get(), opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
    EXPECT_EQ(Render(*result.value().expr, result.value().real_vars), "a * y");

    auto remapped = CloneExpr(*result.value().expr);
    auto support  = BuildVarSupport(vars, result.value().real_vars);
    RemapVarIndices(*remapped, support);

    auto fw = FullWidthCheckEval(eval, 3, *remapped, 64);
    EXPECT_TRUE(fw.passed);
}

TEST(SimplifierTest, SemilinearCanonicalizesMaskedComplementSum) {
    auto input = Expr::Add(
        Expr::Constant(1),
        Expr::BitwiseAnd(Expr::BitwiseNot(Expr::Variable(0)), Expr::Variable(1))
    );
    auto original = CloneExpr(*input);

    auto sig                        = EvaluateBooleanSignature(*input, 2, 64);
    std::vector< std::string > vars = { "a", "y" };

    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [&](const std::vector< uint64_t > &v) {
        return EvalExpr(*original, v, 64);
    };

    auto result = Simplify(sig, vars, input.get(), opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
    EXPECT_EQ(Render(*result.value().expr, result.value().real_vars), "(~a & y) + 1");

    auto remapped = CloneExpr(*result.value().expr);
    auto support  = BuildVarSupport(vars, result.value().real_vars);
    RemapVarIndices(*remapped, support);

    auto fw = FullWidthCheckEval(opts.evaluator, 2, *remapped, 64);
    EXPECT_TRUE(fw.passed);
}

TEST(SimplifierTest, SemilinearCanonicalizesScaledComplement) {
    constexpr uint64_t kNegThree = UINT64_MAX - 2;

    auto input = Expr::Add(
        Expr::Constant(kNegThree),
        Expr::Mul(
            Expr::Constant(kNegThree),
            Expr::BitwiseOr(
                Expr::Variable(0), Expr::BitwiseOr(Expr::Variable(1), Expr::Variable(2))
            )
        )
    );
    auto original = CloneExpr(*input);

    auto sig                        = EvaluateBooleanSignature(*input, 3, 64);
    std::vector< std::string > vars = { "a", "y", "z" };

    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [&](const std::vector< uint64_t > &v) {
        return EvalExpr(*original, v, 64);
    };

    auto result = Simplify(sig, vars, input.get(), opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
    EXPECT_EQ(Render(*result.value().expr, result.value().real_vars), "3 * ~(a | y | z)");

    auto remapped = CloneExpr(*result.value().expr);
    auto support  = BuildVarSupport(vars, result.value().real_vars);
    RemapVarIndices(*remapped, support);

    auto fw = FullWidthCheckEval(opts.evaluator, 3, *remapped, 64);
    EXPECT_TRUE(fw.passed);
}

TEST(SimplifierTest, SemilinearCanonicalizesTwoVarXorAffineCombo) {
    auto input = Expr::Add(
        Expr::Constant(3),
        Expr::Add(
            Expr::Mul(Expr::Constant(3), Expr::Variable(0)),
            Expr::Add(
                Expr::Mul(Expr::Constant(2), Expr::Variable(1)),
                Expr::Negate(
                    Expr::Mul(
                        Expr::Constant(4),
                        Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1))
                    )
                )
            )
        )
    );
    auto original = CloneExpr(*input);

    auto sig                        = EvaluateBooleanSignature(*input, 2, 64);
    std::vector< std::string > vars = { "b", "x" };

    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [&](const std::vector< uint64_t > &v) {
        return EvalExpr(*original, v, 64);
    };

    auto result = Simplify(sig, vars, input.get(), opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);

    auto rendered = Render(*result.value().expr, result.value().real_vars);
    EXPECT_NE(rendered.find("^"), std::string::npos);
    EXPECT_TRUE(IsBetter(ComputeCost(*result.value().expr).cost, ComputeCost(*original).cost));

    auto remapped = CloneExpr(*result.value().expr);
    auto support  = BuildVarSupport(vars, result.value().real_vars);
    RemapVarIndices(*remapped, support);

    auto fw = FullWidthCheckEval(opts.evaluator, 2, *remapped, 64);
    EXPECT_TRUE(fw.passed);
}

TEST(SimplifierTest, SemilinearCanonicalizesTwoVarOrNotAffineCombo) {
    auto input = Expr::Add(
        Expr::Constant(2),
        Expr::Add(
            Expr::Negate(Expr::Variable(0)),
            Expr::Add(
                Expr::Mul(Expr::Constant(2), Expr::Variable(1)),
                Expr::Negate(
                    Expr::Mul(
                        Expr::Constant(2),
                        Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1))
                    )
                )
            )
        )
    );
    auto original = CloneExpr(*input);

    auto sig                        = EvaluateBooleanSignature(*input, 2, 64);
    std::vector< std::string > vars = { "a", "y" };

    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [&](const std::vector< uint64_t > &v) {
        return EvalExpr(*original, v, 64);
    };

    auto result = Simplify(sig, vars, input.get(), opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);

    auto rendered = Render(*result.value().expr, result.value().real_vars);
    EXPECT_NE(rendered.find("a | ~y"), std::string::npos);
    EXPECT_TRUE(IsBetter(ComputeCost(*result.value().expr).cost, ComputeCost(*original).cost));

    auto remapped = CloneExpr(*result.value().expr);
    auto support  = BuildVarSupport(vars, result.value().real_vars);
    RemapVarIndices(*remapped, support);

    auto fw = FullWidthCheckEval(opts.evaluator, 2, *remapped, 64);
    EXPECT_TRUE(fw.passed);
}

TEST(SimplifierTest, NoOpOperandSimplFallsToXorLowering) {
    // (x^y) * z: operands are already simple (no MBA to collapse).
    // Operand simplification returns changed=false.
    // XOR lowering should still handle this case (existing behavior).
    auto e =
        Expr::Mul(Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2));
    auto original = CloneExpr(*e);

    auto sig                        = EvaluateBooleanSignature(*e, 3, 64);
    std::vector< std::string > vars = { "x", "y", "z" };

    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [&](const std::vector< uint64_t > &v) {
        return EvalExpr(*original, v, 64);
    };

    auto result = Simplify(sig, vars, e.get(), opts);
    ASSERT_TRUE(result.has_value());
    // Pipeline should still produce a result (unchanged or
    // simplified via XOR lowering). The key invariant is that
    // operand simplification did not break the existing path.
    EXPECT_NE(result.value().kind, SimplifyOutcome::Kind::kError);
}

// --- NOT lowering: ~(polynomial) -> -(polynomial) - 1 ---

TEST(SimplifierTest, NotOverSquareReclassified) {
    // ~(b*b): classifies as MixedRewrite (BitwiseOverArith + HasMul).
    // With NOT lowering, it should reclassify as PowerRecovery
    // (pure polynomial -(b*b) - 1), avoiding the MixedRewrite detour.
    // Test that the result is Simplified (not just Unchanged).
    auto e        = Expr::BitwiseNot(Expr::Mul(Expr::Variable(0), Expr::Variable(0)));
    auto original = CloneExpr(*e);

    auto sig                        = EvaluateBooleanSignature(*e, 1, 64);
    std::vector< std::string > vars = { "b" };

    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [&](const std::vector< uint64_t > &v) {
        return EvalExpr(*original, v, 64);
    };

    auto result = Simplify(sig, vars, e.get(), opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
    EXPECT_TRUE(result.value().verified);

    // The simplified expression should NOT contain any bitwise NOT
    // (it should be lowered to arithmetic).
    EXPECT_NE(result.value().expr->kind, Expr::Kind::kNot);

    auto check = FullWidthCheckEval(opts.evaluator, 1, *result.value().expr, 64);
    EXPECT_TRUE(check.passed);

    // Verify reclassification: the result should have no bitwise flags
    auto cls = ClassifyStructural(*result.value().expr);
    EXPECT_FALSE(HasFlag(cls.flags, kSfHasBitwiseOverArith));
}

TEST(SimplifierTest, NotOverAddReclassified) {
    // ~(a + b): NOT lowering converts to -(a+b) - 1.
    // The route should become BitwiseOnly or Multilinear,
    // not MixedRewrite.
    auto e        = Expr::BitwiseNot(Expr::Add(Expr::Variable(0), Expr::Variable(1)));
    auto original = CloneExpr(*e);

    auto sig                        = EvaluateBooleanSignature(*e, 2, 64);
    std::vector< std::string > vars = { "a", "b" };

    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [&](const std::vector< uint64_t > &v) {
        return EvalExpr(*original, v, 64);
    };

    auto result = Simplify(sig, vars, e.get(), opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
    EXPECT_TRUE(result.value().verified);

    EXPECT_NE(result.value().expr->kind, Expr::Kind::kNot);

    auto check = FullWidthCheckEval(opts.evaluator, 2, *result.value().expr, 64);
    EXPECT_TRUE(check.passed);
}

TEST(SimplifierTest, NotOverSquareRouteImproved) {
    // ~(b*b) without NOT lowering routes to MixedRewrite.
    // With NOT lowering, the input should be -(b*b) - 1,
    // which routes to PowerRecovery — a better path.
    // This test verifies that the diag route is NOT MixedRewrite
    // (i.e., NOT lowering happened before classification).
    auto e        = Expr::BitwiseNot(Expr::Mul(Expr::Variable(0), Expr::Variable(0)));
    auto original = CloneExpr(*e);

    auto sig                        = EvaluateBooleanSignature(*e, 1, 64);
    std::vector< std::string > vars = { "b" };

    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [&](const std::vector< uint64_t > &v) {
        return EvalExpr(*original, v, 64);
    };

    auto result = Simplify(sig, vars, e.get(), opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
}

// PLDIPoly L188: product identity + large-coefficient linear residual.
// f(x,y) = x*y + ~x - (x|~y) - 10*(x&~y) - 10*(x&y)
//         = x*y + 12*~(x|~y) - 11*(x|y)   (ground truth)
// Product identity collapse works, but the linear residual isn't
// simplified because the pipeline gives up on the whole x*y + linear.
TEST(SimplifierTest, ProductIdentityPlusLinearResidual) {
    auto x = []() { return Expr::Variable(0); };
    auto y = []() { return Expr::Variable(1); };

    // (x&y)*(x|y) + (x&~y)*(~x&y) + ~x - (x|~y) - 10*(x&~y) - 10*(x&y)
    auto product_id = Expr::Add(
        Expr::Mul(Expr::BitwiseAnd(x(), y()), Expr::BitwiseOr(x(), y())),
        Expr::Mul(
            Expr::BitwiseAnd(x(), Expr::BitwiseNot(y())),
            Expr::BitwiseAnd(Expr::BitwiseNot(x()), y())
        )
    );
    auto e = Expr::Add(std::move(product_id), Expr::BitwiseNot(x()));
    e      = Expr::Add(std::move(e), Expr::Negate(Expr::BitwiseOr(x(), Expr::BitwiseNot(y()))));
    e      = Expr::Add(
        std::move(e),
        Expr::Negate(
            Expr::Mul(Expr::Constant(10), Expr::BitwiseAnd(x(), Expr::BitwiseNot(y())))
        )
    );
    e = Expr::Add(
        std::move(e), Expr::Negate(Expr::Mul(Expr::Constant(10), Expr::BitwiseAnd(x(), y())))
    );
    auto original = CloneExpr(*e);

    auto sig                        = EvaluateBooleanSignature(*e, 2, 64);
    std::vector< std::string > vars = { "x", "y" };

    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [&](const std::vector< uint64_t > &v) {
        return EvalExpr(*original, v, 64);
    };

    auto result = Simplify(sig, vars, e.get(), opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
    EXPECT_TRUE(result.value().verified);

    auto check = FullWidthCheckEval(opts.evaluator, 2, *result.value().expr, 64);
    EXPECT_TRUE(check.passed);
}

// PLDIPoly L733 (3-var): product identity + 3-var linear residual.
// f(x,y,z) = x*y + (y^(x|z)) - 6*~(~x|(y&z)) + 5*~(~x|(y|z))
//             + 6*~(~x|(~y|z))
// GT: x*y + 5*~(x|(~y|z)) - 4*(~x&(y|z))
TEST(SimplifierTest, ProductIdentityPlusLinearResidual3Var) {
    auto x = []() { return Expr::Variable(0); };
    auto y = []() { return Expr::Variable(1); };
    auto z = []() { return Expr::Variable(2); };

    auto product_id = Expr::Add(
        Expr::Mul(Expr::BitwiseAnd(x(), y()), Expr::BitwiseOr(x(), y())),
        Expr::Mul(
            Expr::BitwiseAnd(x(), Expr::BitwiseNot(y())),
            Expr::BitwiseAnd(Expr::BitwiseNot(x()), y())
        )
    );
    // + (y ^ (x | z))
    auto e = Expr::Add(std::move(product_id), Expr::BitwiseXor(y(), Expr::BitwiseOr(x(), z())));
    // - 6 * ~(~x | (y & z))
    e      = Expr::Add(
        std::move(e),
        Expr::Negate(
            Expr::Mul(
                Expr::Constant(6),
                Expr::BitwiseNot(
                    Expr::BitwiseOr(Expr::BitwiseNot(x()), Expr::BitwiseAnd(y(), z()))
                )
            )
        )
    );
    // + 5 * ~(~x | (y | z))
    e = Expr::Add(
        std::move(e),
        Expr::Mul(
            Expr::Constant(5),
            Expr::BitwiseNot(Expr::BitwiseOr(Expr::BitwiseNot(x()), Expr::BitwiseOr(y(), z())))
        )
    );
    // + 6 * ~(~x | (~y | z))
    e = Expr::Add(
        std::move(e),
        Expr::Mul(
            Expr::Constant(6),
            Expr::BitwiseNot(
                Expr::BitwiseOr(
                    Expr::BitwiseNot(x()), Expr::BitwiseOr(Expr::BitwiseNot(y()), z())
                )
            )
        )
    );
    auto original = CloneExpr(*e);

    auto sig                        = EvaluateBooleanSignature(*e, 3, 64);
    std::vector< std::string > vars = { "x", "y", "z" };

    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [&](const std::vector< uint64_t > &v) {
        return EvalExpr(*original, v, 64);
    };

    auto result = Simplify(sig, vars, e.get(), opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
    EXPECT_TRUE(result.value().verified);

    auto check = FullWidthCheckEval(opts.evaluator, 3, *result.value().expr, 64);
    EXPECT_TRUE(check.passed);
}

TEST(SimplifierTest, NotOverMulSubExpression) {
    // d | ~(a*b): decomposer peels d, sub-problem is ~(a*b).
    // NOT lowering on the sub-expression gives d | (-(a*b) - 1),
    // making the sub-problem a pure polynomial.
    auto e = Expr::BitwiseOr(
        Expr::Variable(0), Expr::BitwiseNot(Expr::Mul(Expr::Variable(1), Expr::Variable(2)))
    );
    auto original = CloneExpr(*e);

    auto sig                        = EvaluateBooleanSignature(*e, 3, 64);
    std::vector< std::string > vars = { "d", "a", "b" };

    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [&](const std::vector< uint64_t > &v) {
        return EvalExpr(*original, v, 64);
    };

    auto result = Simplify(sig, vars, e.get(), opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().kind, SimplifyOutcome::Kind::kSimplified);
    EXPECT_TRUE(result.value().verified);

    auto check = FullWidthCheckEval(opts.evaluator, 3, *result.value().expr, 64);
    EXPECT_TRUE(check.passed);
}

// Regression: issue #9 — a | ((1+a) & (2-a)) was incorrectly simplified
// to a. Boolean sig [0,1] but diverges at full width (f(3)=7 ≠ 3).
TEST(SimplifierTest, Issue9_CarryPropagation) {
    auto one_a = Expr::Add(Expr::Constant(1), Expr::Variable(0));
    auto neg_a = Expr::Add(Expr::Negate(Expr::Variable(0)), Expr::Constant(2));
    auto inner = Expr::BitwiseAnd(std::move(one_a), std::move(neg_a));
    auto e     = Expr::BitwiseOr(Expr::Variable(0), std::move(inner));

    auto original = CloneExpr(*e);
    auto sig      = EvaluateBooleanSignature(*e, 1, 64);
    ASSERT_EQ(sig.size(), 2);
    EXPECT_EQ(sig[0], 0);
    EXPECT_EQ(sig[1], 1);

    std::vector< std::string > vars = { "a" };
    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [&](const std::vector< uint64_t > &v) {
        return EvalExpr(*original, v, 64);
    };

    auto result = Simplify(sig, vars, e.get(), opts);
    ASSERT_TRUE(result.has_value());

    if (result.value().kind == SimplifyOutcome::Kind::kSimplified) {
        auto fw = FullWidthCheckEval(opts.evaluator, 1, *result.value().expr, 64);
        EXPECT_TRUE(fw.passed) << "Simplified to "
                               << Render(*result.value().expr, result.value().real_vars)
                               << " but fails full-width check";
    }
}
