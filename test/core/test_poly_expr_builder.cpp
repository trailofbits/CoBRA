#include "cobra/core/BitWidth.h"
#include "cobra/core/PolyExprBuilder.h"
#include "cobra/core/SignatureChecker.h"
#include <gtest/gtest.h>

using namespace cobra;

namespace {

    MonomialKey make_exp(std::initializer_list< uint8_t > exps) {
        auto data = exps.begin();
        return MonomialKey::FromExponents(data, static_cast< uint8_t >(exps.size()));
    }

} // namespace

TEST(PolyExprBuilderTest, EmptyReturnsZero) {
    NormalizedPoly np{ 2, 8, {} };
    auto result = BuildPolyExpr(np);
    ASSERT_TRUE(result.has_value());
    auto &expr = result.value();
    ASSERT_NE(expr, nullptr);
    EXPECT_EQ(expr->kind, Expr::Kind::kConstant);
    EXPECT_EQ(expr->constant_val, 0u);
}

TEST(PolyExprBuilderTest, LinearTerm) {
    // x_(1) with coeff 5 -> 5*x in monomial
    NormalizedPoly np{ 1, 64, { { make_exp({ 1 }), 5 } } };
    auto r = BuildPolyExpr(np);
    ASSERT_TRUE(r.has_value());
    auto &expr = r.value();

    // Evaluate at x=7
    std::vector< uint64_t > vals = { 7 };
    uint64_t result              = EvalExpr(*expr, vals, 64);
    EXPECT_EQ(result, 35u);
}

TEST(PolyExprBuilderTest, ReintroducedLinearMass) {
    // 1 * x_(2) -> x^2 - x in monomial basis
    NormalizedPoly np{ 1, 64, { { make_exp({ 2 }), 1 } } };
    auto r = BuildPolyExpr(np);
    ASSERT_TRUE(r.has_value());
    auto &expr = r.value();

    // x_(2) = x(x-1) -> x=5: 5*4=20, x=0: 0, x=1: 0
    std::vector< uint64_t > v5 = { 5 };
    EXPECT_EQ(EvalExpr(*expr, v5, 64), 20u);
    std::vector< uint64_t > v0 = { 0 };
    EXPECT_EQ(EvalExpr(*expr, v0, 64), 0u);
    std::vector< uint64_t > v1 = { 1 };
    EXPECT_EQ(EvalExpr(*expr, v1, 64), 0u);
}

TEST(PolyExprBuilderTest, XSquaredNormalized) {
    // x^2 normalized: x_(1) + x_(2)
    // Monomial: x_(1) -> x, x_(2) -> x^2-x -> sum = x^2
    NormalizedPoly np{
        1, 64, { { make_exp({ 1 }), 1 }, { make_exp({ 2 }), 1 } }
    };
    auto r = BuildPolyExpr(np);
    ASSERT_TRUE(r.has_value());
    auto &expr = r.value();

    for (uint64_t x = 0; x < 10; ++x) {
        std::vector< uint64_t > v = { x };
        EXPECT_EQ(EvalExpr(*expr, v, 64), x * x);
    }
}

TEST(PolyExprBuilderTest, MultivariateXY) {
    // x*y = tuple (1,1), coeff 1
    NormalizedPoly np{ 2, 64, { { make_exp({ 1, 1 }), 1 } } };
    auto r = BuildPolyExpr(np);
    ASSERT_TRUE(r.has_value());
    auto &expr = r.value();

    std::vector< uint64_t > v = { 3, 7 };
    EXPECT_EQ(EvalExpr(*expr, v, 64), 21u);
}

TEST(PolyExprBuilderTest, MultivariateSquaredVar) {
    // x_(2)*y_(1) with coeff 1
    // -> C_3: (2,1) splits to (-1 at (1,1)) + (1 at (2,1))
    // Monomial: -x*y + x^2*y = x*y*(x-1)
    NormalizedPoly np{ 2, 64, { { make_exp({ 2, 1 }), 1 } } };
    auto r = BuildPolyExpr(np);
    ASSERT_TRUE(r.has_value());
    auto &expr = r.value();

    for (uint64_t x = 0; x < 5; ++x) {
        for (uint64_t y = 0; y < 5; ++y) {
            std::vector< uint64_t > v = { x, y };
            uint64_t expected         = x * (x - 1) * y;
            EXPECT_EQ(EvalExpr(*expr, v, 64), expected) << "x=" << x << " y=" << y;
        }
    }
}

