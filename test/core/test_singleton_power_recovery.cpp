#include "cobra/core/BitWidth.h"
#include "cobra/core/CoefficientSplitter.h"
#include "cobra/core/MathUtils.h"
#include "cobra/core/SingletonPowerRecovery.h"
#include <cassert>
#include <gtest/gtest.h>

using namespace cobra;

// Helper: evaluate a univariate polynomial in factorial basis.
// H(x) = sum_k h_k * x_(k)  where x_(k) = x(x-1)...(x-k+1)
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

// Helper: build an evaluator from a univariate factorial poly
// for variable var_index in a num_vars-variable expression.
static Evaluator make_univariate_eval(
    const std::vector< UnivariateTerm > &terms, uint32_t var_index, uint32_t /*num_vars*/,
    uint32_t bitwidth, uint64_t constant = 0
) {
    return [=](const std::vector< uint64_t > &v) -> uint64_t {
        uint64_t mask = Bitmask(bitwidth);
        return (constant + eval_factorial_poly(v[var_index], terms, bitwidth)) & mask;
    };
}

TEST(SingletonPowerRecoveryTest, LinearOnly) {
    // H(x) = 3x. Factorial basis: h_1 = 3.
    // 1 variable, w=64.
    std::vector< UnivariateTerm > poly = {
        { 1, 3 }
    };
    auto eval = make_univariate_eval(poly, 0, 1, 64);

    auto result = RecoverSingletonPowers(eval, 1, 64);
    ASSERT_TRUE(result.has_value());

    const auto &per_var = result.value().per_var;
    ASSERT_EQ(per_var.size(), 1u);
    ASSERT_EQ(per_var[0].terms.size(), 1u);
    EXPECT_EQ(per_var[0].terms[0].degree, 1);
    EXPECT_EQ(per_var[0].terms[0].coeff, 3u);
}

// --- Task 6: Known-polynomial recovery ---

TEST(SingletonPowerRecoveryTest, Quadratic) {
    // x^2 = x_(2) + x_(1). h_1 = 1, h_2 = 1.
    std::vector< UnivariateTerm > poly = {
        { 1, 1 },
        { 2, 1 }
    };
    auto eval = make_univariate_eval(poly, 0, 1, 64);

    auto result = RecoverSingletonPowers(eval, 1, 64);
    ASSERT_TRUE(result.has_value());
    const auto &terms = result.value().per_var[0].terms;
    ASSERT_EQ(terms.size(), 2u);
    EXPECT_EQ(terms[0].degree, 1);
    EXPECT_EQ(terms[0].coeff, 1u);
    EXPECT_EQ(terms[1].degree, 2);
    EXPECT_EQ(terms[1].coeff, 1u);
}

TEST(SingletonPowerRecoveryTest, Cubic) {
    // x^3 = x_(3) + 3*x_(2) + x_(1). h_1=1, h_2=3, h_3=1.
    std::vector< UnivariateTerm > poly = {
        { 1, 1 },
        { 2, 3 },
        { 3, 1 }
    };
    auto eval = make_univariate_eval(poly, 0, 1, 64);

    auto result = RecoverSingletonPowers(eval, 1, 64);
    ASSERT_TRUE(result.has_value());
    const auto &terms = result.value().per_var[0].terms;
    ASSERT_EQ(terms.size(), 3u);
    EXPECT_EQ(terms[0].degree, 1);
    EXPECT_EQ(terms[0].coeff, 1u);
    EXPECT_EQ(terms[1].degree, 2);
    EXPECT_EQ(terms[1].coeff, 3u);
    EXPECT_EQ(terms[2].degree, 3);
    EXPECT_EQ(terms[2].coeff, 1u);
}

TEST(SingletonPowerRecoveryTest, PureFactorialDegree5) {
    // x_(5) only. h_5 = 1, all others zero.
    std::vector< UnivariateTerm > poly = {
        { 5, 1 }
    };
    auto eval = make_univariate_eval(poly, 0, 1, 64);

    auto result = RecoverSingletonPowers(eval, 1, 64);
    ASSERT_TRUE(result.has_value());
    const auto &terms = result.value().per_var[0].terms;
    ASSERT_EQ(terms.size(), 1u);
    EXPECT_EQ(terms[0].degree, 5);
    EXPECT_EQ(terms[0].coeff, 1u);
}

