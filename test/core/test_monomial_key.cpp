#include "cobra/core/BitWidth.h"
#include "cobra/core/MathUtils.h"
#include "cobra/core/MonomialKey.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(MonomialKeyTest, RoundtripSingleVar) {
    uint8_t exps[] = { 2 };
    auto k         = MonomialKey::FromExponents(exps, 1);
    uint8_t out[1];
    k.ToExponents(out, 1);
    EXPECT_EQ(out[0], 2);
}

TEST(MonomialKeyTest, RoundtripTwoVars) {
    uint8_t exps[] = { 1, 2 };
    auto k         = MonomialKey::FromExponents(exps, 2);
    uint8_t out[2];
    k.ToExponents(out, 2);
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[1], 2);
}

TEST(MonomialKeyTest, RoundtripAllZeros) {
    uint8_t exps[] = { 0, 0, 0 };
    auto k         = MonomialKey::FromExponents(exps, 3);
    uint8_t out[3];
    k.ToExponents(out, 3);
    for (int i = 0; i < 3; ++i) { EXPECT_EQ(out[i], 0); }
}

TEST(MonomialKeyTest, ExponentAt) {
    uint8_t exps[] = { 1, 0, 2 };
    auto k         = MonomialKey::FromExponents(exps, 3);
    EXPECT_EQ(k.ExponentAt(0), 1);
    EXPECT_EQ(k.ExponentAt(1), 0);
    EXPECT_EQ(k.ExponentAt(2), 2);
}

TEST(MonomialKeyTest, WithExponent) {
    uint8_t exps[] = { 1, 0, 2 };
    auto k         = MonomialKey::FromExponents(exps, 3);
    auto k2        = k.WithExponent(2, 1);
    EXPECT_EQ(k2.ExponentAt(0), 1);
    EXPECT_EQ(k2.ExponentAt(1), 0);
    EXPECT_EQ(k2.ExponentAt(2), 1);
}

TEST(MonomialKeyTest, TotalDegreeReturnsUint32) {
    uint8_t exps[] = { 1, 2, 0 };
    auto k         = MonomialKey::FromExponents(exps, 3);
    uint32_t td    = k.TotalDegree(3);
    EXPECT_EQ(td, 3u);
}

TEST(MonomialKeyTest, MaxDegree) {
    uint8_t exps[] = { 1, 3, 0, 2 };
    auto k         = MonomialKey::FromExponents(exps, 4);
    EXPECT_EQ(k.MaxDegree(4), 3);
}

TEST(MonomialKeyTest, HighDegreeExponents) {
    // Degree 4 — beyond old base-3 limit
    uint8_t exps[] = { 4, 3 };
    auto k         = MonomialKey::FromExponents(exps, 2);
    EXPECT_EQ(k.ExponentAt(0), 4);
    EXPECT_EQ(k.ExponentAt(1), 3);
    EXPECT_EQ(k.TotalDegree(2), 7u);
}

TEST(MonomialKeyTest, HashConsistency) {
    uint8_t exps[] = { 1, 2 };
    auto k1        = MonomialKey::FromExponents(exps, 2);
    auto k2        = MonomialKey::FromExponents(exps, 2);
    MonomialKeyHash h;
    EXPECT_EQ(h(k1), h(k2));
}

TEST(MonomialKeyTest, HashDiffers) {
    uint8_t e1[] = { 1, 2 };
    uint8_t e2[] = { 2, 1 };
    auto k1      = MonomialKey::FromExponents(e1, 2);
    auto k2      = MonomialKey::FromExponents(e2, 2);
    MonomialKeyHash h;
    EXPECT_NE(h(k1), h(k2));
}

TEST(MonomialKeyTest, LexicographicOrder) {
    uint8_t e01[] = { 0, 1 };
    uint8_t e10[] = { 1, 0 };
    uint8_t e12[] = { 1, 2 };
    uint8_t e20[] = { 2, 0 };
    auto k01      = MonomialKey::FromExponents(e01, 2);
    auto k10      = MonomialKey::FromExponents(e10, 2);
    auto k12      = MonomialKey::FromExponents(e12, 2);
    auto k20      = MonomialKey::FromExponents(e20, 2);
    EXPECT_LT(k01, k10);
    EXPECT_LT(k10, k12);
    EXPECT_LT(k12, k20);
}

TEST(MonomialKeyTest, MaxVars16) {
    uint8_t exps[16];
    for (int i = 0; i < 16; ++i) { exps[i] = static_cast< uint8_t >(i % 5); }
    auto k = MonomialKey::FromExponents(exps, 16);
    uint8_t out[16];
    k.ToExponents(out, 16);
    for (int i = 0; i < 16; ++i) { EXPECT_EQ(out[i], i % 5); }
}

TEST(MonomialKeyTest, UnusedSuffixIsZero) {
    uint8_t exps[] = { 3, 4 };
    auto k         = MonomialKey::FromExponents(exps, 2);
    // Indices 2..19 must all be zero
    for (uint8_t i = 2; i < kMaxPolyVars; ++i) {
        EXPECT_EQ(k.exponents[i], 0) << "nonzero at index " << (int) i;
    }
}

TEST(MonomialKeyTest, EqualityRequiresFullArray) {
    // Two keys with same first 2 exponents but different num_vars
    // used during construction should still be equal if the array matches.
    uint8_t e2[] = { 1, 2 };
    uint8_t e3[] = { 1, 2, 0 };
    auto k2      = MonomialKey::FromExponents(e2, 2);
    auto k3      = MonomialKey::FromExponents(e3, 3);
    EXPECT_EQ(k2, k3);
}

