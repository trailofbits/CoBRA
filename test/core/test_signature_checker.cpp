#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include "cobra/core/SignatureChecker.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(SignatureCheckerTest, EquivalentExpressions) {
    std::vector< uint64_t > original_sig = { 0, 1, 1, 2 };
    auto simplified                      = Expr::Add(Expr::Variable(0), Expr::Variable(1));

    auto result = SignatureCheck(original_sig, *simplified, 2, 64);
    EXPECT_TRUE(result.passed);
}

TEST(SignatureCheckerTest, NonEquivalentExpressions) {
    std::vector< uint64_t > original_sig = { 0, 1, 1, 2 };
    auto simplified                      = Expr::Variable(0);

    auto result = SignatureCheck(original_sig, *simplified, 2, 64);
    EXPECT_FALSE(result.passed);
    EXPECT_FALSE(result.failing_input.empty());
}

TEST(SignatureCheckerTest, ConstantEquivalent) {
    std::vector< uint64_t > original_sig = { 42, 42, 42, 42 };
    auto simplified                      = Expr::Constant(42);

    auto result = SignatureCheck(original_sig, *simplified, 2, 64);
    EXPECT_TRUE(result.passed);
}

TEST(SignatureCheckerTest, Bitwidth8) {
    std::vector< uint64_t > original_sig = { 0, 1, 1, 2 };
    auto simplified                      = Expr::Add(Expr::Variable(0), Expr::Variable(1));

    auto result = SignatureCheck(original_sig, *simplified, 2, 8);
    EXPECT_TRUE(result.passed);
}

// Direct eval_expr tests for all Expr kinds
TEST(SignatureCheckerTest, EvalMul) {
    auto e = Expr::Mul(Expr::Constant(3), Expr::Variable(0));
    EXPECT_EQ(EvalExpr(*e, { 5 }, 64), 15u);
    EXPECT_EQ(EvalExpr(*e, { 100 }, 8), 44u); // 300 mod 256
}

TEST(SignatureCheckerTest, EvalAnd) {
    auto e = Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1));
    EXPECT_EQ(EvalExpr(*e, { 0, 0 }, 64), 0u);
    EXPECT_EQ(EvalExpr(*e, { 1, 1 }, 64), 1u);
    EXPECT_EQ(EvalExpr(*e, { 1, 0 }, 64), 0u);
}

TEST(SignatureCheckerTest, EvalOr) {
    auto e = Expr::BitwiseOr(Expr::Variable(0), Expr::Variable(1));
    EXPECT_EQ(EvalExpr(*e, { 0, 0 }, 64), 0u);
    EXPECT_EQ(EvalExpr(*e, { 1, 0 }, 64), 1u);
    EXPECT_EQ(EvalExpr(*e, { 0, 1 }, 64), 1u);
    EXPECT_EQ(EvalExpr(*e, { 1, 1 }, 64), 1u);
}

TEST(SignatureCheckerTest, EvalXor) {
    auto e = Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1));
    EXPECT_EQ(EvalExpr(*e, { 0, 0 }, 64), 0u);
    EXPECT_EQ(EvalExpr(*e, { 1, 1 }, 64), 0u);
    EXPECT_EQ(EvalExpr(*e, { 1, 0 }, 64), 1u);
}

TEST(SignatureCheckerTest, EvalNot) {
    auto e = Expr::BitwiseNot(Expr::Variable(0));
    EXPECT_EQ(EvalExpr(*e, { 0 }, 8), 255u);
    EXPECT_EQ(EvalExpr(*e, { 1 }, 8), 254u);
}

TEST(SignatureCheckerTest, EvalNeg) {
    auto e = Expr::Negate(Expr::Variable(0));
    EXPECT_EQ(EvalExpr(*e, { 1 }, 8), 255u);
    EXPECT_EQ(EvalExpr(*e, { 0 }, 8), 0u);
}

// Bitwidth=1 signature check
TEST(SignatureCheckerTest, Bitwidth1XorEquiv) {
    // At 1-bit: x + y mod 2 = XOR. sig = [0, 1, 1, 0]
    std::vector< uint64_t > sig = { 0, 1, 1, 0 };
    auto simplified             = Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1));
    auto result                 = SignatureCheck(sig, *simplified, 2, 1);
    EXPECT_TRUE(result.passed);
}

// Three-variable signature check
TEST(SignatureCheckerTest, ThreeVarAffine) {
    // 3*x + 5*y + 7*z
    std::vector< uint64_t > sig(8);
    for (uint32_t i = 0; i < 8; ++i) {
        uint64_t x = (i >> 0) & 1;
        uint64_t y = (i >> 1) & 1;
        uint64_t z = (i >> 2) & 1;
        sig[i]     = 3 * x + 5 * y + 7 * z;
    }
    auto simplified = Expr::Add(
        Expr::Add(
            Expr::Mul(Expr::Constant(3), Expr::Variable(0)),
            Expr::Mul(Expr::Constant(5), Expr::Variable(1))
        ),
        Expr::Mul(Expr::Constant(7), Expr::Variable(2))
    );
    auto result = SignatureCheck(sig, *simplified, 3, 64);
    EXPECT_TRUE(result.passed);
}

