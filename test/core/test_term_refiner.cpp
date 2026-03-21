#include "cobra/core/Expr.h"
#include "cobra/core/SemilinearIR.h"
#include "cobra/core/SemilinearNormalizer.h"
#include "cobra/core/TermRefiner.h"
#include <gtest/gtest.h>

using namespace cobra;

// --- CanChangeCoefficientTo tests ---

TEST(TermRefinerTest, SameCoefficientAlwaysTrue) {
    EXPECT_TRUE(CanChangeCoefficientTo(5, 5, 0xFF, 8));
    EXPECT_TRUE(CanChangeCoefficientTo(42, 42, 0xFFFF, 64));
}

TEST(TermRefinerTest, ZeroCoefficientAlwaysTrue) {
    // 0 * (mask & x) = 0 for any new coefficient is FALSE unless new is also 0
    EXPECT_TRUE(CanChangeCoefficientTo(0, 0, 0xFF, 8));
    EXPECT_FALSE(CanChangeCoefficientTo(0, 1, 0xFF, 8));
}

TEST(TermRefinerTest, CoefficientKillsMask8Bit) {
    // 64 * (192 & x) mod 256: bits 6,7 of mask are set, but 64*64=4096=0,
    // 64*128=8192=0 mod 256. So coefficient can be changed to 0.
    EXPECT_TRUE(CanChangeCoefficientTo(64, 0, 192, 8));
}

TEST(TermRefinerTest, CoefficientCannotChange) {
    // 1 * (0xFF & x) cannot become 2 * (0xFF & x)
    EXPECT_FALSE(CanChangeCoefficientTo(1, 2, 0xFF, 8));
}

// --- CanChangeMaskTo tests ---

TEST(TermRefinerTest, SameMaskAlwaysTrue) { EXPECT_TRUE(CanChangeMaskTo(5, 0xFF, 0xFF, 8)); }

TEST(TermRefinerTest, MaskReduction8Bit) {
    // 64 * (130 & x) mod 256: only bit 1 of mask 130 (=0x82) contributes.
    // 64*2=128≠0, 64*128=0. So mask 130 can be reduced to 2.
    EXPECT_TRUE(CanChangeMaskTo(64, 130, 2, 8));
}

TEST(TermRefinerTest, MaskCannotChange) {
    // 1 * (0xFF & x) cannot become 1 * (0x0F & x)
    EXPECT_FALSE(CanChangeMaskTo(1, 0xFF, 0x0F, 8));
}

// --- ReduceMask tests ---

TEST(TermRefinerTest, ReduceMaskStripsDeadBits) {
    // 64 * (130 & x) mod 256: bit 7 (128) is dead. Only bit 1 (2) survives.
    EXPECT_EQ(ReduceMask(64, 130, 8), 2u);
}

TEST(TermRefinerTest, ReduceMaskNoChange) {
    // 1 * (0xFF & x): all bits contribute
    EXPECT_EQ(ReduceMask(1, 0xFF, 8), 0xFFu);
}

TEST(TermRefinerTest, ReduceMaskAllDead) {
    // 64 * (192 & x) mod 256: all masked bits are killed
    EXPECT_EQ(ReduceMask(64, 192, 8), 0u);
}

// --- RefineTerms tests ---

TEST(TermRefinerTest, MergeSameCoefficientDisjointMasks) {
    // 1 * (x & 0x0F) + 1 * (x & 0xF0) → 1 * (x & 0xFF)
    auto e = Expr::Add(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0x0F)),
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xF0))
    );
    auto ir_r = NormalizeToSemilinear(*e, { "x" }, 8);
    ASSERT_TRUE(ir_r.has_value());
    auto &ir = ir_r.value();
    EXPECT_EQ(ir.terms.size(), 2u);

    RefineTerms(ir);
    EXPECT_EQ(ir.terms.size(), 1u);
    EXPECT_EQ(ir.terms[0].coeff, 1u);
}

TEST(TermRefinerTest, DiscardZeroEffectiveTerm) {
    // 64 * (192 & x) mod 256 = 0, should be discarded
    auto e =
        Expr::Mul(Expr::Constant(64), Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(192)));
    auto ir_r = NormalizeToSemilinear(*e, { "x" }, 8);
    ASSERT_TRUE(ir_r.has_value());
    auto &ir = ir_r.value();
    EXPECT_EQ(ir.terms.size(), 1u);

    RefineTerms(ir);
    EXPECT_EQ(ir.terms.size(), 0u);
}

TEST(TermRefinerTest, CoefficientMatchThenMerge) {
    // In 8-bit: 64 * (130 & x) + 64 * (192 & x)
    // 64*(192&x) is zero (killed), 64*(130&x) reduces mask to 2.
    // Result: 64 * (2 & x)
    auto e = Expr::Add(
        Expr::Mul(Expr::Constant(64), Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(130))),
        Expr::Mul(Expr::Constant(64), Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(192)))
    );
    auto ir_r = NormalizeToSemilinear(*e, { "x" }, 8);
    ASSERT_TRUE(ir_r.has_value());
    auto &ir = ir_r.value();

    RefineTerms(ir);
    EXPECT_EQ(ir.terms.size(), 1u);
    EXPECT_EQ(ir.terms[0].coeff, 64u);
}

TEST(TermRefinerTest, NoRefinementForOpaqueAtoms) {
    // (x ^ y) + (x & y): both are pure-variable atoms, no constant masks
    // RefineTerms should leave them untouched.
    auto e = Expr::Add(
        Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1)),
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1))
    );
    auto ir_r = NormalizeToSemilinear(*e, { "x", "y" }, 64);
    ASSERT_TRUE(ir_r.has_value());
    auto &ir    = ir_r.value();
    size_t orig = ir.terms.size();

    RefineTerms(ir);
    EXPECT_EQ(ir.terms.size(), orig);
}

TEST(TermRefinerTest, SingleTermNoChange) {
    // Single term: nothing to merge
    auto e  = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF));
    auto ir = NormalizeToSemilinear(*e, { "x" }, 64);
    ASSERT_TRUE(ir.has_value());

    RefineTerms(ir.value());
    EXPECT_EQ(ir.value().terms.size(), 1u);
}
