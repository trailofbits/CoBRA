#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include "cobra/core/MathUtils.h"
#include "cobra/core/SignatureChecker.h" // eval_expr
#include "cobra/core/SingletonPowerExprBuilder.h"
#include "cobra/core/SingletonPowerPoly.h"
#include <gtest/gtest.h>
#include <random>

using namespace cobra;

// Helper: evaluate a univariate polynomial in factorial basis.
static uint64_t
eval_factorial_poly(uint64_t x, const std::vector< UnivariateTerm > &terms, uint32_t bitwidth) {
    uint64_t mask   = Bitmask(bitwidth);
    uint64_t result = 0;
    for (const auto &t : terms) {
        uint64_t falling = 1;
        for (uint16_t i = 0; i < t.degree; ++i) {
            falling = (falling * ((x - i) & mask)) & mask;
        }
        result = (result + t.coeff * falling) & mask;
    }
    return result;
}

TEST(FactorialToMonomialTest, X_2_At_W64) {
    // x_(2) = x^2 - x. Monomial: a[1] = -1 mod 2^64, a[2] = 1.
    SingletonPowerResult powers;
    powers.num_vars = 1;
    powers.bitwidth = 64;
    powers.per_var.push_back({ 64, { { 2, 1 } } });

    auto expr = BuildSingletonPowerExpr(powers);
    ASSERT_NE(expr, nullptr);

    // Evaluate at several points and compare to x^2 - x
    uint64_t mask = Bitmask(64);
    for (uint64_t x : { 0ULL, 1ULL, 2ULL, 3ULL, 100ULL, ~0ULL }) {
        uint64_t expected = (x * x - x) & mask;
        uint64_t actual   = EvalExpr(*expr, { x }, 64);
        EXPECT_EQ(actual, expected) << "x=" << x;
    }
}

TEST(FactorialToMonomialTest, X_3_At_W64) {
    // x_(3) = x^3 - 3x^2 + 2x. a[1]=2, a[2]=-3 mod 2^64, a[3]=1.
    SingletonPowerResult powers;
    powers.num_vars = 1;
    powers.bitwidth = 64;
    powers.per_var.push_back({ 64, { { 3, 1 } } });

    auto expr = BuildSingletonPowerExpr(powers);
    ASSERT_NE(expr, nullptr);

    uint64_t mask = Bitmask(64);
    for (uint64_t x : { 0ULL, 1ULL, 2ULL, 3ULL, 10ULL, 255ULL }) {
        uint64_t expected = eval_factorial_poly(
            x,
            {
                { 3, 1 }
        },
            64
        );
        uint64_t actual = EvalExpr(*expr, { x }, 64);
        EXPECT_EQ(actual, expected) << "x=" << x;
    }
}

TEST(SingletonPowerExprBuilderTest, TwoVariableSingleton) {
    // Variable 0: x^2 (h_1=1, h_2=1).
    // Variable 1: 5*y (h_1=5).
    SingletonPowerResult powers;
    powers.num_vars = 2;
    powers.bitwidth = 64;
    powers.per_var.push_back({
        64, { { 1, 1 }, { 2, 1 } }
    });
    powers.per_var.push_back({ 64, { { 1, 5 } } });

    auto expr = BuildSingletonPowerExpr(powers);
    ASSERT_NE(expr, nullptr);

    uint64_t mask = Bitmask(64);
    for (uint64_t x : { 0ULL, 1ULL, 2ULL, 10ULL }) {
        for (uint64_t y : { 0ULL, 1ULL, 3ULL, 7ULL }) {
            uint64_t expected = ((x * x) + 5 * y) & mask;
            uint64_t actual   = EvalExpr(*expr, { x, y }, 64);
            EXPECT_EQ(actual, expected) << "x=" << x << " y=" << y;
        }
    }
}

TEST(SingletonPowerExprBuilderTest, EmptyResult) {
    SingletonPowerResult powers;
    powers.num_vars = 2;
    powers.bitwidth = 64;
    powers.per_var.push_back({ 64, {} });
    powers.per_var.push_back({ 64, {} });

    auto expr = BuildSingletonPowerExpr(powers);
    EXPECT_EQ(expr, nullptr);
}

TEST(SingletonPowerExprBuilderTest, PreservesVariableIndexAboveUint8) {
    constexpr uint32_t num_vars   = 300;
    constexpr uint32_t target_var = 299;

    SingletonPowerResult powers;
    powers.num_vars = num_vars;
    powers.bitwidth = 64;
    powers.per_var.resize(num_vars, { 64, {} });
    powers.per_var[target_var].terms.push_back({ 1, 7 });

    auto expr = BuildSingletonPowerExpr(powers);
    ASSERT_NE(expr, nullptr);

    std::vector< uint64_t > inputs(num_vars, 0);
    inputs[target_var] = 5;
    EXPECT_EQ(EvalExpr(*expr, inputs, 64), 35u);
}

TEST(SingletonPowerExprBuilderTest, MultiBitwidthEvaluation) {
    // x^3 across multiple bitwidths.
    // x^3 = x_(3) + 3*x_(2) + x_(1).
    for (uint32_t w : { 2, 4, 8, 16, 32, 64 }) {
        uint64_t mask                       = Bitmask(w);
        std::vector< UnivariateTerm > terms = {
            { 1,        1 },
            { 2, 3 & mask },
            { 3,        1 }
        };

        SingletonPowerResult powers;
        powers.num_vars = 1;
        powers.bitwidth = w;
        powers.per_var.push_back({ w, terms });

        auto expr = BuildSingletonPowerExpr(powers);
        ASSERT_NE(expr, nullptr) << "w=" << w;

        std::mt19937_64 rng(42 + w);
        for (int i = 0; i < 20; ++i) {
            uint64_t x        = rng() & mask;
            uint64_t expected = eval_factorial_poly(x, terms, w);
            uint64_t actual   = EvalExpr(*expr, { x }, w);
            EXPECT_EQ(actual, expected) << "w=" << w << " x=" << x;
        }
    }
}