// Bitwidth=1 with 3 vars
TEST(SignatureCheckerTest, Bitwidth1ThreeVar) {
    // x XOR y XOR z at 1-bit
    std::vector< uint64_t > sig(8);
    for (uint32_t i = 0; i < 8; ++i) {
        uint64_t x = (i >> 0) & 1;
        uint64_t y = (i >> 1) & 1;
        uint64_t z = (i >> 2) & 1;
        sig[i]     = x ^ y ^ z;
    }
    auto simplified = Expr::BitwiseXor(
        Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2)
    );
    auto result = SignatureCheck(sig, *simplified, 3, 1);
    EXPECT_TRUE(result.passed);
}

TEST(SignatureCheckerTest, EvalExprShr) {
    auto shr = Expr::LogicalShr(Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)), 4);
    std::vector< uint64_t > vars = { 0xAB };
    EXPECT_EQ(EvalExpr(*shr, vars, 64), 0xAu);
}

// --- full_width_check tests ---

TEST(FullWidthCheckTest, LinearExprPasses) {
    // x + y is correct on all bitwidths
    auto original   = Expr::Add(Expr::Variable(0), Expr::Variable(1));
    auto simplified = Expr::Add(Expr::Variable(0), Expr::Variable(1));

    auto result = FullWidthCheck(*original, 2, *simplified, {}, 64);
    EXPECT_TRUE(result.passed);
}

TEST(FullWidthCheckTest, PolynomialTargetFails) {
    // original: x * y, simplified: x & y (the wrong CoB result)
    auto original   = Expr::Mul(Expr::Variable(0), Expr::Variable(1));
    auto simplified = Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1));

    auto result = FullWidthCheck(*original, 2, *simplified, {}, 64);
    EXPECT_FALSE(result.passed);
    EXPECT_FALSE(result.failing_input.empty());
}

TEST(FullWidthCheckTest, CancelledPolynomialPasses) {
    // x*y - x*y + x = x (polynomial terms cancel)
    auto original = Expr::Add(
        Expr::Add(
            Expr::Mul(Expr::Variable(0), Expr::Variable(1)),
            Expr::Negate(Expr::Mul(Expr::Variable(0), Expr::Variable(1)))
        ),
        Expr::Variable(0)
    );
    auto simplified = Expr::Variable(0);

    auto result = FullWidthCheck(*original, 2, *simplified, {}, 64);
    EXPECT_TRUE(result.passed);
}

TEST(FullWidthCheckTest, WithVarMapAfterElimination) {
    // original has 3 vars [a, x, y], simplified uses 2 vars [x, y]
    // original: 0*a + x + y (a is spurious)
    // After elimination, simplified: var0 + var1 (mapped to x=1, y=2)
    auto original = Expr::Add(
        Expr::Add(Expr::Mul(Expr::Constant(0), Expr::Variable(0)), Expr::Variable(1)),
        Expr::Variable(2)
    );
    auto simplified = Expr::Add(Expr::Variable(0), Expr::Variable(1));

    // var_map: simplified var 0 → original var 1, simplified var 1 → original var 2
    std::vector< uint32_t > var_map = { 1, 2 };
    auto result                     = FullWidthCheck(*original, 3, *simplified, var_map, 64);
    EXPECT_TRUE(result.passed);
}

TEST(FullWidthCheckTest, Bitwidth8) {
    // x + y at 8-bit
    auto original   = Expr::Add(Expr::Variable(0), Expr::Variable(1));
    auto simplified = Expr::Add(Expr::Variable(0), Expr::Variable(1));

    auto result = FullWidthCheck(*original, 2, *simplified, {}, 8);
    EXPECT_TRUE(result.passed);
}

TEST(FullWidthCheckTest, AlgebraicIdentityNegX) {
    // x*y + x*~y = -x
    auto original = Expr::Add(
        Expr::Mul(Expr::Variable(0), Expr::Variable(1)),
        Expr::Mul(Expr::Variable(0), Expr::BitwiseNot(Expr::Variable(1)))
    );
    auto simplified = Expr::Negate(Expr::Variable(0));

    // After aux elimination, y is spurious → var_map: [0] (x stays at 0)
    // But original still has 2 vars
    std::vector< uint32_t > var_map = { 0 };
    auto result                     = FullWidthCheck(*original, 2, *simplified, var_map, 64);
    EXPECT_TRUE(result.passed);
}
