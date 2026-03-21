#include "cobra/core/SemilinearNormalizer.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(NormalizerTest, SingleAtom) {
    auto e  = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF));
    auto ir = NormalizeToSemilinear(*e, { "x" }, 64);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().constant, 0u);
    EXPECT_EQ(ir.value().terms.size(), 1u);
    EXPECT_EQ(ir.value().terms[0].coeff, 1u);
    EXPECT_EQ(ir.value().atom_table.size(), 1u);
}

TEST(NormalizerTest, WeightedAtom) {
    auto e =
        Expr::Mul(Expr::Constant(5), Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)));
    auto ir = NormalizeToSemilinear(*e, { "x" }, 64);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().terms.size(), 1u);
    EXPECT_EQ(ir.value().terms[0].coeff, 5u);
}

TEST(NormalizerTest, TwoAtoms) {
    // (x & 0xFF) + (y | 0x80)
    // OR lowering: y | 0x80 → y + 0x80 - (y & 0x80)
    // Result: constant=0x80, atoms: x&0xFF, y, y&0x80
    auto e = Expr::Add(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)),
        Expr::BitwiseOr(Expr::Variable(1), Expr::Constant(0x80))
    );
    auto ir = NormalizeToSemilinear(*e, { "x", "y" }, 64);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().constant, 0x80u);
    EXPECT_EQ(ir.value().terms.size(), 3u);
    EXPECT_EQ(ir.value().atom_table.size(), 3u);
}

TEST(NormalizerTest, ConstantTerm) {
    auto e = Expr::Add(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)), Expr::Constant(42)
    );
    auto ir = NormalizeToSemilinear(*e, { "x" }, 64);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().constant, 42u);
    EXPECT_EQ(ir.value().terms.size(), 1u);
}

TEST(NormalizerTest, SubtractionAsAddNeg) {
    // (x & 0xFF) - (y | 0x80)
    // OR lowering on -(y | 0x80): -y - 0x80 + (y & 0x80)
    // Result: constant=-0x80, atoms: x&0xFF(+1), y(-1), y&0x80(+1)
    auto e = Expr::Add(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)),
        Expr::Negate(Expr::BitwiseOr(Expr::Variable(1), Expr::Constant(0x80)))
    );
    auto ir = NormalizeToSemilinear(*e, { "x", "y" }, 64);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().terms.size(), 3u);
    uint64_t neg_one = UINT64_MAX;
    bool found_neg   = false;
    for (const auto &t : ir.value().terms) {
        if (t.coeff == neg_one) { found_neg = true; }
    }
    EXPECT_TRUE(found_neg);
}

TEST(NormalizerTest, DuplicateAtomsMerged) {
    // (x & 0xFF) + 3 * (x & 0xFF) => one atom, coeff=4
    auto e = Expr::Add(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)),
        Expr::Mul(Expr::Constant(3), Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)))
    );
    auto ir = NormalizeToSemilinear(*e, { "x" }, 64);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().terms.size(), 1u);
    EXPECT_EQ(ir.value().terms[0].coeff, 4u);
    EXPECT_EQ(ir.value().atom_table.size(), 1u);
}

TEST(NormalizerTest, Distribution) {
    // 3 * ((x & 0xFF) + (y | 0x80))
    // OR lowering: y|0x80 → y + 0x80 - (y&0x80), then *3 distributes
    // Result: constant=3*0x80, atoms: x&0xFF(3), y(3), y&0x80(-3)
    auto e = Expr::Mul(
        Expr::Constant(3),
        Expr::Add(
            Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)),
            Expr::BitwiseOr(Expr::Variable(1), Expr::Constant(0x80))
        )
    );
    auto ir = NormalizeToSemilinear(*e, { "x", "y" }, 64);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().constant, 3u * 0x80u);
    EXPECT_EQ(ir.value().terms.size(), 3u);
}

