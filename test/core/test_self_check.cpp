#include "cobra/core/Expr.h"
#include "cobra/core/MaskedAtomReconstructor.h"
#include "cobra/core/SelfCheck.h"
#include "cobra/core/SemilinearIR.h"
#include "cobra/core/SemilinearNormalizer.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(SelfCheckTest, IdenticalIRPasses) {
    auto input = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF));
    auto ir    = NormalizeToSemilinear(*input, { "x" }, 64);
    ASSERT_TRUE(ir.has_value());
    auto reconstructed = ReconstructMaskedAtoms(ir.value(), {});
    ASSERT_NE(reconstructed, nullptr);
    auto result = SelfCheckSemilinear(ir.value(), *reconstructed, { "x" }, 64);
    EXPECT_TRUE(result.passed);
}

TEST(SelfCheckTest, DetectsMismatch) {
    auto input = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF));
    auto ir    = NormalizeToSemilinear(*input, { "x" }, 64);
    ASSERT_TRUE(ir.has_value());
    auto wrong  = Expr::BitwiseOr(Expr::Variable(0), Expr::Constant(0xFF));
    auto result = SelfCheckSemilinear(ir.value(), *wrong, { "x" }, 64);
    EXPECT_FALSE(result.passed);
}

TEST(SelfCheckTest, TwoAtomRoundTrip) {
    auto input = Expr::Add(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)),
        Expr::BitwiseOr(Expr::Variable(1), Expr::Constant(0x80))
    );
    auto ir = NormalizeToSemilinear(*input, { "x", "y" }, 64);
    ASSERT_TRUE(ir.has_value());
    auto reconstructed = ReconstructMaskedAtoms(ir.value(), {});
    ASSERT_NE(reconstructed, nullptr);
    auto result = SelfCheckSemilinear(ir.value(), *reconstructed, { "x", "y" }, 64);
    EXPECT_TRUE(result.passed);
}

TEST(SelfCheckTest, WithConstant) {
    auto input = Expr::Add(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)), Expr::Constant(42)
    );
    auto ir = NormalizeToSemilinear(*input, { "x" }, 64);
    ASSERT_TRUE(ir.has_value());
    auto reconstructed = ReconstructMaskedAtoms(ir.value(), {});
    ASSERT_NE(reconstructed, nullptr);
    auto result = SelfCheckSemilinear(ir.value(), *reconstructed, { "x" }, 64);
    EXPECT_TRUE(result.passed);
}

// Round-trip tests for NOT(x) under XOR/OR constant lowering.
// These verify the fix for the (~a)&c asymmetry: the XOR/OR lowering
// synthesizes AND(var_child, C) and must process it through CollectTerms
// (not RegisterAtom) so that AND(NOT(x), C) is further lowered to
// C - (x & C) on the first pass, matching re-normalization.

TEST(SelfCheckTest, NotXorConstant8bit) {
    // ~x ^ 0x10 at 8-bit
    auto input = Expr::BitwiseXor(Expr::BitwiseNot(Expr::Variable(0)), Expr::Constant(0x10));
    auto ir    = NormalizeToSemilinear(*input, { "x" }, 8);
    ASSERT_TRUE(ir.has_value());
    auto reconstructed = ReconstructMaskedAtoms(ir.value(), {});
    ASSERT_NE(reconstructed, nullptr);
    auto result = SelfCheckSemilinear(ir.value(), *reconstructed, { "x" }, 8);
    EXPECT_TRUE(result.passed) << result.mismatch_detail;
}

TEST(SelfCheckTest, NotOrConstant8bit) {
    // ~x | 0x10 at 8-bit
    auto input = Expr::BitwiseOr(Expr::BitwiseNot(Expr::Variable(0)), Expr::Constant(0x10));
    auto ir    = NormalizeToSemilinear(*input, { "x" }, 8);
    ASSERT_TRUE(ir.has_value());
    auto reconstructed = ReconstructMaskedAtoms(ir.value(), {});
    ASSERT_NE(reconstructed, nullptr);
    auto result = SelfCheckSemilinear(ir.value(), *reconstructed, { "x" }, 8);
    EXPECT_TRUE(result.passed) << result.mismatch_detail;
}

TEST(SelfCheckTest, NotXorConstant64bit) {
    // ~x ^ C at 64-bit (MSiMBA-representative)
    auto input = Expr::BitwiseXor(
        Expr::BitwiseNot(Expr::Variable(0)), Expr::Constant(0x2EBAE2A0AD330C5FULL)
    );
    auto ir = NormalizeToSemilinear(*input, { "x" }, 64);
    ASSERT_TRUE(ir.has_value());
    auto reconstructed = ReconstructMaskedAtoms(ir.value(), {});
    ASSERT_NE(reconstructed, nullptr);
    auto result = SelfCheckSemilinear(ir.value(), *reconstructed, { "x" }, 64);
    EXPECT_TRUE(result.passed) << result.mismatch_detail;
}

TEST(SelfCheckTest, NestedNotXorOrConstant) {
    // ((~x ^ C1) | C2) ^ C3 — nested lowering chain from MSiMBA
    auto inner = Expr::BitwiseXor(Expr::BitwiseNot(Expr::Variable(0)), Expr::Constant(0x42));
    auto mid   = Expr::BitwiseOr(std::move(inner), Expr::Constant(0x1F));
    auto input = Expr::BitwiseXor(std::move(mid), Expr::Constant(0xA5));
    auto ir    = NormalizeToSemilinear(*input, { "x" }, 8);
    ASSERT_TRUE(ir.has_value());
    auto reconstructed = ReconstructMaskedAtoms(ir.value(), {});
    ASSERT_NE(reconstructed, nullptr);
    auto result = SelfCheckSemilinear(ir.value(), *reconstructed, { "x" }, 8);
    EXPECT_TRUE(result.passed) << result.mismatch_detail;
}