TEST(SingletonPowerRecoveryTest, ZeroPolynomial) {
    auto eval   = [](const std::vector< uint64_t > &) -> uint64_t { return 0; };
    auto result = RecoverSingletonPowers(eval, 1, 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value().per_var[0].terms.empty());
}

TEST(SingletonPowerRecoveryTest, NullPolynomialReducesToZero) {
    // 2^{w-1} * x_(2) is a null polynomial (coeff equals modulus).
    // Recovery should produce empty terms after reduction.
    uint32_t w                         = 8;
    uint64_t null_coeff                = 1ULL << (w - 1); // 128
    std::vector< UnivariateTerm > poly = {
        { 2, null_coeff }
    };
    auto eval = make_univariate_eval(poly, 0, 1, w);

    auto result = RecoverSingletonPowers(eval, 1, w);
    ASSERT_TRUE(result.has_value());
    // h_2 mod 2^{w-1} = 128 mod 128 = 0
    EXPECT_TRUE(result.value().per_var[0].terms.empty());
}

TEST(SingletonPowerRecoveryTest, ReducedPrecisionHighDegree) {
    // At w=8, d_w=10. Degree 4 has v_2(4!)=3, so precision = 5 bits.
    // Set h_4 = 7 (fits in 5 bits). Verify recovery matches mod 2^5.
    uint32_t w                         = 8;
    std::vector< UnivariateTerm > poly = {
        { 4, 7 }
    };
    auto eval = make_univariate_eval(poly, 0, 1, w);

    auto result = RecoverSingletonPowers(eval, 1, w);
    ASSERT_TRUE(result.has_value());
    const auto &terms = result.value().per_var[0].terms;
    ASSERT_EQ(terms.size(), 1u);
    EXPECT_EQ(terms[0].degree, 4);
    uint64_t prec_mask = Bitmask(w - TwosInFactorial(4));
    EXPECT_EQ(terms[0].coeff & prec_mask, 7u & prec_mask);
}

TEST(SingletonPowerRecoveryTest, MinimalWidthW2) {
    // w=2, d_w=4. Degrees 1-3. Precision: deg1=2bits, deg2=1bit, deg3=1bit.
    uint32_t w                         = 2;
    // H(x) = 1*x_(1) + 1*x_(2) + 1*x_(3) — all coefficients 1.
    std::vector< UnivariateTerm > poly = {
        { 1, 1 },
        { 2, 1 },
        { 3, 1 }
    };
    auto eval = make_univariate_eval(poly, 0, 1, w);

    auto result = RecoverSingletonPowers(eval, 1, w);
    ASSERT_TRUE(result.has_value());
    const auto &terms = result.value().per_var[0].terms;
    // h_1 = 1 mod 4 = 1. h_2 = 1 mod 2 = 1. h_3 = 1 mod 2 = 1.
    EXPECT_GE(terms.size(), 1u);
    // At minimum, h_1 should be recovered.
    EXPECT_EQ(terms[0].degree, 1);
    EXPECT_EQ(terms[0].coeff, 1u);
}

// --- Task 7: Multivariate isolation and divisibility failure ---

TEST(SingletonPowerRecoveryTest, MultivariateIsolation_XSquaredPlusXY) {
    // f(x,y) = x^2 + x*y. Slice at x (y=0): g_0(t) = t^2.
    // x^2 = x_(2) + x_(1): h_1=1, h_2=1.
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] * v[0] + v[0] * v[1];
    };

    auto result = RecoverSingletonPowers(eval, 2, 64);
    ASSERT_TRUE(result.has_value());

    // Variable 0: recovers x^2 terms
    const auto &t0 = result.value().per_var[0].terms;
    ASSERT_EQ(t0.size(), 2u);
    EXPECT_EQ(t0[0].degree, 1);
    EXPECT_EQ(t0[0].coeff, 1u);
    EXPECT_EQ(t0[1].degree, 2);
    EXPECT_EQ(t0[1].coeff, 1u);

    // Variable 1: slice g_1(t) = 0 (x=0 kills both terms)
    EXPECT_TRUE(result.value().per_var[1].terms.empty());
}

