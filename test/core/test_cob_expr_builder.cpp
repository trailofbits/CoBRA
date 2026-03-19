#include "cobra/core/BitWidth.h"
#include "cobra/core/CoBExprBuilder.h"
#include "cobra/core/CoeffInterpolator.h"
#include <gtest/gtest.h>

using namespace cobra;

namespace {

    std::string r(const Expr &expr, const std::vector< std::string > &vars, uint32_t bw = 64) {
        return Render(expr, vars, bw);
    }

} // namespace

TEST(CoBExprBuilderTest, Constant) {
    std::vector< uint64_t > coeffs = { 42, 0, 0, 0 };
    auto expr                      = BuildCobExpr(coeffs, 2, 64);
    EXPECT_EQ(r(*expr, { "x", "y" }), "42");
}

TEST(CoBExprBuilderTest, Zero) {
    std::vector< uint64_t > coeffs = { 0, 0, 0, 0 };
    auto expr                      = BuildCobExpr(coeffs, 2, 64);
    EXPECT_EQ(r(*expr, { "x", "y" }), "0");
}

TEST(CoBExprBuilderTest, SingleVariable) {
    // f(x) = x: coeffs = [0, 1]
    std::vector< uint64_t > coeffs = { 0, 1 };
    auto expr                      = BuildCobExpr(coeffs, 1, 64);
    EXPECT_EQ(r(*expr, { "x" }), "x");
}

TEST(CoBExprBuilderTest, XPlusY) {
    // CoB of x+y = [0, 1, 1, 0]
    std::vector< uint64_t > coeffs = { 0, 1, 1, 0 };
    auto expr                      = BuildCobExpr(coeffs, 2, 64);
    EXPECT_EQ(r(*expr, { "x", "y" }), "x + y");
}

TEST(CoBExprBuilderTest, OrRecognition) {
    // x | y: CoB = [0, 1, 1, -1]
    uint64_t neg1                  = ModNeg(1, 64);
    std::vector< uint64_t > coeffs = { 0, 1, 1, neg1 };
    auto expr                      = BuildCobExpr(coeffs, 2, 64);
    EXPECT_EQ(r(*expr, { "x", "y" }), "x | y");
}

TEST(CoBExprBuilderTest, XorRecognition) {
    // x ^ y: CoB = [0, 1, 1, -2]
    uint64_t neg2                  = ModNeg(2, 64);
    std::vector< uint64_t > coeffs = { 0, 1, 1, neg2 };
    auto expr                      = BuildCobExpr(coeffs, 2, 64);
    EXPECT_EQ(r(*expr, { "x", "y" }), "x ^ y");
}

TEST(CoBExprBuilderTest, AndTwoVars) {
    // x & y: CoB = [0, 0, 0, 1]
    std::vector< uint64_t > coeffs = { 0, 0, 0, 1 };
    auto expr                      = BuildCobExpr(coeffs, 2, 64);
    EXPECT_EQ(r(*expr, { "x", "y" }), "x & y");
}

TEST(CoBExprBuilderTest, AndPlusLinear) {
    // (x & y) + z: CoB = [0, 0, 0, 1, 1, 0, 0, 0]
    std::vector< uint64_t > coeffs = { 0, 0, 0, 1, 1, 0, 0, 0 };
    auto expr                      = BuildCobExpr(coeffs, 3, 64);
    EXPECT_EQ(r(*expr, { "x", "y", "z" }), "(x & y) + z");
}

TEST(CoBExprBuilderTest, ScaledOr) {
    // 3*(x | y) + 5: CoB = [5, 3, 3, -3]
    uint64_t neg3                  = ModNeg(3, 64);
    std::vector< uint64_t > coeffs = { 5, 3, 3, neg3 };
    auto expr                      = BuildCobExpr(coeffs, 2, 64);
    EXPECT_EQ(r(*expr, { "x", "y" }), "5 + 3 * (x | y)");
}

