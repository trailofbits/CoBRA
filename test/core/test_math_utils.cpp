#include "cobra/core/MathUtils.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(TwosInFactorialTest, KnownValues) {
    EXPECT_EQ(TwosInFactorial(0), 0u);
    EXPECT_EQ(TwosInFactorial(1), 0u);
    EXPECT_EQ(TwosInFactorial(2), 1u);
    EXPECT_EQ(TwosInFactorial(3), 1u);
    EXPECT_EQ(TwosInFactorial(4), 3u);
    EXPECT_EQ(TwosInFactorial(5), 3u);
    EXPECT_EQ(TwosInFactorial(6), 4u);
    EXPECT_EQ(TwosInFactorial(8), 7u);
    EXPECT_EQ(TwosInFactorial(16), 15u);
    EXPECT_EQ(TwosInFactorial(32), 31u);
    EXPECT_EQ(TwosInFactorial(64), 63u);
}

TEST(TwosInFactorialTest, ClosedFormCrossCheck) {
    for (uint32_t k = 0; k <= 200; ++k) {
        uint32_t expected = k - static_cast< uint32_t >(std::popcount(k));
        EXPECT_EQ(TwosInFactorial(k), expected) << "k=" << k;
    }
}

TEST(DegreeCapTest, KnownValues) {
    EXPECT_EQ(DegreeCap(2), 4u);
    EXPECT_EQ(DegreeCap(8), 10u);
    EXPECT_EQ(DegreeCap(16), 18u);
    EXPECT_EQ(DegreeCap(32), 34u);
    EXPECT_EQ(DegreeCap(64), 66u);
}

TEST(DegreeCapTest, BoundaryInvariant) {
    for (uint32_t w : { 2, 3, 4, 8, 16, 32, 64 }) {
        uint32_t dw = DegreeCap(w);
        EXPECT_LT(TwosInFactorial(dw - 1), w) << "w=" << w;
        EXPECT_GE(TwosInFactorial(dw), w) << "w=" << w;
    }
}

TEST(OddPartFactorialTest, KnownValues) {
    EXPECT_EQ(OddPartFactorial(1, 64), 1u);
    EXPECT_EQ(OddPartFactorial(2, 64), 1u);
    EXPECT_EQ(OddPartFactorial(3, 64), 3u);
    EXPECT_EQ(OddPartFactorial(4, 64), 3u);
    EXPECT_EQ(OddPartFactorial(5, 64), 15u);
    EXPECT_EQ(OddPartFactorial(6, 64), 45u);
}

TEST(OddPartFactorialTest, RoundtripSmallK) {
    uint64_t factorial = 1;
    for (uint32_t k = 1; k <= 20; ++k) {
        factorial    *= k;
        uint64_t odd  = OddPartFactorial(k, 64);
        uint32_t v    = TwosInFactorial(k);
        EXPECT_EQ(odd * (1ULL << v), factorial) << "k=" << k;
    }
}

TEST(OddPartFactorialTest, AlwaysOdd) {
    for (uint32_t k = 1; k <= 100; ++k) {
        uint64_t odd = OddPartFactorial(k, 64);
        EXPECT_TRUE(odd & 1) << "k=" << k;
    }
}

TEST(ModInverseOddTest, InverseOfOne) {
    EXPECT_EQ(ModInverseOdd(1, 64), 1u);
    EXPECT_EQ(ModInverseOdd(1, 8), 1u);
    EXPECT_EQ(ModInverseOdd(1, 1), 1u);
}

TEST(ModInverseOddTest, InverseOfThree) {
    for (uint32_t bits : { 2, 4, 8, 16, 32, 63, 64 }) {
        uint64_t inv  = ModInverseOdd(3, bits);
        uint64_t mask = Bitmask(bits);
        EXPECT_EQ((3 * inv) & mask, 1u) << "bits=" << bits;
    }
}

TEST(ModInverseOddTest, InverseOfOddFactorials) {
    for (uint32_t w : { 8, 16, 32, 64 }) {
        uint32_t dw = DegreeCap(w);
        for (uint32_t k = 1; k < dw; ++k) {
            uint32_t v    = TwosInFactorial(k);
            uint32_t bits = w - v;
            uint64_t odd  = OddPartFactorial(k, bits);
            uint64_t inv  = ModInverseOdd(odd, bits);
            uint64_t mask = Bitmask(bits);
            EXPECT_EQ((odd * inv) & mask, 1u) << "w=" << w << " k=" << k << " bits=" << bits;
        }
    }
}

TEST(ModInverseOddTest, SingleBit) {
    EXPECT_EQ(ModInverseOdd(1, 1), 1u);
    EXPECT_EQ(ModInverseOdd(3, 1), 1u); // 3 & 1 == 1
}
