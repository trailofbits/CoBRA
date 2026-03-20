#include "cobra/core/MonomialKey.h"
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
    auto key                   = MonomialKey::FromExponents(exps, 2);
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
    auto key21                   = MonomialKey::FromExponents(exps21, 2);
    auto it21                    = result->coeffs.find(key21);
    ASSERT_NE(it21, result->coeffs.end());
    EXPECT_EQ(it21->second, 1u);

    uint8_t exps11[kMaxPolyVars] = {};
    exps11[0]                    = 1;
    exps11[1]                    = 1;
    auto key11                   = MonomialKey::FromExponents(exps11, 2);
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
    auto key0                   = MonomialKey::FromExponents(exps0, 1);
    auto it0                    = result->coeffs.find(key0);
    ASSERT_NE(it0, result->coeffs.end());
    EXPECT_EQ(it0->second, 3u);

    uint8_t exps2[kMaxPolyVars] = {};
    exps2[0]                    = 2;
    auto key2                   = MonomialKey::FromExponents(exps2, 1);
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
    auto key                   = MonomialKey::FromExponents(exps, 5);
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

// --- Degree 3/4 tests (BuildPolyExpr generalization is Task 6) ---

TEST(MultivarPolyRecoveryTest, Degree3_XCubed) {
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] * v[0] * v[0]; };
    auto result = RecoverMultivarPoly(eval, { 0 }, 1, 64, 3);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->IsValid());

    // x^3 = x_(3) + 3*x_(2) + x_(1) in factorial basis
    EXPECT_EQ(result->coeffs.size(), 3u);

    uint8_t e1[kMaxPolyVars] = {};
    e1[0]                    = 1;
    auto it1                 = result->coeffs.find(MonomialKey::FromExponents(e1, 1));
    ASSERT_NE(it1, result->coeffs.end());
    EXPECT_EQ(it1->second, 1u);

    uint8_t e2[kMaxPolyVars] = {};
    e2[0]                    = 2;
    auto it2                 = result->coeffs.find(MonomialKey::FromExponents(e2, 1));
    ASSERT_NE(it2, result->coeffs.end());
    EXPECT_EQ(it2->second, 3u);

    uint8_t e3[kMaxPolyVars] = {};
    e3[0]                    = 3;
    auto it3                 = result->coeffs.find(MonomialKey::FromExponents(e3, 1));
    ASSERT_NE(it3, result->coeffs.end());
    EXPECT_EQ(it3->second, 1u);
}

TEST(MultivarPolyRecoveryTest, Degree3_XCubedPlusY) {
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] * v[0] * v[0] + v[1];
    };
    auto result = RecoverMultivarPoly(eval, { 0, 1 }, 2, 64, 3);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->IsValid());

    // x^3 + y = x_(3) + 3*x_(2) + x_(1) + y in factorial basis
    EXPECT_EQ(result->coeffs.size(), 4u);

    uint8_t ey[kMaxPolyVars] = {};
    ey[1]                    = 1;
    auto ity                 = result->coeffs.find(MonomialKey::FromExponents(ey, 2));
    ASSERT_NE(ity, result->coeffs.end());
    EXPECT_EQ(ity->second, 1u);
}

TEST(MultivarPolyRecoveryTest, Degree3_WrongAtDegree2) {
    // x^3 at max_degree=2: divisibility gate passes but the recovered
    // polynomial is 3*x_(2) + x_(1) = 3x^2 - 2x, not x^3.
    // Recovery succeeds but has no degree-3 term.
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] * v[0] * v[0]; };
    auto result = RecoverMultivarPoly(eval, { 0 }, 1, 64, 2);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->IsValid());

    // No degree-3 term exists in the result
    uint8_t e3[kMaxPolyVars] = {};
    e3[0]                    = 3;
    auto it3                 = result->coeffs.find(MonomialKey::FromExponents(e3, 1));
    EXPECT_EQ(it3, result->coeffs.end());

    // But degree-2 and degree-1 terms are present
    EXPECT_EQ(result->coeffs.size(), 2u);
}

