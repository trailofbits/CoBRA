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