TEST(SingletonPowerRecoveryTest, MultivariateIsolation_XCubedPlusXYPlusY) {
    // f(x,y) = x^3 + x*y + y. Slice at x (y=0): g_0(t) = t^3.
    // x^3 = x_(3) + 3*x_(2) + x_(1).
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] * v[0] * v[0] + v[0] * v[1] + v[1];
    };

    auto result = RecoverSingletonPowers(eval, 2, 64);
    ASSERT_TRUE(result.has_value());

    const auto &t0 = result.value().per_var[0].terms;
    ASSERT_EQ(t0.size(), 3u);
    EXPECT_EQ(t0[0].degree, 1);
    EXPECT_EQ(t0[0].coeff, 1u);
    EXPECT_EQ(t0[1].degree, 2);
    EXPECT_EQ(t0[1].coeff, 3u);
    EXPECT_EQ(t0[2].degree, 3);
    EXPECT_EQ(t0[2].coeff, 1u);

    // Variable 1: slice g_1(t) = t. h_1 = 1.
    const auto &t1 = result.value().per_var[1].terms;
    ASSERT_EQ(t1.size(), 1u);
    EXPECT_EQ(t1[0].degree, 1);
    EXPECT_EQ(t1[0].coeff, 1u);
}

TEST(SingletonPowerRecoveryTest, DivisibilityFailureReturnsError) {
    // Non-polynomial function: f(x) = popcount(x) & 0xFF (bitwise, not polynomial).
    // Forward differences will not be divisible by k!.
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return static_cast< uint64_t >(std::popcount(v[0]));
    };

    auto result = RecoverSingletonPowers(eval, 1, 8);
    // Should return error, not crash.
    EXPECT_FALSE(result.has_value());
}

TEST(SingletonPowerRecoveryTest, PreservesVariableIndexAboveUint8) {
    constexpr uint32_t num_vars   = 300;
    constexpr uint32_t target_var = 299;

    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return 7 * v[target_var]; };

    auto result = RecoverSingletonPowers(eval, num_vars, 64);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result.value().num_vars, num_vars);
    ASSERT_EQ(result.value().per_var.size(), num_vars);

    for (uint32_t i = 0; i < num_vars; ++i) {
        const auto &terms = result.value().per_var[i].terms;
        if (i == target_var) {
            ASSERT_EQ(terms.size(), 1u);
            EXPECT_EQ(terms[0].degree, 1);
            EXPECT_EQ(terms[0].coeff, 7u);
        } else {
            EXPECT_TRUE(terms.empty()) << "i=" << i;
        }
    }
}

// Coefficient splitting and singleton-power recovery must agree on x^2.

TEST(SingletonPowerRecoveryTest, SplittingVsRecoveryOracle) {
    // For x^2, verify that singleton-power recovery's h_2 mod 2^{w-1}
    // equals coefficient splitting's mul_c[1].
    for (uint32_t w : { 8u, 16u, 32u, 64u }) {
        uint64_t mask = Bitmask(w);
        uint64_t half = Bitmask(w - 1);

        auto eval = [mask](const std::vector< uint64_t > &v) -> uint64_t {
            return (v[0] * v[0]) & mask;
        };

        // Coefficient splitting
        std::vector< uint64_t > cob = { 0, 1 };
        auto split                  = SplitCoefficients(cob, eval, 1, w);
        uint64_t split_mul          = split.mul_coeffs[1];

        // Singleton-power recovery
        auto result = RecoverSingletonPowers(eval, 1, w);
        ASSERT_TRUE(result.has_value()) << "w=" << w;

        uint64_t recovered_h2 = 0;
        for (const auto &t : result.value().per_var[0].terms) {
            if (t.degree == 2) {
                recovered_h2 = t.coeff;
                break;
            }
        }

        EXPECT_EQ(recovered_h2 & half, split_mul & half)
            << "w=" << w << " split_mul=" << split_mul << " recovered_h2=" << recovered_h2;
    }
}