TEST(MultivarPolyRecoveryTest, Degree4_XSquaredYSquared) {
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] * v[0] * v[1] * v[1];
    };
    // Per-variable degree is 2, so degree-4 grid also works
    auto result = RecoverMultivarPoly(eval, { 0, 1 }, 2, 64, 4);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->IsValid());

    // Same coefficients as degree-2 recovery
    auto result2 = RecoverMultivarPoly(eval, { 0, 1 }, 2, 64, 2);
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(result->coeffs, result2->coeffs);
}

TEST(MultivarPolyRecoveryTest, Degree4_XToTheFourth) {
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        uint64_t x = v[0];
        return x * x * x * x;
    };
    auto result = RecoverMultivarPoly(eval, { 0 }, 1, 64, 4);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->IsValid());

    // x^4 = x_(4) + 7*x_(3) + 6*x_(2) + x_(1)
    EXPECT_EQ(result->coeffs.size(), 4u);

    uint8_t e1[kMaxPolyVars] = {};
    e1[0]                    = 1;
    auto it1                 = result->coeffs.find(MonomialKey::FromExponents(e1, 1));
    ASSERT_NE(it1, result->coeffs.end());
    EXPECT_EQ(it1->second, 1u);

    uint8_t e2[kMaxPolyVars] = {};
    e2[0]                    = 2;
    auto it2                 = result->coeffs.find(MonomialKey::FromExponents(e2, 1));
    ASSERT_NE(it2, result->coeffs.end());
    EXPECT_EQ(it2->second, 7u);

    uint8_t e3[kMaxPolyVars] = {};
    e3[0]                    = 3;
    auto it3                 = result->coeffs.find(MonomialKey::FromExponents(e3, 1));
    ASSERT_NE(it3, result->coeffs.end());
    EXPECT_EQ(it3->second, 6u);

    uint8_t e4[kMaxPolyVars] = {};
    e4[0]                    = 4;
    auto it4                 = result->coeffs.find(MonomialKey::FromExponents(e4, 1));
    ASSERT_NE(it4, result->coeffs.end());
    EXPECT_EQ(it4->second, 1u);
}

TEST(MultivarPolyRecoveryTest, Degree3_DivisibilityGate) {
    // x*(x-1)*(x-2)/6 fails divisibility gate at degree 3
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        uint64_t x = v[0];
        return (x * (x - 1) * (x - 2)) / 6;
    };
    auto result = RecoverMultivarPoly(eval, { 0 }, 1, 64, 3);
    EXPECT_FALSE(result.has_value());
}

TEST(MultivarPolyRecoveryTest, DefaultDegreeIsTwo) {
    // Calling without max_degree should behave like degree 2.
    // x^3 at degree 2: recovery succeeds but produces wrong polynomial
    // (same as explicit max_degree=2).
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] * v[0] * v[0]; };
    auto result_default = RecoverMultivarPoly(eval, { 0 }, 1, 64);
    auto result_deg2    = RecoverMultivarPoly(eval, { 0 }, 1, 64, 2);
    ASSERT_TRUE(result_default.has_value());
    ASSERT_TRUE(result_deg2.has_value());
    EXPECT_EQ(result_default->coeffs, result_deg2->coeffs);
}

// --- RecoverAndVerifyPoly tests ---

TEST(RecoverAndVerifyPolyTest, Degree2_Success) {
    auto eval   = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] * v[0]; };
    auto result = RecoverAndVerifyPoly(eval, { 0 }, 1, 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->degree_used, 2);

    // Verify the expression
    for (uint64_t x = 0; x < 10; ++x) {
        std::vector< uint64_t > v = { x };
        EXPECT_EQ(EvalExpr(*result->expr, v, 64), x * x);
    }
}

