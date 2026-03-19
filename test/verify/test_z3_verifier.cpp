#include "cobra/core/Expr.h"
#include "cobra/verify/Z3Verifier.h"
#include <gtest/gtest.h>

using namespace cobra;

// x + y: CoB coeffs [0, 1, 1, 0] should match Expr(x + y)
TEST(Z3VerifierTest, AffineXPlusYEquivalent) {
    std::vector< uint64_t > cob_coeffs = { 0, 1, 1, 0 };
    auto expr                          = Expr::Add(Expr::Variable(0), Expr::Variable(1));
    std::vector< std::string > vars    = { "x", "y" };

    auto result = Z3Verify(cob_coeffs, *expr, vars, 2, 64);
    EXPECT_TRUE(result.equivalent);
    EXPECT_TRUE(result.counterexample.empty());
}

// Constant: CoB coeffs [42] should match Expr(42)
TEST(Z3VerifierTest, ConstantEquivalent) {
    std::vector< uint64_t > cob_coeffs = { 42 };
    auto expr                          = Expr::Constant(42);
    std::vector< std::string > vars    = {};

    auto result = Z3Verify(cob_coeffs, *expr, vars, 0, 64);
    EXPECT_TRUE(result.equivalent);
}

// Mismatch: CoB coeffs [0, 1, 1, 0] vs Expr(x - y)
TEST(Z3VerifierTest, NonEquivalentDetected) {
    std::vector< uint64_t > cob_coeffs = { 0, 1, 1, 0 };
    // x - y = x + (-y)
    auto expr = Expr::Add(Expr::Variable(0), Expr::Negate(Expr::Variable(1)));
    std::vector< std::string > vars = { "x", "y" };

    auto result = Z3Verify(cob_coeffs, *expr, vars, 2, 64);
    EXPECT_FALSE(result.equivalent);
    EXPECT_FALSE(result.counterexample.empty());
}

// 8-bit: 3*x + 5*y with wrapping
TEST(Z3VerifierTest, Bitwidth8Equivalent) {
    // CoB coeffs for 3*x + 5*y: constant=0, x-coeff=3, y-coeff=5, x&y=0
    std::vector< uint64_t > cob_coeffs = { 0, 3, 5, 0 };
    auto expr                          = Expr::Add(
        Expr::Mul(Expr::Constant(3), Expr::Variable(0)),
        Expr::Mul(Expr::Constant(5), Expr::Variable(1))
    );
    std::vector< std::string > vars = { "x", "y" };

    auto result = Z3Verify(cob_coeffs, *expr, vars, 2, 8);
    EXPECT_TRUE(result.equivalent);
}

// Single variable: identity x
TEST(Z3VerifierTest, SingleVarIdentity) {
    std::vector< uint64_t > cob_coeffs = { 0, 1 };
    auto expr                          = Expr::Variable(0);
    std::vector< std::string > vars    = { "x" };

    auto result = Z3Verify(cob_coeffs, *expr, vars, 1, 64);
    EXPECT_TRUE(result.equivalent);
}

// Three variables: x + 2*y + 3*z
TEST(Z3VerifierTest, ThreeVarAffine) {
    // CoB indices: 0=const, 1=x, 2=y, 3=x&y, 4=z, 5=x&z, 6=y&z, 7=x&y&z
    std::vector< uint64_t > cob_coeffs = { 0, 1, 2, 0, 3, 0, 0, 0 };
    auto expr                          = Expr::Add(
        Expr::Add(Expr::Variable(0), Expr::Mul(Expr::Constant(2), Expr::Variable(1))),
        Expr::Mul(Expr::Constant(3), Expr::Variable(2))
    );
    std::vector< std::string > vars = { "x", "y", "z" };

    auto result = Z3Verify(cob_coeffs, *expr, vars, 3, 64);
    EXPECT_TRUE(result.equivalent);
}