// --- V2FactorialWeight tests ---

TEST(MonomialKeyTest, V2FactorialWeight_AllZeros) {
    uint8_t exps[] = { 0, 0 };
    auto k         = MonomialKey::FromExponents(exps, 2);
    EXPECT_EQ(k.V2FactorialWeight(2), 0u);
}

TEST(MonomialKeyTest, V2FactorialWeight_AllOnes) {
    uint8_t exps[] = { 1, 1, 1 };
    auto k         = MonomialKey::FromExponents(exps, 3);
    EXPECT_EQ(k.V2FactorialWeight(3), 0u); // v_2(1!) = 0
}

TEST(MonomialKeyTest, V2FactorialWeight_Degree2) {
    // v_2(2!) = 1 per variable with exp=2
    uint8_t exps[] = { 2, 1 };
    auto k         = MonomialKey::FromExponents(exps, 2);
    EXPECT_EQ(k.V2FactorialWeight(2), 1u);
}

TEST(MonomialKeyTest, V2FactorialWeight_TwoSquares) {
    uint8_t exps[] = { 2, 2 };
    auto k         = MonomialKey::FromExponents(exps, 2);
    EXPECT_EQ(k.V2FactorialWeight(2), 2u);
}

TEST(MonomialKeyTest, V2FactorialWeight_Degree3) {
    // v_2(3!) = v_2(6) = 1
    uint8_t exps[] = { 3, 0 };
    auto k         = MonomialKey::FromExponents(exps, 2);
    EXPECT_EQ(k.V2FactorialWeight(2), 1u);
}

TEST(MonomialKeyTest, V2FactorialWeight_Degree4) {
    // v_2(4!) = v_2(24) = 3
    uint8_t exps[] = { 4, 0 };
    auto k         = MonomialKey::FromExponents(exps, 2);
    EXPECT_EQ(k.V2FactorialWeight(2), 3u);
}

TEST(MonomialKeyTest, V2FactorialWeight_Mixed) {
    // (4, 3, 2): v_2(4!) + v_2(3!) + v_2(2!) = 3 + 1 + 1 = 5
    uint8_t exps[] = { 4, 3, 2 };
    auto k         = MonomialKey::FromExponents(exps, 3);
    EXPECT_EQ(k.V2FactorialWeight(3), 5u);
}

TEST(MonomialKeyTest, V2FactorialWeight_MatchesTwosInFactorial) {
    // Cross-check against TwosInFactorial for each exponent
    for (uint32_t d = 0; d <= 10; ++d) {
        uint8_t exps[] = { static_cast< uint8_t >(d) };
        auto k         = MonomialKey::FromExponents(exps, 1);
        EXPECT_EQ(k.V2FactorialWeight(1), TwosInFactorial(d)) << "Mismatch at degree " << d;
    }
}

// --- Stirling table tests ---

TEST(StirlingTableTest, SecondKind_KnownValues) {
    // S(0,0)=1, S(1,0)=0, S(1,1)=1, S(2,0)=0, S(2,1)=1, S(2,2)=1
    // S(3,0)=0, S(3,1)=1, S(3,2)=3, S(3,3)=1
    auto table = cobra::BuildStirlingSecondKind(3, 64);
    EXPECT_EQ(table[0][0], 1u);
    EXPECT_EQ(table[1][0], 0u);
    EXPECT_EQ(table[1][1], 1u);
    EXPECT_EQ(table[2][0], 0u);
    EXPECT_EQ(table[2][1], 1u);
    EXPECT_EQ(table[2][2], 1u);
    EXPECT_EQ(table[3][0], 0u);
    EXPECT_EQ(table[3][1], 1u);
    EXPECT_EQ(table[3][2], 3u);
    EXPECT_EQ(table[3][3], 1u);
}

TEST(StirlingTableTest, FirstKind_KnownValues) {
    // s(0,0)=1, s(1,0)=0, s(1,1)=1
    // s(2,0)=0, s(2,1)=-1 mod 2^64, s(2,2)=1
    // s(3,0)=0, s(3,1)=2, s(3,2)=-3 mod 2^64, s(3,3)=1
    auto table = cobra::BuildStirlingFirstKind(3, 64);
    EXPECT_EQ(table[0][0], 1u);
    EXPECT_EQ(table[1][1], 1u);
    EXPECT_EQ(table[2][1], UINT64_MAX); // -1 mod 2^64
    EXPECT_EQ(table[2][2], 1u);
    EXPECT_EQ(table[3][1], 2u);
    EXPECT_EQ(table[3][2], UINT64_MAX - 2); // -3 mod 2^64
    EXPECT_EQ(table[3][3], 1u);
}

TEST(StirlingTableTest, Degree4_SecondKind) {
    // S(4,1)=1, S(4,2)=7, S(4,3)=6, S(4,4)=1
    auto table = cobra::BuildStirlingSecondKind(4, 64);
    EXPECT_EQ(table[4][1], 1u);
    EXPECT_EQ(table[4][2], 7u);
    EXPECT_EQ(table[4][3], 6u);
    EXPECT_EQ(table[4][4], 1u);
}

TEST(StirlingTableTest, ModularArithmetic_W8) {
    // At w=8, -1 mod 256 = 255, -3 mod 256 = 253
    auto table = cobra::BuildStirlingFirstKind(3, 8);
    EXPECT_EQ(table[2][1], 255u);
    EXPECT_EQ(table[3][2], 253u);
}
