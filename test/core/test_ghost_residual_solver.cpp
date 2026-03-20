#include "cobra/core/GhostResidualSolver.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(IsBooleanNullResidualTest, TrueForMulSubAnd) {
    // r(x0, x1) = x0*x1 - (x0 & x1) — zero on {0,1}, nonzero elsewhere
    Evaluator eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return (v[0] * v[1]) - (v[0] & v[1]);
    };
    std::vector< uint32_t > support = { 0, 1 };
    auto sig                        = EvaluateBooleanSignature(eval, 2, 64);
    EXPECT_TRUE(IsBooleanNullResidual(eval, support, 2, 64, sig));
}

TEST(IsBooleanNullResidualTest, FalseForOrdinaryBitwise) {
    // r(x0, x1) = x0 ^ x1 — nonzero on boolean inputs
    Evaluator eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] ^ v[1]; };
    std::vector< uint32_t > support = { 0, 1 };
    auto sig                        = EvaluateBooleanSignature(eval, 2, 64);
    EXPECT_FALSE(IsBooleanNullResidual(eval, support, 2, 64, sig));
}

TEST(IsBooleanNullResidualTest, FalseForIdenticallyZero) {
    // r(x0, x1) = 0 — zero everywhere, not just on booleans
    Evaluator eval = [](const std::vector< uint64_t > & /*v*/) -> uint64_t { return 0; };
    std::vector< uint32_t > support = { 0, 1 };
    auto sig                        = EvaluateBooleanSignature(eval, 2, 64);
    EXPECT_FALSE(IsBooleanNullResidual(eval, support, 2, 64, sig));
}

TEST(IsBooleanNullResidualTest, LateIndexVarsClassifiedCorrectly) {
    // r(x3, x5) = x3*x5 - (x3 & x5) — live vars at indices 3, 5
    // Must still be classified as boolean-null despite not being vars 0,1
    Evaluator eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return (v[3] * v[5]) - (v[3] & v[5]);
    };
    std::vector< uint32_t > support = { 3, 5 };
    auto sig                        = EvaluateBooleanSignature(eval, 6, 64);
    EXPECT_TRUE(IsBooleanNullResidual(eval, support, 6, 64, sig));
}

TEST(SolveGhostResidualTest, SolvesMulSubAnd) {
    Evaluator eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return (v[0] * v[1]) - (v[0] & v[1]);
    };
    std::vector< uint32_t > support = { 0, 1 };
    auto result                     = SolveGhostResidual(eval, support, 2, 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->num_terms, 1);
    auto check = FullWidthCheckEval(eval, 2, *result->expr, 64);
    EXPECT_TRUE(check.passed);
}

TEST(SolveGhostResidualTest, SolvesScaledGhost) {
    Evaluator eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return 5 * ((v[0] * v[1]) - (v[0] & v[1]));
    };
    std::vector< uint32_t > support = { 0, 1 };
    auto result                     = SolveGhostResidual(eval, support, 2, 64);
    ASSERT_TRUE(result.has_value());
    auto check = FullWidthCheckEval(eval, 2, *result->expr, 64);
    EXPECT_TRUE(check.passed);
}

TEST(SolveGhostResidualTest, SolvesNegatedGhost) {
    Evaluator eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return 0 - ((v[0] * v[1]) - (v[0] & v[1]));
    };
    std::vector< uint32_t > support = { 0, 1 };
    auto result                     = SolveGhostResidual(eval, support, 2, 64);
    ASSERT_TRUE(result.has_value());
    auto check = FullWidthCheckEval(eval, 2, *result->expr, 64);
    EXPECT_TRUE(check.passed);
}

TEST(SolveGhostResidualTest, SolvesArity3Ghost) {
    Evaluator eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return (v[0] * v[1] * v[2]) - (v[0] & v[1] & v[2]);
    };
    std::vector< uint32_t > support = { 0, 1, 2 };
    auto result                     = SolveGhostResidual(eval, support, 3, 64);
    ASSERT_TRUE(result.has_value());
    auto check = FullWidthCheckEval(eval, 3, *result->expr, 64);
    EXPECT_TRUE(check.passed);
}

