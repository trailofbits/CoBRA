#include "cobra/core/ExponentTuple.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(ExponentTupleTest, RoundtripSingleVar) {
    uint8_t exps[] = { 2 };
    auto t         = ExponentTuple::FromExponents(exps, 1);
    uint8_t out[1];
    t.ToExponents(out, 1);
    EXPECT_EQ(out[0], 2);
}

TEST(ExponentTupleTest, RoundtripTwoVars) {
    uint8_t exps[] = { 1, 2 };
    auto t         = ExponentTuple::FromExponents(exps, 2);
    uint8_t out[2];
    t.ToExponents(out, 2);
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[1], 2);
}

TEST(ExponentTupleTest, RoundtripAllZeros) {
    uint8_t exps[] = { 0, 0, 0 };
    auto t         = ExponentTuple::FromExponents(exps, 3);
    EXPECT_EQ(t.packed, 0u);
    uint8_t out[3];
    t.ToExponents(out, 3);
    EXPECT_EQ(out[0], 0);
    EXPECT_EQ(out[1], 0);
    EXPECT_EQ(out[2], 0);
}

TEST(ExponentTupleTest, ExponentAt) {
    // (1, 0, 2) for 3 vars
    uint8_t exps[] = { 1, 0, 2 };
    auto t         = ExponentTuple::FromExponents(exps, 3);
    EXPECT_EQ(t.ExponentAt(0, 3), 1);
    EXPECT_EQ(t.ExponentAt(1, 3), 0);
    EXPECT_EQ(t.ExponentAt(2, 3), 2);
}

TEST(ExponentTupleTest, WithExponent) {
    uint8_t exps[] = { 1, 0, 2 };
    auto t         = ExponentTuple::FromExponents(exps, 3);
    auto t2        = t.WithExponent(2, 1, 3);
    EXPECT_EQ(t2.ExponentAt(0, 3), 1);
    EXPECT_EQ(t2.ExponentAt(1, 3), 0);
    EXPECT_EQ(t2.ExponentAt(2, 3), 1);
}

TEST(ExponentTupleTest, TotalDegree) {
    uint8_t exps[] = { 1, 2, 0 };
    auto t         = ExponentTuple::FromExponents(exps, 3);
    EXPECT_EQ(t.TotalDegree(3), 3);
}

TEST(ExponentTupleTest, HashConsistency) {
    uint8_t exps[] = { 1, 2 };
    auto t1        = ExponentTuple::FromExponents(exps, 2);
    auto t2        = ExponentTuple::FromExponents(exps, 2);
    ExponentTupleHash h;
    EXPECT_EQ(h(t1), h(t2));
}

TEST(ExponentTupleTest, LexicographicOrder) {
    // (0, 1) < (1, 0) < (1, 2) < (2, 0)
    uint8_t e01[] = { 0, 1 };
    uint8_t e10[] = { 1, 0 };
    uint8_t e12[] = { 1, 2 };
    uint8_t e20[] = { 2, 0 };
    auto t01      = ExponentTuple::FromExponents(e01, 2);
    auto t10      = ExponentTuple::FromExponents(e10, 2);
    auto t12      = ExponentTuple::FromExponents(e12, 2);
    auto t20      = ExponentTuple::FromExponents(e20, 2);
    EXPECT_LT(t01, t10);
    EXPECT_LT(t10, t12);
    EXPECT_LT(t12, t20);
}

TEST(ExponentTupleTest, MaxVars16) {
    uint8_t exps[16];
    for (int i = 0; i < 16; ++i) { exps[i] = (i % 3); }
    auto t = ExponentTuple::FromExponents(exps, 16);
    uint8_t out[16];
    t.ToExponents(out, 16);
    for (int i = 0; i < 16; ++i) { EXPECT_EQ(out[i], i % 3); }
}
