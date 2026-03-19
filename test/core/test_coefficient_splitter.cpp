#include "cobra/core/CoefficientSplitter.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(ModInverseTest, InverseOfOne) {
    // 1 * 1 = 1 mod 2^63
    EXPECT_EQ(ModInverseOddHalf(1, 64), 1u);
}

TEST(ModInverseTest, InverseOfThree64Bit) {
    uint64_t inv      = ModInverseOddHalf(3, 64);
    // 3 * inv should be 1 mod 2^63
    uint64_t half_mod = (1ULL << 63) - 1;
    EXPECT_EQ((3 * inv) & half_mod, 1u);
}

TEST(ModInverseTest, InverseOfSeven64Bit) {
    uint64_t inv      = ModInverseOddHalf(7, 64);
    uint64_t half_mod = (1ULL << 63) - 1;
    EXPECT_EQ((7 * inv) & half_mod, 1u);
}

TEST(ModInverseTest, InverseOf255_8Bit) {
    // 255 is odd; inverse mod 2^7
    uint64_t inv      = ModInverseOddHalf(255, 8);
    uint64_t half_mod = (1ULL << 7) - 1;
    EXPECT_EQ((255 * inv) & half_mod, 1u);
}

TEST(ModInverseTest, LargeOddNumber) {
    uint64_t x        = 0xDEADBEEFDEADBEEFULL | 1; // force odd
    uint64_t inv      = ModInverseOddHalf(x, 64);
    uint64_t half_mod = (1ULL << 63) - 1;
    EXPECT_EQ((x * inv) & half_mod, 1u);
}

// --- Task 2: two-variable AND/MUL case ---

TEST(SplitCoefficientsTest, PureAndUnchanged) {
    // x & y: CoB coefficients [0, 0, 0, 1]
    // Evaluator IS x & y, so AND interpretation is correct.
    std::vector< uint64_t > cob = { 0, 0, 0, 1 };
    uint32_t n = 2, w = 64;

    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] & v[1]; };

    auto result = SplitCoefficients(cob, eval, n, w);
    EXPECT_EQ(result.and_coeffs[3], 1u);
    EXPECT_EQ(result.mul_coeffs[3], 0u);
}

TEST(SplitCoefficientsTest, PureMulDetected) {
    // x * y: sig on {0,1} is [0, 0, 0, 1], same as x & y.
    // CoB gives [0, 0, 0, 1].
    // Evaluator IS x * y, so AND interpretation is wrong.
    std::vector< uint64_t > cob = { 0, 0, 0, 1 };
    uint32_t n = 2, w = 64;

    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] * v[1]; };

    auto result = SplitCoefficients(cob, eval, n, w);
    // mask 0b11 should be entirely MUL, not AND
    EXPECT_EQ(result.and_coeffs[3], 0u);
    EXPECT_EQ(result.mul_coeffs[3], 1u);
}

TEST(SplitCoefficientsTest, MixedAndMulSplit) {
    // Target: 3*(x&y) + 5*(x*y)
    // On {0,1}: x&y = x*y, so CoB coefficient for mask 0b11 = 8.
    std::vector< uint64_t > cob = { 0, 0, 0, 8 };
    uint32_t n = 2, w = 64;

    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return 3 * (v[0] & v[1]) + 5 * (v[0] * v[1]);
    };

    auto result = SplitCoefficients(cob, eval, n, w);
    // Canonical representative: b_m recovered mod 2^63
    EXPECT_EQ(result.mul_coeffs[3], 5u);
    EXPECT_EQ(result.and_coeffs[3], 3u);
}

// --- Task 3: singleton-square (popcount-1) splitting tests ---

TEST(SplitCoefficientsTest, SingletonSquareDetected) {
    // Target: x^2. On {0,1}: x^2 = x, so CoB = [0, 1].
    std::vector< uint64_t > cob = { 0, 1 };
    uint32_t n = 1, w = 64;

    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] * v[0]; };

    auto result = SplitCoefficients(cob, eval, n, w);
    EXPECT_EQ(result.and_coeffs[1], 0u); // no linear part
    EXPECT_EQ(result.mul_coeffs[1], 1u); // quadratic part
}

TEST(SplitCoefficientsTest, kLinearPlusQuadratic) {
    // Target: 3*x + 5*x^2. On {0,1}: 3x + 5x = 8x.
    // CoB = [0, 8].
    std::vector< uint64_t > cob = { 0, 8 };
    uint32_t n = 1, w = 64;

    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return 3 * v[0] + 5 * v[0] * v[0];
    };

    auto result = SplitCoefficients(cob, eval, n, w);
    EXPECT_EQ(result.and_coeffs[1], 3u); // linear
    EXPECT_EQ(result.mul_coeffs[1], 5u); // quadratic
}

// --- Task 4: edge cases and width variants ---

TEST(SplitCoefficientsTest, AllZeroCoefficients) {
    std::vector< uint64_t > cob = { 0, 0, 0, 0 };
    uint32_t n = 2, w = 64;
    auto eval   = [](const std::vector< uint64_t > &) -> uint64_t { return 0; };
    auto result = SplitCoefficients(cob, eval, n, w);
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(result.and_coeffs[i], 0u);
        EXPECT_EQ(result.mul_coeffs[i], 0u);
    }
}

