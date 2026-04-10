#include "cobra/core/DynamicMask.h"
#include "cobra/core/Expr.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(DynamicMaskTest, IsPowerOfTwoMinusOne_Basics) {
    EXPECT_EQ(IsPowerOfTwoMinusOne(0), std::nullopt);
    EXPECT_EQ(IsPowerOfTwoMinusOne(1), 1);  // 2^1 - 1
    EXPECT_EQ(IsPowerOfTwoMinusOne(3), 2);  // 2^2 - 1
    EXPECT_EQ(IsPowerOfTwoMinusOne(7), 3);  // 2^3 - 1
    EXPECT_EQ(IsPowerOfTwoMinusOne(15), 4); // 2^4 - 1
    EXPECT_EQ(IsPowerOfTwoMinusOne(31), 5);
    EXPECT_EQ(IsPowerOfTwoMinusOne(255), 8);
    EXPECT_EQ(IsPowerOfTwoMinusOne(0xFFFF), 16);
    EXPECT_EQ(IsPowerOfTwoMinusOne(0xFFFFFFFF), 32);
}

TEST(DynamicMaskTest, IsPowerOfTwoMinusOne_NonMasks) {
    EXPECT_EQ(IsPowerOfTwoMinusOne(2), std::nullopt);
    EXPECT_EQ(IsPowerOfTwoMinusOne(5), std::nullopt);
    EXPECT_EQ(IsPowerOfTwoMinusOne(6), std::nullopt);
    EXPECT_EQ(IsPowerOfTwoMinusOne(10), std::nullopt);
    EXPECT_EQ(IsPowerOfTwoMinusOne(100), std::nullopt);
    EXPECT_EQ(IsPowerOfTwoMinusOne(256), std::nullopt);
    // UINT64_MAX = 2^64 - 1: val+1 overflows to 0, correctly rejected.
    EXPECT_EQ(IsPowerOfTwoMinusOne(UINT64_MAX), std::nullopt);
}

TEST(DynamicMaskTest, DetectRootLowBitMask_ConstantFirst) {
    auto expr =
        Expr::BitwiseAnd(Expr::Constant(0xFF), Expr::Add(Expr::Variable(0), Expr::Variable(1)));
    auto mask = DetectRootLowBitMask(*expr, 64);
    ASSERT_TRUE(mask.has_value());
    EXPECT_EQ(mask->effective_width, 8);
    EXPECT_EQ(mask->inner->kind, Expr::Kind::kAdd);
}

TEST(DynamicMaskTest, DetectRootLowBitMask_ConstantSecond) {
    auto expr = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xF));
    auto mask = DetectRootLowBitMask(*expr, 64);
    ASSERT_TRUE(mask.has_value());
    EXPECT_EQ(mask->effective_width, 4);
    EXPECT_EQ(mask->inner->kind, Expr::Kind::kVariable);
}

TEST(DynamicMaskTest, DetectRootLowBitMask_FullWidthMaskRejected) {
    // mask = 2^64 - 1 covers full width — no reduction possible
    auto expr = Expr::BitwiseAnd(Expr::Constant(UINT64_MAX), Expr::Variable(0));
    auto mask = DetectRootLowBitMask(*expr, 64);
    EXPECT_FALSE(mask.has_value());
}

TEST(DynamicMaskTest, DetectRootLowBitMask_NonMaskConstant) {
    auto expr = Expr::BitwiseAnd(Expr::Constant(0xAB), Expr::Variable(0));
    auto mask = DetectRootLowBitMask(*expr, 64);
    EXPECT_FALSE(mask.has_value());
}

TEST(DynamicMaskTest, DetectRootLowBitMask_NotAndNode) {
    auto expr = Expr::Add(Expr::Constant(0xFF), Expr::Variable(0));
    auto mask = DetectRootLowBitMask(*expr, 64);
    EXPECT_FALSE(mask.has_value());
}

TEST(DynamicMaskTest, ContainsShr_False) {
    auto expr = Expr::Add(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)),
        Expr::BitwiseNot(Expr::Variable(0))
    );
    EXPECT_FALSE(ContainsShr(*expr));
}

TEST(DynamicMaskTest, ContainsShr_True) {
    auto expr = Expr::Add(Expr::LogicalShr(Expr::Variable(0), 3), Expr::Variable(1));
    EXPECT_TRUE(ContainsShr(*expr));
}

TEST(DynamicMaskTest, ContainsShr_Deep) {
    auto expr = Expr::BitwiseAnd(
        Expr::Add(
            Expr::Variable(0),
            Expr::BitwiseOr(Expr::Variable(1), Expr::LogicalShr(Expr::Variable(0), 1))
        ),
        Expr::Constant(0xFF)
    );
    EXPECT_TRUE(ContainsShr(*expr));
}
