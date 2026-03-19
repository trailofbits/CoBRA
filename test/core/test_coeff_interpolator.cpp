#include "cobra/core/BitWidth.h"
#include "cobra/core/CoeffInterpolator.h"
#include <bit>
#include <gtest/gtest.h>

using namespace cobra;

TEST(CoeffInterpolatorTest, SingleVar) {
    std::vector< uint64_t > sig = { 0, 1 };
    auto result                 = InterpolateCoefficients(sig, 1, 64);
    EXPECT_EQ(result[0], 0u);
    EXPECT_EQ(result[1], 1u);
}

TEST(CoeffInterpolatorTest, TwoVarAddFromSlides) {
    // From spec: G * [0, 1, 1, 2] = [0, 1, 1, 0]
    std::vector< uint64_t > sig = { 0, 1, 1, 2 };
    auto result                 = InterpolateCoefficients(sig, 2, 64);
    EXPECT_EQ(result[0], 0u);
    EXPECT_EQ(result[1], 1u);
    EXPECT_EQ(result[2], 1u);
    EXPECT_EQ(result[3], 0u);
}

TEST(CoeffInterpolatorTest, TwoVarXor) {
    std::vector< uint64_t > sig = { 0, 1, 1, 0 };
    auto result                 = InterpolateCoefficients(sig, 2, 64);
    EXPECT_EQ(result[0], 0u);
    EXPECT_EQ(result[1], 1u);
    EXPECT_EQ(result[2], 1u);
    EXPECT_EQ(result[3], ModNeg(2, 64));
}

TEST(CoeffInterpolatorTest, TwoVarAnd) {
    std::vector< uint64_t > sig = { 0, 0, 0, 1 };
    auto result                 = InterpolateCoefficients(sig, 2, 64);
    EXPECT_EQ(result[0], 0u);
    EXPECT_EQ(result[1], 0u);
    EXPECT_EQ(result[2], 0u);
    EXPECT_EQ(result[3], 1u);
}

TEST(CoeffInterpolatorTest, TwoVarOr) {
    std::vector< uint64_t > sig = { 0, 1, 1, 1 };
    auto result                 = InterpolateCoefficients(sig, 2, 64);
    EXPECT_EQ(result[0], 0u);
    EXPECT_EQ(result[1], 1u);
    EXPECT_EQ(result[2], 1u);
    EXPECT_EQ(result[3], ModNeg(1, 64));
}

TEST(CoeffInterpolatorTest, Constant) {
    std::vector< uint64_t > sig = { 42, 42, 42, 42 };
    auto result                 = InterpolateCoefficients(sig, 2, 64);
    EXPECT_EQ(result[0], 42u);
    EXPECT_EQ(result[1], 0u);
    EXPECT_EQ(result[2], 0u);
    EXPECT_EQ(result[3], 0u);
}

TEST(CoeffInterpolatorTest, ThreeVars) {
    std::vector< uint64_t > sig(8);
    for (uint32_t i = 0; i < 8; ++i) {
        uint64_t x = (i >> 0) & 1;
        uint64_t y = (i >> 1) & 1;
        uint64_t z = (i >> 2) & 1;
        sig[i]     = 3 * x + 5 * y + 7 * z;
    }
    auto result = InterpolateCoefficients(sig, 3, 64);
    EXPECT_EQ(result[0], 0u);
    EXPECT_EQ(result[1], 3u);
    EXPECT_EQ(result[2], 5u);
    EXPECT_EQ(result[3], 0u);
    EXPECT_EQ(result[4], 7u);
    EXPECT_EQ(result[5], 0u);
    EXPECT_EQ(result[6], 0u);
    EXPECT_EQ(result[7], 0u);
}

TEST(CoeffInterpolatorTest, Bitwidth8) {
    std::vector< uint64_t > sig = { 0, 1, 1, 2 };
    auto result                 = InterpolateCoefficients(sig, 2, 8);
    EXPECT_EQ(result[0], 0u);
    EXPECT_EQ(result[1], 1u);
    EXPECT_EQ(result[2], 1u);
    EXPECT_EQ(result[3], 0u);
}

// Gap #1: 1-bit bitwidth — all operations are mod 2
TEST(CoeffInterpolatorTest, Bitwidth1SingleVar) {
    // f(x) = x at 1-bit: sig = [0, 1]
    std::vector< uint64_t > sig = { 0, 1 };
    auto result                 = InterpolateCoefficients(sig, 1, 1);
    EXPECT_EQ(result[0], 0u);
    EXPECT_EQ(result[1], 1u);
}

TEST(CoeffInterpolatorTest, Bitwidth1TwoVarXor) {
    // XOR at 1-bit: sig = [0, 1, 1, 0]
    // CoB: [0, 1, 1, -2 mod 2 = 0]
    std::vector< uint64_t > sig = { 0, 1, 1, 0 };
    auto result                 = InterpolateCoefficients(sig, 2, 1);
    EXPECT_EQ(result[0], 0u);
    EXPECT_EQ(result[1], 1u);
    EXPECT_EQ(result[2], 1u);
    EXPECT_EQ(result[3], 0u); // -2 mod 2 = 0
}

TEST(CoeffInterpolatorTest, Bitwidth1TwoVarAnd) {
    // AND at 1-bit: sig = [0, 0, 0, 1]
    std::vector< uint64_t > sig = { 0, 0, 0, 1 };
    auto result                 = InterpolateCoefficients(sig, 2, 1);
    EXPECT_EQ(result[0], 0u);
    EXPECT_EQ(result[1], 0u);
    EXPECT_EQ(result[2], 0u);
    EXPECT_EQ(result[3], 1u);
}