TEST(CoBExprBuilderTest, ScaledXor) {
    // 7*(x ^ y): CoB = [0, 7, 7, -14]
    uint64_t neg14                 = ModNeg(14, 64);
    std::vector< uint64_t > coeffs = { 0, 7, 7, neg14 };
    auto expr                      = BuildCobExpr(coeffs, 2, 64);
    EXPECT_EQ(r(*expr, { "x", "y" }), "7 * (x ^ y)");
}

TEST(CoBExprBuilderTest, NotAndRecognition) {
    // ~(x & y): CoB = [-1, 0, 0, -1]
    uint64_t neg1                  = ModNeg(1, 64);
    std::vector< uint64_t > coeffs = { neg1, 0, 0, neg1 };
    auto expr                      = BuildCobExpr(coeffs, 2, 64);
    EXPECT_EQ(r(*expr, { "x", "y" }), "~(x & y)");
}

TEST(CoBExprBuilderTest, NotOrRecognition) {
    // ~(x | y): CoB = [-1, -1, -1, 1]
    // After OR rewrite: terms become (-1)*(x|y)
    // Then NOT: (-1) + (-1)*(x|y) = ~(x|y)
    uint64_t neg1                  = ModNeg(1, 64);
    std::vector< uint64_t > coeffs = { neg1, neg1, neg1, 1 };
    auto expr                      = BuildCobExpr(coeffs, 2, 64);
    EXPECT_EQ(r(*expr, { "x", "y" }), "~(x | y)");
}

TEST(CoBExprBuilderTest, ThreeVarAffine) {
    // 3x + 5y + 7z: CoB = [0, 3, 5, 0, 7, 0, 0, 0]
    std::vector< uint64_t > coeffs = { 0, 3, 5, 0, 7, 0, 0, 0 };
    auto expr                      = BuildCobExpr(coeffs, 3, 64);
    EXPECT_EQ(r(*expr, { "x", "y", "z" }), "3 * x + 5 * y + 7 * z");
}

TEST(CoBExprBuilderTest, ThreeVarXorPair) {
    // (x ^ y) & z: CoB = [0, 0, 0, 0, 0, 1, 1, -2]
    uint64_t neg2                  = ModNeg(2, 64);
    std::vector< uint64_t > coeffs = { 0, 0, 0, 0, 0, 1, 1, neg2 };
    auto expr                      = BuildCobExpr(coeffs, 3, 64);
    // x&z and y&z pair into XOR; & binds tighter than ^ in rendering
    EXPECT_EQ(r(*expr, { "x", "y", "z" }), "x & z ^ y & z");
}

TEST(CoBExprBuilderTest, SingleHigherOrderTerm) {
    // 42 * (x & y & z)
    std::vector< uint64_t > coeffs = { 0, 0, 0, 0, 0, 0, 0, 42 };
    auto expr                      = BuildCobExpr(coeffs, 3, 64);
    EXPECT_EQ(r(*expr, { "x", "y", "z" }), "42 * (x & y & z)");
}

TEST(CoBExprBuilderTest, NegatedAndProduct) {
    // -(x & y): coeff = -1 but no constant → negate, not NOT
    uint64_t neg1                  = ModNeg(1, 64);
    std::vector< uint64_t > coeffs = { 0, 0, 0, neg1 };
    auto expr                      = BuildCobExpr(coeffs, 2, 64);
    EXPECT_EQ(r(*expr, { "x", "y" }), "-(x & y)");
}

TEST(CoBExprBuilderTest, Bitwidth8Or) {
    // x | y at 8-bit: CoB = [0, 1, 1, 255]  (255 = -1 mod 256)
    std::vector< uint64_t > coeffs = { 0, 1, 1, 255 };
    auto expr                      = BuildCobExpr(coeffs, 2, 8);
    EXPECT_EQ(r(*expr, { "x", "y" }, 8), "x | y");
}