TEST(NormalizerTest, ConstantOnlyBitwiseFolds) {
    // (x & 0xFF) + (~0 & 7) => constant=7, one atom
    auto e = Expr::Add(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)),
        Expr::BitwiseAnd(Expr::BitwiseNot(Expr::Constant(0)), Expr::Constant(7))
    );
    auto ir = NormalizeToSemilinear(*e, { "x" }, 64);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().constant, 7u);
    EXPECT_EQ(ir.value().terms.size(), 1u);
}

TEST(NormalizerTest, PureConstantExpr) {
    auto e  = Expr::Add(Expr::Constant(10), Expr::Constant(32));
    auto ir = NormalizeToSemilinear(*e, {}, 64);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().constant, 42u);
    EXPECT_EQ(ir.value().terms.size(), 0u);
}

TEST(NormalizerTest, NegatedConstant) {
    // -(5) + (x & 0xFF)
    auto e = Expr::Add(
        Expr::Negate(Expr::Constant(5)),
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF))
    );
    auto ir = NormalizeToSemilinear(*e, { "x" }, 64);
    ASSERT_TRUE(ir.has_value());
    uint64_t expected = UINT64_MAX - 5 + 1; // -5 mod 2^64
    EXPECT_EQ(ir.value().constant, expected);
    EXPECT_EQ(ir.value().terms.size(), 1u);
}

TEST(NormalizerTest, BareVariable) {
    auto e  = Expr::Variable(0);
    auto ir = NormalizeToSemilinear(*e, { "x" }, 64);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().constant, 0u);
    EXPECT_EQ(ir.value().terms.size(), 1u);
    EXPECT_EQ(ir.value().terms[0].coeff, 1u);
}

TEST(NormalizerTest, MulOnRightSide) {
    // (x & 0xFF) * 7
    auto e =
        Expr::Mul(Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)), Expr::Constant(7));
    auto ir = NormalizeToSemilinear(*e, { "x" }, 64);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().terms.size(), 1u);
    EXPECT_EQ(ir.value().terms[0].coeff, 7u);
}

TEST(NormalizerTest, ZeroCoefficientDropped) {
    // (x & 0xFF) + (-(x & 0xFF)) => cancels to 0
    auto e = Expr::Add(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)),
        Expr::Negate(Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)))
    );
    auto ir = NormalizeToSemilinear(*e, { "x" }, 64);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().constant, 0u);
    EXPECT_EQ(ir.value().terms.size(), 0u);
}

TEST(NormalizerTest, NarrowBitwidth8) {
    // 200 + (x & 0xFF) in 8-bit
    auto e = Expr::Add(
        Expr::Constant(200), Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF))
    );
    auto ir = NormalizeToSemilinear(*e, { "x" }, 8);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().constant, 200u);
    EXPECT_EQ(ir.value().terms.size(), 1u);
    EXPECT_EQ(ir.value().bitwidth, 8u);
}

TEST(NormalizerTest, NonlinearMulReturnsError) {
    // x * y should fail (not supported in semilinear)
    auto e  = Expr::Mul(Expr::Variable(0), Expr::Variable(1));
    auto ir = NormalizeToSemilinear(*e, { "x", "y" }, 64);
    EXPECT_FALSE(ir.has_value());
    EXPECT_EQ(ir.error().code, CobraError::kNonLinearInput);
}

TEST(NormalizerTest, ShrAtomSingleVar) {
    auto e  = Expr::LogicalShr(Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)), 3);
    auto ir = NormalizeToSemilinear(*e, { "x" }, 64);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().constant, 0u);
    EXPECT_EQ(ir.value().terms.size(), 1u);
    EXPECT_EQ(ir.value().terms[0].coeff, 1u);
}

TEST(NormalizerTest, ShrAtomWeighted) {
    auto e = Expr::Mul(
        Expr::Constant(3),
        Expr::LogicalShr(Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)), 2)
    );
    auto ir = NormalizeToSemilinear(*e, { "x", "y" }, 64);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().terms.size(), 1u);
    EXPECT_EQ(ir.value().terms[0].coeff, 3u);
}

TEST(NormalizerTest, TwoShrAtoms) {
    auto e = Expr::Add(
        Expr::LogicalShr(Expr::Variable(0), 2), Expr::LogicalShr(Expr::Variable(1), 2)
    );
    auto ir = NormalizeToSemilinear(*e, { "x", "y" }, 64);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().terms.size(), 2u);
}