TEST(CoeffInterpolatorTest, Bitwidth1Add) {
    // At 1-bit: x + y mod 2 = XOR, so sig = [0, 1, 1, 0]
    std::vector< uint64_t > sig = { 0, 1, 1, 0 };
    auto result                 = InterpolateCoefficients(sig, 2, 1);
    EXPECT_EQ(result[0], 0u);
    EXPECT_EQ(result[1], 1u);
    EXPECT_EQ(result[2], 1u);
    // At 1-bit, XOR and addition are the same, so the x&y coefficient
    // is -2 mod 2 = 0, making it affine.
    EXPECT_EQ(result[3], 0u);
}

// Gap #2: 4+ variables
TEST(CoeffInterpolatorTest, FourVars) {
    // f(x,y,z,w) = x + 2*y + 3*z + 4*w
    std::vector< uint64_t > sig(16);
    for (uint32_t i = 0; i < 16; ++i) {
        uint64_t x = (i >> 0) & 1;
        uint64_t y = (i >> 1) & 1;
        uint64_t z = (i >> 2) & 1;
        uint64_t w = (i >> 3) & 1;
        sig[i]     = x + 2 * y + 3 * z + 4 * w;
    }
    auto result = InterpolateCoefficients(sig, 4, 64);
    EXPECT_EQ(result[0], 0u); // constant
    EXPECT_EQ(result[1], 1u); // x
    EXPECT_EQ(result[2], 2u); // y
    EXPECT_EQ(result[4], 3u); // z
    EXPECT_EQ(result[8], 4u); // w
    // All interaction terms should be zero
    for (size_t i = 0; i < 16; ++i) {
        if (std::popcount(i) > 1) {
            EXPECT_EQ(result[i], 0u) << "non-zero interaction at index " << i;
        }
    }
    // All interaction terms verified zero above
}

TEST(CoeffInterpolatorTest, FiveVarsNonAffine) {
    // f = x0 & x1 & x2 & x3 & x4 (5-way AND)
    std::vector< uint64_t > sig(32);
    for (uint32_t i = 0; i < 32; ++i) {
        sig[i] = (i == 31) ? 1 : 0; // only 1 when all bits set
    }
    auto result = InterpolateCoefficients(sig, 5, 64);
    EXPECT_EQ(result[31], 1u); // x0&x1&x2&x3&x4 coefficient
    // Non-affine: highest-order term has non-zero coefficient
}

// Gap #10: Zero-variable cob_transform (constant function)
TEST(CoeffInterpolatorTest, ZeroVars) {
    std::vector< uint64_t > sig = { 42 };
    auto result                 = InterpolateCoefficients(sig, 0, 64);
    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], 42u);
}

TEST(CoeffInterpolatorTest, AllZeroCoeffs) {
    std::vector< uint64_t > coeffs = { 0, 0, 0, 0 };
    for (auto c : coeffs) { EXPECT_EQ(c, 0u); }
}

// Bitwidth=1 with 3 vars
TEST(CoeffInterpolatorTest, Bitwidth1ThreeVars) {
    // x + y + z at 1-bit = x XOR y XOR z
    // sig = [0, 1, 1, 0, 1, 0, 0, 1]
    std::vector< uint64_t > sig(8);
    for (uint32_t i = 0; i < 8; ++i) {
        uint64_t x = (i >> 0) & 1;
        uint64_t y = (i >> 1) & 1;
        uint64_t z = (i >> 2) & 1;
        sig[i]     = (x + y + z) & 1;
    }
    auto result = InterpolateCoefficients(sig, 3, 1);
    // At 1-bit, -2 mod 2 = 0, so all interaction terms vanish
    EXPECT_EQ(result[0], 0u); // constant
    EXPECT_EQ(result[1], 1u); // x
    EXPECT_EQ(result[2], 1u); // y
    EXPECT_EQ(result[4], 1u); // z
    // All interaction terms (popcount>1) should be 0 at 1-bit
    EXPECT_EQ(result[3], 0u); // x&y: -2 mod 2 = 0
    EXPECT_EQ(result[5], 0u); // x&z
    EXPECT_EQ(result[6], 0u); // y&z
    EXPECT_EQ(result[7], 0u); // x&y&z
    // All interaction terms verified zero above
}

// Large coefficients near UINT64_MAX
TEST(CoeffInterpolatorTest, LargeCoefficients64) {
    // f(x) = (UINT64_MAX - 1) * x: sig = [0, UINT64_MAX - 1]
    uint64_t big                = UINT64_MAX - 1;
    std::vector< uint64_t > sig = { 0, big };
    auto result                 = InterpolateCoefficients(sig, 1, 64);
    EXPECT_EQ(result[0], 0u);
    EXPECT_EQ(result[1], big);
}

// All-zero signature
TEST(CoeffInterpolatorTest, AllZeroSig) {
    std::vector< uint64_t > sig = { 0, 0, 0, 0 };
    auto result                 = InterpolateCoefficients(sig, 2, 64);
    for (auto c : result) { EXPECT_EQ(c, 0u); }
    // Interaction terms are zero — affine result
}

// Large constant with wrapping at 8-bit
TEST(CoeffInterpolatorTest, Bitwidth8LargeCoeffs) {
    // 200*x + 100*y at 8-bit: sig = [0, 200, 100, 44]
    // 200+100 = 300 mod 256 = 44
    std::vector< uint64_t > sig = { 0, 200, 100, 44 };
    auto result                 = InterpolateCoefficients(sig, 2, 8);
    EXPECT_EQ(result[0], 0u);
    EXPECT_EQ(result[1], 200u);
    EXPECT_EQ(result[2], 100u);
    EXPECT_EQ(result[3], 0u); // affine: interaction = 0
    // Interaction terms are zero — affine result
}
