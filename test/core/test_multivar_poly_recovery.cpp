#include "cobra/core/MultivarPolyRecovery.h"
#include "cobra/core/PolyExprBuilder.h"
#include "cobra/core/SignatureChecker.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(MultivarPolyRecoveryTest, EmptySupport_ReturnsNullopt) {
    auto eval   = [](const std::vector< uint64_t > &) -> uint64_t { return 0; };
    auto result = RecoverMultivarPoly(eval, {}, 2, 64);
    EXPECT_FALSE(result.has_value());
}

TEST(MultivarPolyRecoveryTest, IndexOutOfRange_ReturnsNullopt) {
    auto eval   = [](const std::vector< uint64_t > &) -> uint64_t { return 0; };
    auto result = RecoverMultivarPoly(eval, { 0, 5 }, 3, 64);
    EXPECT_FALSE(result.has_value());
}

TEST(MultivarPolyRecoveryTest, TotalVarsExceedsMax_ReturnsNullopt) {
    auto eval   = [](const std::vector< uint64_t > &) -> uint64_t { return 0; };
    auto result = RecoverMultivarPoly(eval, { 0, 1 }, kMaxPolyVars + 1, 64);
    EXPECT_FALSE(result.has_value());
}

TEST(MultivarPolyRecoveryTest, ForwardDiff_XTimesY) {
    auto eval   = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] * v[1]; };
    auto result = RecoverMultivarPoly(eval, { 0, 1 }, 2, 64);
    ASSERT_TRUE(result.has_value());
    uint8_t exps[kMaxPolyVars] = {};
    exps[0]                    = 1;
    exps[1]                    = 1;
    auto key                   = ExponentTuple::FromExponents(exps, 2);
    auto it                    = result->coeffs.find(key);
    ASSERT_NE(it, result->coeffs.end());
    EXPECT_EQ(it->second, 1u);
    EXPECT_EQ(result->coeffs.size(), 1u);
}

TEST(MultivarPolyRecoveryTest, QuadraticCrossTerm_ASquaredTimesD) {
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] * v[0] * v[1]; };
    auto result = RecoverMultivarPoly(eval, { 0, 1 }, 2, 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->IsValid());

    uint8_t exps21[kMaxPolyVars] = {};
    exps21[0]                    = 2;
    exps21[1]                    = 1;
    auto key21                   = ExponentTuple::FromExponents(exps21, 2);
    auto it21                    = result->coeffs.find(key21);
    ASSERT_NE(it21, result->coeffs.end());
    EXPECT_EQ(it21->second, 1u);

    uint8_t exps11[kMaxPolyVars] = {};
    exps11[0]                    = 1;
    exps11[1]                    = 1;
    auto key11                   = ExponentTuple::FromExponents(exps11, 2);
    auto it11                    = result->coeffs.find(key11);
    ASSERT_NE(it11, result->coeffs.end());
    EXPECT_EQ(it11->second, 1u);
}

TEST(MultivarPolyRecoveryTest, DivisibilityGate_Rejects) {
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        uint64_t x = v[0];
        return (x * (x - 1)) / 2;
    };
    auto result = RecoverMultivarPoly(eval, { 0 }, 1, 64);
    EXPECT_FALSE(result.has_value());
}

TEST(MultivarPolyRecoveryTest, NonzeroConstant_ASquaredPlusThree) {
    auto eval   = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] * v[0] + 3; };
    auto result = RecoverMultivarPoly(eval, { 0 }, 1, 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->IsValid());

    uint8_t exps0[kMaxPolyVars] = {};
    auto key0                   = ExponentTuple::FromExponents(exps0, 1);
    auto it0                    = result->coeffs.find(key0);
    ASSERT_NE(it0, result->coeffs.end());
    EXPECT_EQ(it0->second, 3u);

    uint8_t exps2[kMaxPolyVars] = {};
    exps2[0]                    = 2;
    auto key2                   = ExponentTuple::FromExponents(exps2, 1);
    auto it2                    = result->coeffs.find(key2);
    ASSERT_NE(it2, result->coeffs.end());
    EXPECT_EQ(it2->second, 1u);
}

TEST(MultivarPolyRecoveryTest, PureLinear_APlusD) {
    auto eval   = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] + v[1]; };
    auto result = RecoverMultivarPoly(eval, { 0, 1 }, 2, 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->IsValid());
    EXPECT_EQ(result->coeffs.size(), 2u);
}

TEST(MultivarPolyRecoveryTest, DoubleQuadratic_ASquaredDSquared) {
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] * v[0] * v[1] * v[1];
    };
    auto result = RecoverMultivarPoly(eval, { 0, 1 }, 2, 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->IsValid());

    auto expr_result = BuildPolyExpr(*result);
    ASSERT_TRUE(expr_result.has_value());

    auto check = FullWidthCheckEval(eval, 2, *expr_result.value(), 64);
    EXPECT_TRUE(check.passed);
}

TEST(MultivarPolyRecoveryTest, NarrowWidth_Bitwidth8) {
    uint32_t w    = 8;
    uint64_t mask = (1ULL << w) - 1;
    auto eval     = [mask](const std::vector< uint64_t > &v) -> uint64_t {
        return (v[0] * v[0] * v[1]) & mask;
    };
    auto result = RecoverMultivarPoly(eval, { 0, 1 }, 2, w);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->IsValid());
    EXPECT_EQ(result->bitwidth, w);

    auto expr_result = BuildPolyExpr(*result);
    ASSERT_TRUE(expr_result.has_value());

    auto check = FullWidthCheckEval(eval, 2, *expr_result.value(), w);
    EXPECT_TRUE(check.passed);
}

TEST(MultivarPolyRecoveryTest, PrunedSupport_RemapsVariables) {
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[1] * v[1] * v[4]; };
    auto result = RecoverMultivarPoly(eval, { 1, 4 }, 5, 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->IsValid());
    EXPECT_EQ(result->num_vars, 5);

    uint8_t exps[kMaxPolyVars] = {};
    exps[1]                    = 2;
    exps[4]                    = 1;
    auto key                   = ExponentTuple::FromExponents(exps, 5);
    auto it                    = result->coeffs.find(key);
    ASSERT_NE(it, result->coeffs.end());
    EXPECT_EQ(it->second, 1u);

    auto expr_result = BuildPolyExpr(*result);
    ASSERT_TRUE(expr_result.has_value());

    auto check = FullWidthCheckEval(eval, 5, *expr_result.value(), 64);
    EXPECT_TRUE(check.passed);
}

TEST(MultivarPolyRecoveryTest, ThreeVar_ASquaredTimesBTimesC) {
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] * v[0] * v[1] * v[2];
    };
    auto result = RecoverMultivarPoly(eval, { 0, 1, 2 }, 3, 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->IsValid());

    auto expr_result = BuildPolyExpr(*result);
    ASSERT_TRUE(expr_result.has_value());

    auto check = FullWidthCheckEval(eval, 3, *expr_result.value(), 64);
    EXPECT_TRUE(check.passed);
}