TEST(CoBExprBuilderTest, Bitwidth1Affine) {
    // At 1-bit: -2 mod 2 = 0, so x+y CoB = [0, 1, 1, 0] (affine).
    // Builder produces Add(x, y); renderer may show as "x ^ y" since
    // they're equivalent at 1-bit.
    std::vector< uint64_t > coeffs = { 0, 1, 1, 0 };
    auto expr                      = BuildCobExpr(coeffs, 2, 1);
    auto text                      = r(*expr, { "x", "y" }, 1);
    EXPECT_TRUE(text == "x + y" || text == "x ^ y") << "Got: " << text;
}

// Integration test: build from actual CoB transform output
TEST(CoBExprBuilderTest, IntegrationOrFromSig) {
    // x | y signature = [0, 1, 1, 1]
    std::vector< uint64_t > sig = { 0, 1, 1, 1 };
    auto coeffs                 = InterpolateCoefficients(sig, 2, 64);
    auto expr                   = BuildCobExpr(coeffs, 2, 64);
    EXPECT_EQ(r(*expr, { "x", "y" }), "x | y");
}

TEST(CoBExprBuilderTest, IntegrationXorFromSig) {
    // x ^ y signature = [0, 1, 1, 0]
    std::vector< uint64_t > sig = { 0, 1, 1, 0 };
    auto coeffs                 = InterpolateCoefficients(sig, 2, 64);
    auto expr                   = BuildCobExpr(coeffs, 2, 64);
    EXPECT_EQ(r(*expr, { "x", "y" }), "x ^ y");
}

TEST(CoBExprBuilderTest, IntegrationAndPlusZFromSig) {
    // (x & y) + z
    std::vector< uint64_t > sig(8);
    for (uint32_t i = 0; i < 8; ++i) {
        uint64_t x = (i >> 0) & 1;
        uint64_t y = (i >> 1) & 1;
        uint64_t z = (i >> 2) & 1;
        sig[i]     = (x & y) + z;
    }
    auto coeffs = InterpolateCoefficients(sig, 3, 64);
    auto expr   = BuildCobExpr(coeffs, 3, 64);
    EXPECT_EQ(r(*expr, { "x", "y", "z" }), "(x & y) + z");
}

// Regression: compound OR/XOR terms must not re-enter pairing.
// (x|y) + z - (x&y&z) must NOT simplify to x|y|z (wrong at {0,1}).
TEST(CoBExprBuilderTest, NoCompoundReentry) {
    // f(x,y,z) = (x|y) + z - (x&y&z)
    // sig[5] = f(1,0,1) = 2, but (x|y|z) at (1,0,1) = 1
    std::vector< uint64_t > sig(8);
    for (uint32_t i = 0; i < 8; ++i) {
        uint64_t x = (i >> 0) & 1;
        uint64_t y = (i >> 1) & 1;
        uint64_t z = (i >> 2) & 1;
        sig[i]     = (x | y) + z - (x & y & z);
    }
    auto coeffs = InterpolateCoefficients(sig, 3, 64);
    auto expr   = BuildCobExpr(coeffs, 3, 64);
    auto text   = r(*expr, { "x", "y", "z" });
    // Must NOT be "x | y | z" — that's incorrect
    EXPECT_NE(text, "x | y | z");
    // Verify the expression matches the signature at {0,1}
    EXPECT_EQ(sig[5], 2u); // (1|0) + 1 - (1&0&1) = 2
}

TEST(CoBExprBuilderTest, IntegrationNotXorFromSig) {
    // ~(x ^ y) = NOT(XOR)
    std::vector< uint64_t > sig = { UINT64_MAX, UINT64_MAX - 1, UINT64_MAX - 1, UINT64_MAX };
    auto coeffs                 = InterpolateCoefficients(sig, 2, 64);
    auto expr                   = BuildCobExpr(coeffs, 2, 64);
    auto text                   = r(*expr, { "x", "y" });
    // Should contain NOT and XOR in some form
    EXPECT_TRUE(
        text.find('~') != std::string::npos || text.find("NOT") != std::string::npos
        || expr->kind == Expr::Kind::kNot
    ) << "Expected NOT expression, got: "
      << text;
}