TEST(NormalizerTest, ShrOverArithmeticFails) {
    auto e  = Expr::LogicalShr(Expr::Add(Expr::Variable(0), Expr::Variable(1)), 2);
    auto ir = NormalizeToSemilinear(*e, { "x", "y" }, 64);
    EXPECT_FALSE(ir.has_value());
    EXPECT_EQ(ir.error().code, CobraError::kNonLinearInput);
}

TEST(NormalizerTest, ShrConstantFolds) {
    auto e  = Expr::Add(Expr::LogicalShr(Expr::Constant(0xFF), 4), Expr::Variable(0));
    auto ir = NormalizeToSemilinear(*e, { "x" }, 64);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().constant, 0xFu);
    EXPECT_EQ(ir.value().terms.size(), 1u);
}

TEST(NormalizerTest, ShrAtomDedupSafety) {
    auto e = Expr::Add(
        Expr::LogicalShr(Expr::Variable(0), 2), Expr::LogicalShr(Expr::Variable(0), 3)
    );
    auto ir = NormalizeToSemilinear(*e, { "x" }, 64);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().terms.size(), 2u);
    EXPECT_EQ(ir.value().atom_table.size(), 2u);
}

TEST(NormalizerTest, ShrNestedUnderBitwiseConstant) {
    // ~(0xFF >> 4) + x  should fold the constant to ~0xF = 0xFFFF...F0
    auto e = Expr::Add(
        Expr::BitwiseNot(Expr::LogicalShr(Expr::Constant(0xFF), 4)), Expr::Variable(0)
    );
    auto ir = NormalizeToSemilinear(*e, { "x" }, 64);
    ASSERT_TRUE(ir.has_value());
    uint64_t expected = ~uint64_t(0xF);
    EXPECT_EQ(ir.value().constant, expected);
    EXPECT_EQ(ir.value().terms.size(), 1u);
}

// --- XOR/OR constant lowering tests ---

TEST(NormalizerTest, XorConstantLowering) {
    // x ^ 0x10 → x + 0x10 - 2*(x & 0x10)
    auto e  = Expr::BitwiseXor(Expr::Variable(0), Expr::Constant(0x10));
    auto ir = NormalizeToSemilinear(*e, { "x" }, 64);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().constant, 0x10u);
    // Two atoms: x (coeff 1) and x&0x10 (coeff -2)
    EXPECT_EQ(ir.value().terms.size(), 2u);
}

TEST(NormalizerTest, OrConstantLowering) {
    // x | 0x80 → x + 0x80 - (x & 0x80)
    auto e  = Expr::BitwiseOr(Expr::Variable(0), Expr::Constant(0x80));
    auto ir = NormalizeToSemilinear(*e, { "x" }, 64);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().constant, 0x80u);
    EXPECT_EQ(ir.value().terms.size(), 2u);
}

TEST(NormalizerTest, XorConstantCancellation) {
    // Issue #8: (x ^ 0x10) + 2*(x & 0x10) → x + 0x10
    // XOR lowering: x + 0x10 - 2*(x&0x10) + 2*(x&0x10) → x + 0x10
    auto e = Expr::Add(
        Expr::BitwiseXor(Expr::Variable(0), Expr::Constant(0x10)),
        Expr::Mul(Expr::Constant(2), Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0x10)))
    );
    auto ir = NormalizeToSemilinear(*e, { "x" }, 64);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().constant, 0x10u);
    // x&0x10 coefficients cancel: -2 + 2 = 0, only bare x remains
    EXPECT_EQ(ir.value().terms.size(), 1u);
    EXPECT_EQ(ir.value().terms[0].coeff, 1u);
}

TEST(NormalizerTest, OrConstantCancellation) {
    // (x | 0x10) + (x & 0x10) → x + 0x10
    // OR lowering: x + 0x10 - (x&0x10) + (x&0x10) → x + 0x10
    auto e = Expr::Add(
        Expr::BitwiseOr(Expr::Variable(0), Expr::Constant(0x10)),
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0x10))
    );
    auto ir = NormalizeToSemilinear(*e, { "x" }, 64);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().constant, 0x10u);
    EXPECT_EQ(ir.value().terms.size(), 1u);
    EXPECT_EQ(ir.value().terms[0].coeff, 1u);
}

