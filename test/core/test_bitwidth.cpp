#include "cobra/core/BitWidth.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(BitWidthTest, Mask64) { EXPECT_EQ(Bitmask(64), UINT64_MAX); }

TEST(BitWidthTest, Mask8) { EXPECT_EQ(Bitmask(8), 0xFFULL); }

TEST(BitWidthTest, Mask1) { EXPECT_EQ(Bitmask(1), 1ULL); }

TEST(BitWidthTest, ModAdd) { EXPECT_EQ(ModAdd(255, 1, 8), 0ULL); }

TEST(BitWidthTest, ModMul) { EXPECT_EQ(ModMul(200, 2, 8), 144ULL); }

TEST(BitWidthTest, ModNeg) { EXPECT_EQ(ModNeg(1, 8), 255ULL); }

TEST(BitWidthTest, ModNeg64) { EXPECT_EQ(ModNeg(1, 64), UINT64_MAX); }

// mod_sub tests (zero coverage before)
TEST(BitWidthTest, ModSubBasic) { EXPECT_EQ(ModSub(5, 3, 8), 2ULL); }

TEST(BitWidthTest, ModSubWrapping) { EXPECT_EQ(ModSub(3, 5, 8), 254ULL); }

TEST(BitWidthTest, ModSubZero) { EXPECT_EQ(ModSub(0, 1, 8), 255ULL); }

TEST(BitWidthTest, ModSub64) { EXPECT_EQ(ModSub(0, 1, 64), UINT64_MAX); }

TEST(BitWidthTest, ModSub1Bit) {
    EXPECT_EQ(ModSub(0, 1, 1), 1ULL);
    EXPECT_EQ(ModSub(1, 1, 1), 0ULL);
}

// mod_not tests (zero coverage before)
TEST(BitWidthTest, ModNot8Zero) { EXPECT_EQ(ModNot(0, 8), 255ULL); }

TEST(BitWidthTest, ModNot8All) { EXPECT_EQ(ModNot(0xFF, 8), 0ULL); }

TEST(BitWidthTest, ModNot64Zero) { EXPECT_EQ(ModNot(0, 64), UINT64_MAX); }

TEST(BitWidthTest, ModNot1Bit) {
    EXPECT_EQ(ModNot(0, 1), 1ULL);
    EXPECT_EQ(ModNot(1, 1), 0ULL);
}

// mod_mul edge cases
TEST(BitWidthTest, ModMulOverflow8) { EXPECT_EQ(ModMul(128, 2, 8), 0ULL); }

TEST(BitWidthTest, ModMul64Large) {
    // (2^63) * 2 mod 2^64 = 0
    EXPECT_EQ(ModMul(1ULL << 63, 2, 64), 0ULL);
}

TEST(BitWidthTest, ModMul1Bit) {
    EXPECT_EQ(ModMul(1, 1, 1), 1ULL);
    EXPECT_EQ(ModMul(1, 0, 1), 0ULL);
}

// bitmask for intermediate bitwidths
TEST(BitWidthTest, Mask16) { EXPECT_EQ(Bitmask(16), 0xFFFFULL); }

TEST(BitWidthTest, Mask32) { EXPECT_EQ(Bitmask(32), 0xFFFFFFFFULL); }
