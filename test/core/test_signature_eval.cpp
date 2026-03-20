#include "cobra/core/Expr.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/Simplifier.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(SignatureEvalTest, XPlusY) {
    auto e   = Expr::Add(Expr::Variable(0), Expr::Variable(1));
    auto sig = EvaluateBooleanSignature(*e, 2, 64);
    ASSERT_EQ(sig.size(), 4u);
    EXPECT_EQ(sig[0], 0u); // x=0, y=0
    EXPECT_EQ(sig[1], 1u); // x=1, y=0
    EXPECT_EQ(sig[2], 1u); // x=0, y=1
    EXPECT_EQ(sig[3], 2u); // x=1, y=1
}

TEST(SignatureEvalTest, XAndY) {
    auto e   = Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1));
    auto sig = EvaluateBooleanSignature(*e, 2, 64);
    ASSERT_EQ(sig.size(), 4u);
    EXPECT_EQ(sig[0], 0u);
    EXPECT_EQ(sig[1], 0u);
    EXPECT_EQ(sig[2], 0u);
    EXPECT_EQ(sig[3], 1u);
}

TEST(SignatureEvalTest, XTimesY) {
    auto e   = Expr::Mul(Expr::Variable(0), Expr::Variable(1));
    auto sig = EvaluateBooleanSignature(*e, 2, 64);
    ASSERT_EQ(sig.size(), 4u);
    // On {0,1}: x*y == x&y
    EXPECT_EQ(sig[0], 0u);
    EXPECT_EQ(sig[1], 0u);
    EXPECT_EQ(sig[2], 0u);
    EXPECT_EQ(sig[3], 1u);
}

TEST(SignatureEvalTest, XorYTimesZ) {
    // (x ^ y) * z — 3 vars, 8 points
    auto e =
        Expr::Mul(Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2));
    auto sig = EvaluateBooleanSignature(*e, 3, 64);
    ASSERT_EQ(sig.size(), 8u);
    // Manually compute: sig[i] = ((bit0 ^ bit1) * bit2)
    for (uint32_t i = 0; i < 8; ++i) {
        uint64_t x = (i >> 0) & 1;
        uint64_t y = (i >> 1) & 1;
        uint64_t z = (i >> 2) & 1;
        EXPECT_EQ(sig[i], (x ^ y) * z) << "i=" << i;
    }
}

TEST(SignatureEvalTest, Constant42) {
    auto e   = Expr::Constant(42);
    auto sig = EvaluateBooleanSignature(*e, 2, 64);
    ASSERT_EQ(sig.size(), 4u);
    for (auto v : sig) { EXPECT_EQ(v, 42u); }
}

TEST(SignatureEvalTest, VariableOrderLockin) {
    // 2*x + 3*y + 5*z — asymmetric coefficients lock down bit mapping
    auto e = Expr::Add(
        Expr::Add(
            Expr::Mul(Expr::Constant(2), Expr::Variable(0)),
            Expr::Mul(Expr::Constant(3), Expr::Variable(1))
        ),
        Expr::Mul(Expr::Constant(5), Expr::Variable(2))
    );
    auto sig = EvaluateBooleanSignature(*e, 3, 64);
    ASSERT_EQ(sig.size(), 8u);
    for (uint32_t i = 0; i < 8; ++i) {
        uint64_t x = (i >> 0) & 1;
        uint64_t y = (i >> 1) & 1;
        uint64_t z = (i >> 2) & 1;
        EXPECT_EQ(sig[i], 2 * x + 3 * y + 5 * z) << "i=" << i;
    }
}

TEST(SignatureEvalTest, CrossCheckWithEvalExpr) {
    // Cross-check: evaluate_boolean_signature matches manual eval_expr
    auto e =
        Expr::Add(Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2));
    auto sig = EvaluateBooleanSignature(*e, 3, 64);
    ASSERT_EQ(sig.size(), 8u);
    for (uint32_t i = 0; i < 8; ++i) {
        std::vector< uint64_t > v = { (i >> 0) & 1ULL, (i >> 1) & 1ULL, (i >> 2) & 1ULL };
        EXPECT_EQ(sig[i], EvalExpr(*e, v, 64)) << "i=" << i;
    }
}

TEST(SignatureEvalTest, SingleVar) {
    auto e   = Expr::Variable(0);
    auto sig = EvaluateBooleanSignature(*e, 1, 64);
    ASSERT_EQ(sig.size(), 2u);
    EXPECT_EQ(sig[0], 0u);
    EXPECT_EQ(sig[1], 1u);
}

TEST(SignatureEvalTest, Bitwidth8) {
    // 200*x + 100*y at 8-bit
    auto e = Expr::Add(
        Expr::Mul(Expr::Constant(200), Expr::Variable(0)),
        Expr::Mul(Expr::Constant(100), Expr::Variable(1))
    );
    auto sig = EvaluateBooleanSignature(*e, 2, 8);
    ASSERT_EQ(sig.size(), 4u);
    EXPECT_EQ(sig[0], 0u);
    EXPECT_EQ(sig[1], 200u);
    EXPECT_EQ(sig[2], 100u);
    EXPECT_EQ(sig[3], (200u + 100u) & 0xFF);
}

TEST(SignatureEvalTest, EvaluatorOverload_XorFunction) {
    // f(x0, x1) = x0 ^ x1
    cobra::Evaluator eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] ^ v[1];
    };
    auto sig = cobra::EvaluateBooleanSignature(eval, 2, 64);
    // sig[0b00]=0, sig[0b01]=1, sig[0b10]=1, sig[0b11]=0
    ASSERT_EQ(sig.size(), 4u);
    EXPECT_EQ(sig[0], 0u);
    EXPECT_EQ(sig[1], 1u);
    EXPECT_EQ(sig[2], 1u);
    EXPECT_EQ(sig[3], 0u);
}

TEST(SignatureEvalTest, EvaluatorOverload_MatchesExprOverload) {
    // f(x0, x1) = x0 & x1
    auto expr     = cobra::Expr::BitwiseAnd(cobra::Expr::Variable(0), cobra::Expr::Variable(1));
    auto sig_expr = cobra::EvaluateBooleanSignature(*expr, 2, 64);

    cobra::Evaluator eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] & v[1];
    };
    auto sig_eval = cobra::EvaluateBooleanSignature(eval, 2, 64);

    ASSERT_EQ(sig_expr.size(), sig_eval.size());
    for (size_t i = 0; i < sig_expr.size(); ++i) {
        EXPECT_EQ(sig_expr[i], sig_eval[i]) << "mismatch at index " << i;
    }
}

TEST(SignatureEvalTest, EvaluatorOverload_BitwidthMasking) {
    // f(x0) = ~x0 (should mask to 8-bit)
    cobra::Evaluator eval = [](const std::vector< uint64_t > &v) -> uint64_t { return ~v[0]; };
    auto sig              = cobra::EvaluateBooleanSignature(eval, 1, 8);
    ASSERT_EQ(sig.size(), 2u);
    EXPECT_EQ(sig[0], 0xFFu); // ~0 & 0xFF
    EXPECT_EQ(sig[1], 0xFEu); // ~1 & 0xFF
}