TEST(SolveGhostResidualTest, TwoAdicPrecisionSelection) {
    Evaluator eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return 4 * ((v[0] * v[1]) - (v[0] & v[1]));
    };
    std::vector< uint32_t > support = { 0, 1 };
    auto result                     = SolveGhostResidual(eval, support, 2, 64);
    ASSERT_TRUE(result.has_value());
    auto check = FullWidthCheckEval(eval, 2, *result->expr, 64);
    EXPECT_TRUE(check.passed);
}

TEST(SolveGhostResidualTest, ReturnsNulloptForPolynomialNull) {
    Evaluator eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] * (v[0] - 1);
    };
    std::vector< uint32_t > support = { 0 };
    auto result                     = SolveGhostResidual(eval, support, 1, 64);
    EXPECT_FALSE(result.has_value());
}

TEST(SolveGhostResidualTest, ReturnsNulloptForMultiTermGhost) {
    Evaluator eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return ((v[0] * v[1]) - (v[0] & v[1])) + ((v[1] * v[2]) - (v[1] & v[2]));
    };
    std::vector< uint32_t > support = { 0, 1, 2 };
    auto result                     = SolveGhostResidual(eval, support, 3, 64);
    EXPECT_FALSE(result.has_value());
}

// --- SolveFactoredGhostResidual tests ---

TEST(SolveFactoredGhostResidualTest, SolvesConstantQuotient) {
    // r(x,y) = 5 * (x*y - (x&y))
    Evaluator eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return 5 * ((v[0] * v[1]) - (v[0] & v[1]));
    };
    std::vector< uint32_t > support = { 0, 1 };
    auto result                     = SolveFactoredGhostResidual(eval, support, 2, 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->num_terms, 1);
    auto check = FullWidthCheckEval(eval, 2, *result->expr, 64);
    EXPECT_TRUE(check.passed);
}

TEST(SolveFactoredGhostResidualTest, SolvesWithPrunedSupport) {
    // r(v) = 3 * (v[1]*v[3] - (v[1]&v[3])), support at indices 1,3
    Evaluator eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return 3 * ((v[1] * v[3]) - (v[1] & v[3]));
    };
    std::vector< uint32_t > support = { 1, 3 };
    auto result                     = SolveFactoredGhostResidual(eval, support, 4, 64);
    ASSERT_TRUE(result.has_value());
    auto check = FullWidthCheckEval(eval, 4, *result->expr, 64);
    EXPECT_TRUE(check.passed);
}

TEST(SolveFactoredGhostResidualTest, Solves3VarWithMulSubAnd) {
    // r(x,y,z) = 7 * (x*y - (x&y)) — only uses 2 of 3 support vars
    Evaluator eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return 7 * ((v[0] * v[1]) - (v[0] & v[1]));
    };
    std::vector< uint32_t > support = { 0, 1, 2 };
    auto result                     = SolveFactoredGhostResidual(eval, support, 3, 64);
    ASSERT_TRUE(result.has_value());
    auto check = FullWidthCheckEval(eval, 3, *result->expr, 64);
    EXPECT_TRUE(check.passed);
}

TEST(SolveFactoredGhostResidualTest, NulloptForXor) {
    // r(x,y) = x ^ y — not representable as q * ghost
    Evaluator eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] ^ v[1]; };
    std::vector< uint32_t > support = { 0, 1 };
    auto result                     = SolveFactoredGhostResidual(eval, support, 2, 64);
    EXPECT_FALSE(result.has_value());
}

TEST(SolveFactoredGhostResidualTest, NulloptForSingleVarSupport) {
    // Support has only 1 variable — no 2-var tuple for mul_sub_and
    Evaluator eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] * (v[0] - 1);
    };
    std::vector< uint32_t > support = { 0 };
    auto result                     = SolveFactoredGhostResidual(eval, support, 1, 64);
    EXPECT_FALSE(result.has_value());
}