TEST(NormalizerTest, XorConstantOnLeft) {
    // 0x10 ^ x → same as x ^ 0x10 (constant on left side)
    auto e  = Expr::BitwiseXor(Expr::Constant(0x10), Expr::Variable(0));
    auto ir = NormalizeToSemilinear(*e, { "x" }, 64);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().constant, 0x10u);
    EXPECT_EQ(ir.value().terms.size(), 2u);
}

TEST(NormalizerTest, XorNoConstantNotLowered) {
    // x ^ y (no constants) should remain as a single atom
    auto e  = Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1));
    auto ir = NormalizeToSemilinear(*e, { "x", "y" }, 64);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().constant, 0u);
    EXPECT_EQ(ir.value().terms.size(), 1u);
    EXPECT_EQ(ir.value().atom_table.size(), 1u);
}

TEST(NormalizerTest, AndOrCancellation) {
    // (x & 0xFF) + (x | 0xFF) = x + 0xFF
    // OR lowering: x + 0xFF - (x & 0xFF), then + (x & 0xFF) cancels
    auto e = Expr::Add(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)),
        Expr::BitwiseOr(Expr::Variable(0), Expr::Constant(0xFF))
    );
    auto ir = NormalizeToSemilinear(*e, { "x" }, 64);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().constant, 0xFFu);
    // x&0xFF cancels (1 + -1 = 0), only bare x remains
    EXPECT_EQ(ir.value().terms.size(), 1u);
    EXPECT_EQ(ir.value().terms[0].coeff, 1u);
}

TEST(NormalizerTest, NotAndConstantLowering) {
    // (~x & 0x10) lowers to 0x10 - (x & 0x10)
    auto e  = Expr::BitwiseAnd(Expr::BitwiseNot(Expr::Variable(0)), Expr::Constant(0x10));
    auto ir = NormalizeToSemilinear(*e, { "x" }, 8);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().constant, 0x10u);
    EXPECT_EQ(ir.value().terms.size(), 1u);
    // coeff should be -1 mod 256 = 0xFF
    EXPECT_EQ(ir.value().terms[0].coeff, 0xFFu);
}

TEST(NormalizerTest, NotAndConstantCancellation) {
    // (~x & 0x10) + (x & 0x10) = 0x10 (constant)
    auto e = Expr::Add(
        Expr::BitwiseAnd(Expr::BitwiseNot(Expr::Variable(0)), Expr::Constant(0x10)),
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0x10))
    );
    auto ir = NormalizeToSemilinear(*e, { "x" }, 8);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().constant, 0x10u);
    EXPECT_EQ(ir.value().terms.size(), 0u);
}

TEST(NormalizerTest, ConstAndNotLowering) {
    // (0x10 & ~x) — constant on left side
    auto e  = Expr::BitwiseAnd(Expr::Constant(0x10), Expr::BitwiseNot(Expr::Variable(0)));
    auto ir = NormalizeToSemilinear(*e, { "x" }, 8);
    ASSERT_TRUE(ir.has_value());
    EXPECT_EQ(ir.value().constant, 0x10u);
    EXPECT_EQ(ir.value().terms.size(), 1u);
    EXPECT_EQ(ir.value().terms[0].coeff, 0xFFu);
}

TEST(SemilinearNormalizerTest, StructuralHashDedup) {
    // (x & 0xFF) + (x & 0xFF) should produce one atom with coeff 2
    auto input = Expr::Add(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)),
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF))
    );
    auto result = NormalizeToSemilinear(*input, { "x" }, 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().atom_table.size(), 1u);
    EXPECT_EQ(result.value().terms.size(), 1u);
    EXPECT_EQ(result.value().terms[0].coeff, 2u);
    // Verify structural hash is populated
    EXPECT_NE(result.value().atom_table[0].structural_hash, 0ULL);
}
