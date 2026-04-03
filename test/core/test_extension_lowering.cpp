#include "cobra/core/ExtensionLowering.h"

#include <cstdint>
#include <gtest/gtest.h>

using namespace cobra;

// ---------- EvalZeroExtend ----------

TEST(EvalZeroExtendTest, OneBitWidth) {
    // 1-bit zext: only bit 0 survives.
    constexpr uint64_t kMask64 = UINT64_MAX;
    EXPECT_EQ(EvalZeroExtend(0, 1, kMask64), 0u);
    EXPECT_EQ(EvalZeroExtend(1, 1, kMask64), 1u);
    EXPECT_EQ(EvalZeroExtend(0xFF, 1, kMask64), 1u);
}

TEST(EvalZeroExtendTest, EightBitWidth) {
    constexpr uint64_t kMask64 = UINT64_MAX;
    EXPECT_EQ(EvalZeroExtend(0, 8, kMask64), 0u);
    EXPECT_EQ(EvalZeroExtend(0x7F, 8, kMask64), 0x7Fu);
    EXPECT_EQ(EvalZeroExtend(0x80, 8, kMask64), 0x80u);
    EXPECT_EQ(EvalZeroExtend(0xFF, 8, kMask64), 0xFFu);
    // Upper bits cleared.
    EXPECT_EQ(EvalZeroExtend(0xDEAD00FF, 8, kMask64), 0xFFu);
}

TEST(EvalZeroExtendTest, ThirtyTwoBitWidth) {
    constexpr uint64_t kMask64 = UINT64_MAX;
    EXPECT_EQ(EvalZeroExtend(0, 32, kMask64), 0u);
    EXPECT_EQ(EvalZeroExtend(0x7FFFFFFFu, 32, kMask64), 0x7FFFFFFFu);
    EXPECT_EQ(EvalZeroExtend(0x80000000u, 32, kMask64), 0x80000000u);
    EXPECT_EQ(EvalZeroExtend(0xFFFFFFFFu, 32, kMask64), 0xFFFFFFFFu);
    EXPECT_EQ(EvalZeroExtend(0xDEADBEEF12345678ULL, 32, kMask64), 0x12345678u);
}

TEST(EvalZeroExtendTest, SixtyFourBitIsIdentity) {
    constexpr uint64_t kMask64 = UINT64_MAX;
    EXPECT_EQ(EvalZeroExtend(0, 64, kMask64), 0u);
    EXPECT_EQ(EvalZeroExtend(0xDEADBEEFCAFEBABEULL, 64, kMask64), 0xDEADBEEFCAFEBABEULL);
}

TEST(EvalZeroExtendTest, ResultMaskApplied) {
    // 8-bit zext into a 32-bit result mask.
    constexpr uint64_t kMask32 = 0xFFFFFFFFu;
    EXPECT_EQ(EvalZeroExtend(0xFF, 8, kMask32), 0xFFu);
    // 32-bit zext into a 32-bit result mask.
    EXPECT_EQ(EvalZeroExtend(0xFFFFFFFFFFFFFFFFULL, 32, kMask32), 0xFFFFFFFFu);
}

// ---------- EvalSignExtend ----------

TEST(EvalSignExtendTest, OneBitWidth) {
    constexpr uint64_t kMask64 = UINT64_MAX;
    // sext i1 0 -> 0
    EXPECT_EQ(EvalSignExtend(0, 1, kMask64), 0u);
    // sext i1 1 -> all ones (sign bit set in 1-bit value)
    EXPECT_EQ(EvalSignExtend(1, 1, kMask64), UINT64_MAX);
}

TEST(EvalSignExtendTest, EightBitWidth) {
    constexpr uint64_t kMask64 = UINT64_MAX;
    EXPECT_EQ(EvalSignExtend(0, 8, kMask64), 0u);
    EXPECT_EQ(EvalSignExtend(0x7F, 8, kMask64), 0x7Fu);
    // 0x80 has sign bit set -> extends to all-F upper bits.
    EXPECT_EQ(EvalSignExtend(0x80, 8, kMask64), 0xFFFFFFFFFFFFFF80ULL);
    EXPECT_EQ(EvalSignExtend(0xFF, 8, kMask64), 0xFFFFFFFFFFFFFFFFULL);
}

TEST(EvalSignExtendTest, ThirtyTwoBitWidth) {
    constexpr uint64_t kMask64 = UINT64_MAX;
    EXPECT_EQ(EvalSignExtend(0, 32, kMask64), 0u);
    EXPECT_EQ(EvalSignExtend(0x7FFFFFFFu, 32, kMask64), 0x7FFFFFFFu);
    EXPECT_EQ(EvalSignExtend(0x80000000u, 32, kMask64), 0xFFFFFFFF80000000ULL);
    EXPECT_EQ(EvalSignExtend(0xFFFFFFFFu, 32, kMask64), 0xFFFFFFFFFFFFFFFFULL);
}

TEST(EvalSignExtendTest, SixtyFourBitIsIdentity) {
    constexpr uint64_t kMask64 = UINT64_MAX;
    EXPECT_EQ(EvalSignExtend(0, 64, kMask64), 0u);
    EXPECT_EQ(EvalSignExtend(0xDEADBEEFCAFEBABEULL, 64, kMask64), 0xDEADBEEFCAFEBABEULL);
}

TEST(EvalSignExtendTest, ResultMaskApplied) {
    // sext 8-bit 0x80 into 32-bit result mask: upper 32 bits cleared.
    constexpr uint64_t kMask32 = 0xFFFFFFFFu;
    EXPECT_EQ(EvalSignExtend(0x80, 8, kMask32), 0xFFFFFF80u);
    // sext 8-bit 0xFF into 32-bit result mask.
    EXPECT_EQ(EvalSignExtend(0xFF, 8, kMask32), 0xFFFFFFFFu);
}

#if defined(GTEST_HAS_DEATH_TEST) && !defined(NDEBUG)
TEST(EvalZeroExtendDeathTest, ZeroBitsAsserts) {
    EXPECT_DEATH(EvalZeroExtend(0, 0, UINT64_MAX), "");
}

TEST(EvalZeroExtendDeathTest, OverSixtyFourBitsAsserts) {
    EXPECT_DEATH(EvalZeroExtend(0, 65, UINT64_MAX), "");
}

TEST(EvalSignExtendDeathTest, ZeroBitsAsserts) {
    EXPECT_DEATH(EvalSignExtend(0, 0, UINT64_MAX), "");
}

TEST(EvalSignExtendDeathTest, OverSixtyFourBitsAsserts) {
    EXPECT_DEATH(EvalSignExtend(0, 65, UINT64_MAX), "");
}
#endif