TEST(SplitCoefficientsTest, ConstantOnly) {
    std::vector< uint64_t > cob = { 42, 0, 0, 0 };
    uint32_t n = 2, w = 64;
    auto eval   = [](const std::vector< uint64_t > &) -> uint64_t { return 42; };
    auto result = SplitCoefficients(cob, eval, n, w);
    EXPECT_EQ(result.and_coeffs[0], 42u);
    EXPECT_EQ(result.mul_coeffs[0], 0u);
}

TEST(SplitCoefficientsTest, Bitwidth8MulDetected) {
    std::vector< uint64_t > cob = { 0, 0, 0, 1 };
    uint32_t n = 2, w = 8;

    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return (v[0] * v[1]) & 0xFF;
    };

    auto result = SplitCoefficients(cob, eval, n, w);
    EXPECT_EQ(result.and_coeffs[3], 0u);
    EXPECT_EQ(result.mul_coeffs[3], 1u);
}

TEST(SplitCoefficientsTest, ThreeVarMulProduct) {
    // x * y * z: CoB on {0,1} = [0,0,0,0,0,0,0,1] (same as x&y&z)
    std::vector< uint64_t > cob = { 0, 0, 0, 0, 0, 0, 0, 1 };
    uint32_t n = 3, w = 64;

    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] * v[1] * v[2]; };

    auto result = SplitCoefficients(cob, eval, n, w);
    EXPECT_EQ(result.and_coeffs[7], 0u);
    EXPECT_EQ(result.mul_coeffs[7], 1u);
}

TEST(SplitCoefficientsTest, Bitwidth2Minimum) {
    std::vector< uint64_t > cob = { 0, 0, 0, 1 };
    uint32_t n = 2, w = 2;

    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return (v[0] * v[1]) & 0x3;
    };

    auto result = SplitCoefficients(cob, eval, n, w);
    EXPECT_EQ(result.mul_coeffs[3], 1u);
    EXPECT_EQ(result.and_coeffs[3], 0u);
}

// --- singleton_at_2 parameter tests ---

TEST(SplitCoefficientsTest, SingletonMasksCrossTermFixed) {
    // f(d,e) = d - d^2 + d*e.
    // On {0,1}: d&e.  CoB = [0, 0, 0, 1].
    // Without singleton_at_2 the diff at (2,2) is 0 and MUL is
    // not detected.  With the recovered S_d(2)=-2, the splitter
    // correctly sees the cross-term.
    std::vector< uint64_t > cob = { 0, 0, 0, 1 };
    uint32_t n = 2, w = 64;

    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] - v[0] * v[0] + v[0] * v[1];
    };

    std::vector< uint64_t > s2 = { static_cast< uint64_t >(-2), 0 }; // S_d(2)=-2, S_e(2)=0
    auto result                = SplitCoefficients(cob, eval, n, w, s2);

    EXPECT_EQ(result.mul_coeffs[3], 1u);
    EXPECT_EQ(result.and_coeffs[3], 0u);
    EXPECT_EQ(result.and_coeffs[1], 0u);
    EXPECT_EQ(result.mul_coeffs[1], 0u);
}

TEST(SplitCoefficientsTest, kLinearPlusCrossTermWithSingleton) {
    // f(d,e) = d + d*e.
    // On {0,1}: sig = [0,1,0,2].  CoB = [0, 1, 0, 1].
    // S_d(2) = 2, S_e(2) = 0.
    // The linear singleton is already correctly modeled, so the
    // cross-term d*e must still be detected.
    std::vector< uint64_t > cob = { 0, 1, 0, 1 };
    uint32_t n = 2, w = 64;

    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] + v[0] * v[1]; };

    std::vector< uint64_t > s2 = { 2, 0 };
    auto result                = SplitCoefficients(cob, eval, n, w, s2);

    EXPECT_EQ(result.mul_coeffs[3], 1u);
    EXPECT_EQ(result.and_coeffs[3], 0u);
    EXPECT_EQ(result.and_coeffs[1], 0u);
    EXPECT_EQ(result.mul_coeffs[1], 0u);
}

TEST(SplitCoefficientsTest, PureMulWithSingletonAtZero) {
    // f(d,e) = d*e.  No singleton powers.
    // singleton_at_2 = [0, 0]: model unchanged, MUL still detected.
    std::vector< uint64_t > cob = { 0, 0, 0, 1 };
    uint32_t n = 2, w = 64;

    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] * v[1]; };

    std::vector< uint64_t > s2 = { 0, 0 };
    auto result                = SplitCoefficients(cob, eval, n, w, s2);

    EXPECT_EQ(result.mul_coeffs[3], 1u);
    EXPECT_EQ(result.and_coeffs[3], 0u);
}

TEST(SplitCoefficientsTest, QuadraticOnlyNoSpuriousMul) {
    // f(d) = d - d^2, 1 variable.  On {0,1}: identically 0.
    // CoB = [0, 0].  singleton_at_2 = [-2].
    // No cross-term masks exist, so no MUL should be created.
    std::vector< uint64_t > cob = { 0, 0 };
    uint32_t n = 1, w = 64;

    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] - v[0] * v[0]; };

    std::vector< uint64_t > s2 = { static_cast< uint64_t >(-2) };
    auto result                = SplitCoefficients(cob, eval, n, w, s2);

    EXPECT_EQ(result.and_coeffs[0], 0u);
    EXPECT_EQ(result.and_coeffs[1], 0u);
    EXPECT_EQ(result.mul_coeffs[0], 0u);
    EXPECT_EQ(result.mul_coeffs[1], 0u);
}