TEST(RecoverAndVerifyPolyTest, Degree3_Escalation) {
    // x^3 fails at degree 2, succeeds at degree 3
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] * v[0] * v[0]; };
    auto result = RecoverAndVerifyPoly(eval, { 0 }, 1, 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->degree_used, 3);
}

TEST(RecoverAndVerifyPolyTest, Degree4_Escalation) {
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        uint64_t x = v[0];
        return x * x * x * x;
    };
    auto result = RecoverAndVerifyPoly(eval, { 0 }, 1, 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->degree_used, 4);
}

TEST(RecoverAndVerifyPolyTest, FailClosed_Bitwise) {
    // x & y is not a polynomial at any degree — should return nullopt
    auto eval   = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] & v[1]; };
    auto result = RecoverAndVerifyPoly(eval, { 0, 1 }, 2, 64, 4);
    EXPECT_FALSE(result.has_value());
}

TEST(RecoverAndVerifyPolyTest, MinDegreeAboveMaxCap_ReturnsNullopt) {
    // min_degree > max_degree_cap should immediately return nullopt.
    // RecoverMultivarPoly at max_degree=d recovers polynomials of degree ≤ d,
    // so a degree-2 poly IS recoverable at d=3. The floor controls which
    // degrees the escalation loop *tries*, not the degree of the result.
    auto eval   = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] * v[1]; };
    // max_degree_cap=2, min_degree=3 → loop body never executes
    auto result = RecoverAndVerifyPoly(eval, { 0, 1 }, 2, 64, 2, 3);
    EXPECT_FALSE(result.has_value());
}

TEST(RecoverAndVerifyPolyTest, MinDegree3_FindsCubic) {
    // f(x0, x1) = x0 * x0 * x1 (degree 3 polynomial)
    // With min_degree=3, should find it at degree 3.
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] * v[0] * v[1]; };
    auto result = RecoverAndVerifyPoly(eval, { 0, 1 }, 2, 64, 4, 3);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->degree_used, 3);
}

TEST(RecoverAndVerifyPolyTest, CapBelowTwo_ReturnsNullopt) {
    auto eval   = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0]; };
    auto result = RecoverAndVerifyPoly(eval, { 0 }, 1, 64, 1);
    EXPECT_FALSE(result.has_value());
}

TEST(RecoverAndVerifyPolyTest, CappedAtTwo_CubicFails) {
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] * v[0] * v[0]; };
    auto result = RecoverAndVerifyPoly(eval, { 0 }, 1, 64, 2);
    EXPECT_FALSE(result.has_value());
}

// --- Pipeline-level regression tests ---

TEST(RecoverAndVerifyPolyTest, GhostFunction_XY_MinusXAndY) {
    // f(x,y) = x*y - (x&y). Zero on {0,1}^2, nonzero at full width.
    // This is a "ghost" residual. As a polynomial, x*y is degree 2.
    // x&y also equals x*y on {0,1}^2 but differs at full width.
    // f = x*y - x*y_bitwise. The polynomial part is 0.
    // This should fail at all degrees (it's irreducibly bitwise).
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] * v[1] - (v[0] & v[1]);
    };
    auto result = RecoverAndVerifyPoly(eval, { 0, 1 }, 2, 64, 4);
    EXPECT_FALSE(result.has_value());
}

TEST(RecoverAndVerifyPolyTest, VerifyFailThenSuccess) {
    // f(x) = x^3 + x. At degree 2: recovers x (from {0,1,2} grid),
    // but verify-fail because x^3 contributes at full width.
    // At degree 3: recovers x^3 + x, verify-pass.
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] * v[0] * v[0] + v[0];
    };
    auto result = RecoverAndVerifyPoly(eval, { 0 }, 1, 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_GE(result->degree_used, 3);

    // Verify correctness
    for (uint64_t x = 0; x < 20; ++x) {
        std::vector< uint64_t > v = { x };
        EXPECT_EQ(EvalExpr(*result->expr, v, 64), x * x * x + x);
    }
}