TEST(PolyExprBuilderTest, ConstantTerm) {
    // Constant: all-zero exponent tuple
    NormalizedPoly np{ 1, 8, { { make_exp({ 0 }), 42 } } };
    auto r = BuildPolyExpr(np);
    ASSERT_TRUE(r.has_value());
    auto &expr = r.value();

    std::vector< uint64_t > v = { 7 };
    EXPECT_EQ(EvalExpr(*expr, v, 8), 42u);
}

TEST(PolyExprBuilderTest, Bitwidth8Wrapping) {
    // 200 * x_(1) at w=8 -> 200*x mod 256
    NormalizedPoly np{ 1, 8, { { make_exp({ 1 }), 200 } } };
    auto r = BuildPolyExpr(np);
    ASSERT_TRUE(r.has_value());
    auto &expr = r.value();

    std::vector< uint64_t > v = { 2 };
    EXPECT_EQ(EvalExpr(*expr, v, 8), (200 * 2) & 0xFF);
}

TEST(PolyExprBuilderTest, DeterministicOutput) {
    // Same input -> same evaluation (run twice)
    NormalizedPoly np{
        2,
        64,
        { { make_exp({ 1, 0 }), 3 }, { make_exp({ 0, 1 }), 5 }, { make_exp({ 1, 1 }), 7 } }
    };
    auto r1 = BuildPolyExpr(np);
    auto r2 = BuildPolyExpr(np);
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    auto &e1 = r1.value();
    auto &e2 = r2.value();

    for (uint64_t x = 0; x < 4; ++x) {
        for (uint64_t y = 0; y < 4; ++y) {
            std::vector< uint64_t > v = { x, y };
            EXPECT_EQ(EvalExpr(*e1, v, 64), EvalExpr(*e2, v, 64));
        }
    }
}

TEST(PolyExprBuilderTest, CubicTerm) {
    // x_(3) with coeff 1. x_(3) = x(x-1)(x-2) = x^3 - 3x^2 + 2x
    NormalizedPoly np{ 1, 64, { { make_exp({ 3 }), 1 } } };
    auto r = BuildPolyExpr(np);
    ASSERT_TRUE(r.has_value());
    auto &expr = r.value();

    // x_(3) at x=4 -> 4*3*2 = 24
    std::vector< uint64_t > v4 = { 4 };
    EXPECT_EQ(EvalExpr(*expr, v4, 64), 24u);
    // x_(3) at x=2 -> 2*1*0 = 0
    std::vector< uint64_t > v2 = { 2 };
    EXPECT_EQ(EvalExpr(*expr, v2, 64), 0u);
}

TEST(PolyExprBuilderTest, Degree4Term) {
    // x_(4) with coeff 1. x_(4) = x(x-1)(x-2)(x-3)
    NormalizedPoly np{ 1, 64, { { make_exp({ 4 }), 1 } } };
    auto r = BuildPolyExpr(np);
    ASSERT_TRUE(r.has_value());
    auto &expr = r.value();

    // x_(4) at x=5 -> 5*4*3*2 = 120
    std::vector< uint64_t > v5 = { 5 };
    EXPECT_EQ(EvalExpr(*expr, v5, 64), 120u);
    // x_(4) at x=3 -> 3*2*1*0 = 0
    std::vector< uint64_t > v3 = { 3 };
    EXPECT_EQ(EvalExpr(*expr, v3, 64), 0u);
}

TEST(PolyExprBuilderTest, MixedDegreeMultivariate) {
    // x^3 * y = x_(1)*y_(1) + 3*x_(2)*y_(1) + x_(3)*y_(1)
    // Check eval at x=2, y=3 -> 2^3 * 3 = 24
    NormalizedPoly np{
        2,
        64,
        { { make_exp({ 1, 1 }), 1 }, { make_exp({ 2, 1 }), 3 }, { make_exp({ 3, 1 }), 1 } }
    };
    auto r = BuildPolyExpr(np);
    ASSERT_TRUE(r.has_value());

    for (uint64_t x = 0; x < 5; ++x) {
        for (uint64_t y = 0; y < 5; ++y) {
            std::vector< uint64_t > v = { x, y };
            uint64_t expected         = x * x * x * y;
            EXPECT_EQ(EvalExpr(*r.value(), v, 64), expected) << "x=" << x << " y=" << y;
        }
    }
}
