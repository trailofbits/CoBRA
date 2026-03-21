#include "cobra/core/Expr.h"
#include "cobra/core/SemilinearSignature.h"
#include <gtest/gtest.h>

using namespace cobra;

// --- EvaluateSemilinearRow tests ---

TEST(SemilinearSignatureTest, LinearExprAllRowsIdentical) {
    // x is linear: evaluating at {0, 2^i} and right-shifting by i
    // always gives [0, 1] for any bit position.
    auto e = Expr::Variable(0);
    for (uint32_t bit = 0; bit < 8; ++bit) {
        auto row = EvaluateSemilinearRow(*e, 1, 8, bit);
        ASSERT_EQ(row.size(), 2u);
        EXPECT_EQ(row[0], 0u) << "bit=" << bit;
        EXPECT_EQ(row[1], 1u) << "bit=" << bit;
    }
}

TEST(SemilinearSignatureTest, AddLinearRowsOverflowAtTopBit) {
    // x + y in 8-bit: rows 0-6 give [0, 1, 1, 2].
    // At bit 7: both vars at 128 → 256 mod 256 = 0, >>7 = 0. Row = [0, 1, 1, 0].
    auto e = Expr::Add(Expr::Variable(0), Expr::Variable(1));
    for (uint32_t bit = 0; bit < 7; ++bit) {
        auto row = EvaluateSemilinearRow(*e, 2, 8, bit);
        ASSERT_EQ(row.size(), 4u);
        EXPECT_EQ(row[0], 0u) << "bit=" << bit;
        EXPECT_EQ(row[1], 1u) << "bit=" << bit;
        EXPECT_EQ(row[2], 1u) << "bit=" << bit;
        EXPECT_EQ(row[3], 2u) << "bit=" << bit;
    }
    // Bit 7: overflow at (128, 128)
    auto row7 = EvaluateSemilinearRow(*e, 2, 8, 7);
    EXPECT_EQ(row7[0], 0u);
    EXPECT_EQ(row7[1], 1u);
    EXPECT_EQ(row7[2], 1u);
    EXPECT_EQ(row7[3], 0u); // 256 mod 256 = 0
}

TEST(SemilinearSignatureTest, MaskedAtomDiffersAcrossBits) {
    // x & 0x0F in 8-bit: bits 0-3 are active, bits 4-7 are not.
    // At bit 0: x=1, 0x0F & 1 = 1 → right-shift 0 → row=[0, 1]. Active.
    // At bit 4: x=16, 0x0F & 16 = 0 → right-shift 4 → row=[0, 0]. Inactive.
    auto e = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0x0F));

    auto row0 = EvaluateSemilinearRow(*e, 1, 8, 0);
    EXPECT_EQ(row0[1], 1u);

    auto row4 = EvaluateSemilinearRow(*e, 1, 8, 4);
    EXPECT_EQ(row4[1], 0u);

    EXPECT_NE(row0, row4);
}

TEST(SemilinearSignatureTest, XorWithConstant) {
    // x ^ 0x10 in 8-bit:
    // At bit 0: x=1, 1^0x10 = 0x11, >>0 = 0x11. Masked to 8-bit: 0x11.
    // At bit 4: x=16, 16^0x10 = 0, >>4 = 0.
    auto e = Expr::BitwiseXor(Expr::Variable(0), Expr::Constant(0x10));

    auto row0 = EvaluateSemilinearRow(*e, 1, 8, 0);
    ASSERT_EQ(row0.size(), 2u);
    EXPECT_EQ(row0[0], 0x10u); // x=0: 0^0x10=0x10, >>0 = 0x10
    EXPECT_EQ(row0[1], 0x11u); // x=1: 1^0x10=0x11, >>0 = 0x11

    auto row4 = EvaluateSemilinearRow(*e, 1, 8, 4);
    ASSERT_EQ(row4.size(), 2u);
    EXPECT_EQ(row4[0], 1u); // x=0: 0^0x10=0x10, >>4 = 1
    EXPECT_EQ(row4[1], 0u); // x=16: 16^0x10=0, >>4 = 0
}

// --- IsLinearShortcut tests ---

TEST(SemilinearSignatureTest, LinearShortcutTrueForVariable) {
    auto e = Expr::Variable(0);
    EXPECT_TRUE(IsLinearShortcut(*e, 1, 8));
}

TEST(SemilinearSignatureTest, LinearShortcutTrueForSum) {
    auto e = Expr::Add(Expr::Variable(0), Expr::Variable(1));
    EXPECT_TRUE(IsLinearShortcut(*e, 2, 8));
}

TEST(SemilinearSignatureTest, LinearShortcutTrueForAndProduct) {
    // x & y is linear (evaluates identically at all bit positions)
    auto e = Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1));
    EXPECT_TRUE(IsLinearShortcut(*e, 2, 8));
}

TEST(SemilinearSignatureTest, LinearShortcutFalseForMaskedAtom) {
    // x & 0xFF is NOT linear in 16-bit (different behavior at bits 0-7 vs 8-15)
    auto e = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF));
    EXPECT_FALSE(IsLinearShortcut(*e, 1, 16));
}

TEST(SemilinearSignatureTest, LinearShortcutTrueForComplementSum) {
    // (x & 0x0F) + (x & 0xF0) = x in 8-bit — linear in disguise.
    auto e = Expr::Add(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0x0F)),
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xF0))
    );
    EXPECT_TRUE(IsLinearShortcut(*e, 1, 8));
}

TEST(SemilinearSignatureTest, LinearShortcutFalseForSemilinear) {
    // 3*(x & 0x0F) is genuinely semilinear in 8-bit
    auto e =
        Expr::Mul(Expr::Constant(3), Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0x0F)));
    EXPECT_FALSE(IsLinearShortcut(*e, 1, 8));
}