// Non-affine: x & y (tests higher-order CoB term reconstruction)
TEST(Z3VerifierTest, NonAffineAndEquivalent) {
    // CoB of x & y: [0, 0, 0, 1]
    std::vector< uint64_t > cob_coeffs = { 0, 0, 0, 1 };
    auto expr                          = Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1));
    std::vector< std::string > vars    = { "x", "y" };

    auto result = Z3Verify(cob_coeffs, *expr, vars, 2, 64);
    EXPECT_TRUE(result.equivalent);
}

// Non-affine: x | y = x + y - (x & y)
TEST(Z3VerifierTest, NonAffineOrEquivalent) {
    // CoB of x | y: [0, 1, 1, -1 mod 2^64]
    std::vector< uint64_t > cob_coeffs = { 0, 1, 1, UINT64_MAX };
    auto expr                          = Expr::BitwiseOr(Expr::Variable(0), Expr::Variable(1));
    std::vector< std::string > vars    = { "x", "y" };

    auto result = Z3Verify(cob_coeffs, *expr, vars, 2, 64);
    EXPECT_TRUE(result.equivalent);
}

// Non-affine: x ^ y = x + y - 2*(x & y)
TEST(Z3VerifierTest, NonAffineXorEquivalent) {
    // CoB of x ^ y: [0, 1, 1, -2 mod 2^64]
    std::vector< uint64_t > cob_coeffs = { 0, 1, 1, UINT64_MAX - 1 };
    auto expr                          = Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1));
    std::vector< std::string > vars    = { "x", "y" };

    auto result = Z3Verify(cob_coeffs, *expr, vars, 2, 64);
    EXPECT_TRUE(result.equivalent);
}

// Non-affine mismatch: CoB of x & y vs x | y
TEST(Z3VerifierTest, NonAffineNonEquivalent) {
    std::vector< uint64_t > cob_coeffs = { 0, 0, 0, 1 }; // x & y
    auto expr                          = Expr::BitwiseOr(Expr::Variable(0), Expr::Variable(1));
    std::vector< std::string > vars    = { "x", "y" };

    auto result = Z3Verify(cob_coeffs, *expr, vars, 2, 64);
    EXPECT_FALSE(result.equivalent);
}

// 1-bit bitwidth
TEST(Z3VerifierTest, Bitwidth1Equivalent) {
    // At 1-bit: x + y mod 2 = x ^ y, CoB coeffs [0, 1, 1, 0]
    std::vector< uint64_t > cob_coeffs = { 0, 1, 1, 0 };
    auto expr                          = Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1));
    std::vector< std::string > vars    = { "x", "y" };

    auto result = Z3Verify(cob_coeffs, *expr, vars, 2, 1);
    EXPECT_TRUE(result.equivalent);
}

// Expr-to-Expr: identical expressions are equivalent
TEST(Z3VerifierTest, ExprEquivalenceIdentical) {
    auto e1     = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF));
    auto e2     = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF));
    auto result = Z3VerifyExprs(*e1, *e2, { "x" }, 64);
    EXPECT_TRUE(result.equivalent);
}

// Expr-to-Expr: x & 0xFF != x on general 64-bit inputs
TEST(Z3VerifierTest, ExprNonEquivalence) {
    auto e1     = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF));
    auto e2     = Expr::Variable(0);
    auto result = Z3VerifyExprs(*e1, *e2, { "x" }, 64);
    EXPECT_FALSE(result.equivalent);
}

// Four variables
TEST(Z3VerifierTest, FourVarAffine) {
    // x + 2*y + 3*z + 4*w
    std::vector< uint64_t > cob_coeffs(16, 0);
    cob_coeffs[1] = 1; // x
    cob_coeffs[2] = 2; // y
    cob_coeffs[4] = 3; // z
    cob_coeffs[8] = 4; // w
    auto expr     = Expr::Add(
        Expr::Add(
            Expr::Add(Expr::Variable(0), Expr::Mul(Expr::Constant(2), Expr::Variable(1))),
            Expr::Mul(Expr::Constant(3), Expr::Variable(2))
        ),
        Expr::Mul(Expr::Constant(4), Expr::Variable(3))
    );
    std::vector< std::string > vars = { "x", "y", "z", "w" };

    auto result = Z3Verify(cob_coeffs, *expr, vars, 4, 64);
    EXPECT_TRUE(result.equivalent);
}
