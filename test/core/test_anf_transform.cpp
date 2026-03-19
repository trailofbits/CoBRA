#include "cobra/core/AnfTransform.h"
#include "cobra/core/Expr.h"
#include "cobra/core/SignatureChecker.h"
#include <gtest/gtest.h>

using namespace cobra;

// --- ANF transform unit tests ---

TEST(AnfTransformTest, ConstantZero) {
    // f = 0: truth table [0, 0, 0, 0]
    std::vector< uint64_t > sig = { 0, 0, 0, 0 };
    auto anf                    = ComputeAnf(sig, 2);
    for (size_t i = 0; i < anf.Size(); ++i) { EXPECT_EQ(anf[i], 0); }
}

TEST(AnfTransformTest, ConstantOne) {
    // f = 1: truth table [1, 1, 1, 1]
    std::vector< uint64_t > sig = { 1, 1, 1, 1 };
    auto anf                    = ComputeAnf(sig, 2);
    EXPECT_EQ(anf[0], 1); // constant term
    EXPECT_EQ(anf[1], 0);
    EXPECT_EQ(anf[2], 0);
    EXPECT_EQ(anf[3], 0);
}

TEST(AnfTransformTest, SingleVarX) {
    // f(x) = x: truth table [0, 1]
    std::vector< uint64_t > sig = { 0, 1 };
    auto anf                    = ComputeAnf(sig, 1);
    EXPECT_EQ(anf[0], 0); // no constant
    EXPECT_EQ(anf[1], 1); // x
}

TEST(AnfTransformTest, TwoVarAnd) {
    // f(x,y) = x & y: truth table [0, 0, 0, 1]
    std::vector< uint64_t > sig = { 0, 0, 0, 1 };
    auto anf                    = ComputeAnf(sig, 2);
    EXPECT_EQ(anf[0], 0); // no constant
    EXPECT_EQ(anf[1], 0); // no x
    EXPECT_EQ(anf[2], 0); // no y
    EXPECT_EQ(anf[3], 1); // x&y
}

TEST(AnfTransformTest, TwoVarOr) {
    // f(x,y) = x | y: truth table [0, 1, 1, 1]
    // ANF: x ^ y ^ (x&y)
    std::vector< uint64_t > sig = { 0, 1, 1, 1 };
    auto anf                    = ComputeAnf(sig, 2);
    EXPECT_EQ(anf[0], 0); // no constant
    EXPECT_EQ(anf[1], 1); // x
    EXPECT_EQ(anf[2], 1); // y
    EXPECT_EQ(anf[3], 1); // x&y
}

TEST(AnfTransformTest, TwoVarXor) {
    // f(x,y) = x ^ y: truth table [0, 1, 1, 0]
    // ANF: x ^ y
    std::vector< uint64_t > sig = { 0, 1, 1, 0 };
    auto anf                    = ComputeAnf(sig, 2);
    EXPECT_EQ(anf[0], 0);
    EXPECT_EQ(anf[1], 1); // x
    EXPECT_EQ(anf[2], 1); // y
    EXPECT_EQ(anf[3], 0); // no x&y
}

TEST(AnfTransformTest, ThreeVarOr) {
    // f(a,b,c) = a | b | c: truth table [0,1,1,1,1,1,1,1]
    // ANF: a ^ b ^ c ^ (a&b) ^ (a&c) ^ (b&c) ^ (a&b&c)
    std::vector< uint64_t > sig = { 0, 1, 1, 1, 1, 1, 1, 1 };
    auto anf                    = ComputeAnf(sig, 3);
    EXPECT_EQ(anf[0], 0); // no constant
    EXPECT_EQ(anf[1], 1); // a
    EXPECT_EQ(anf[2], 1); // b
    EXPECT_EQ(anf[3], 1); // a&b
    EXPECT_EQ(anf[4], 1); // c
    EXPECT_EQ(anf[5], 1); // a&c
    EXPECT_EQ(anf[6], 1); // b&c
    EXPECT_EQ(anf[7], 1); // a&b&c
}

TEST(AnfTransformTest, ThreeVarAnd) {
    // f(a,b,c) = a & b & c: truth table [0,0,0,0,0,0,0,1]
    std::vector< uint64_t > sig = { 0, 0, 0, 0, 0, 0, 0, 1 };
    auto anf                    = ComputeAnf(sig, 3);
    for (int i = 0; i < 7; ++i) { EXPECT_EQ(anf[i], 0); }
    EXPECT_EQ(anf[7], 1); // only a&b&c
}

TEST(AnfTransformTest, ThreeVarXor) {
    // f(a,b,c) = a ^ b ^ c: truth table [0,1,1,0,1,0,0,1]
    std::vector< uint64_t > sig = { 0, 1, 1, 0, 1, 0, 0, 1 };
    auto anf                    = ComputeAnf(sig, 3);
    EXPECT_EQ(anf[0], 0);
    EXPECT_EQ(anf[1], 1); // a
    EXPECT_EQ(anf[2], 1); // b
    EXPECT_EQ(anf[3], 0);
    EXPECT_EQ(anf[4], 1); // c
    EXPECT_EQ(anf[5], 0);
    EXPECT_EQ(anf[6], 0);
    EXPECT_EQ(anf[7], 0);
}

TEST(AnfTransformTest, ModTwoTruncation) {
    // Signature with values > 1 should be taken mod 2
    std::vector< uint64_t > sig = { 0, 3, 5, 7 };
    auto anf                    = ComputeAnf(sig, 2);
    // mod 2: [0, 1, 1, 1] => x | y => ANF: x ^ y ^ (x&y)
    EXPECT_EQ(anf[0], 0);
    EXPECT_EQ(anf[1], 1);
    EXPECT_EQ(anf[2], 1);
    EXPECT_EQ(anf[3], 1);
}

// --- ANF Expr builder tests ---

TEST(AnfExprBuilderTest, ConstantZero) {
    PackedAnf anf = { 0, 0, 0, 0 };
    auto expr     = BuildAnfExpr(anf, 2);
    EXPECT_EQ(expr->kind, Expr::Kind::kConstant);
    EXPECT_EQ(expr->constant_val, 0u);
}

TEST(AnfExprBuilderTest, ConstantOne) {
    PackedAnf anf = { 1, 0, 0, 0 };
    auto expr     = BuildAnfExpr(anf, 2);
    EXPECT_EQ(expr->kind, Expr::Kind::kConstant);
    EXPECT_EQ(expr->constant_val, 1u);
}

TEST(AnfExprBuilderTest, SingleVariable) {
    // ANF: x (mask 1)
    PackedAnf anf = { 0, 1 };
    auto expr     = BuildAnfExpr(anf, 1);
    EXPECT_EQ(expr->kind, Expr::Kind::kVariable);
    EXPECT_EQ(expr->var_index, 0u);
}

TEST(AnfExprBuilderTest, TwoVarAndOnly) {
    // ANF: x&y (mask 3 only)
    PackedAnf anf = { 0, 0, 0, 1 };
    auto expr     = BuildAnfExpr(anf, 2);
    EXPECT_EQ(expr->kind, Expr::Kind::kAnd);
}

TEST(AnfExprBuilderTest, TwoVarXorOnly) {
    // ANF: x ^ y (masks 1, 2)
    PackedAnf anf = { 0, 1, 1, 0 };
    auto expr     = BuildAnfExpr(anf, 2);
    EXPECT_EQ(expr->kind, Expr::Kind::kXor);
}

TEST(AnfExprBuilderTest, TwoVarOr) {
    // ANF: x ^ y ^ (x&y) — cleanup recognizes as OR
    PackedAnf anf               = { 0, 1, 1, 1 };
    auto expr                   = BuildAnfExpr(anf, 2);
    std::vector< uint64_t > sig = { 0, 1, 1, 1 };
    auto check                  = SignatureCheck(sig, *expr, 2, 1);
    EXPECT_TRUE(check.passed);
}

// --- Semantic verification: build ANF expr, check it matches ---

static bool anf_semantically_correct(
    const std::vector< uint64_t > &sig, uint32_t num_vars, uint32_t bitwidth
) {
    auto anf   = ComputeAnf(sig, num_vars);
    auto expr  = BuildAnfExpr(anf, num_vars);
    auto check = SignatureCheck(sig, *expr, num_vars, bitwidth);
    return check.passed;
}

TEST(AnfSemanticTest, TwoVarOrVerifies) {
    std::vector< uint64_t > sig = { 0, 1, 1, 1 };
    EXPECT_TRUE(anf_semantically_correct(sig, 2, 64));
}

TEST(AnfSemanticTest, TwoVarAndVerifies) {
    std::vector< uint64_t > sig = { 0, 0, 0, 1 };
    EXPECT_TRUE(anf_semantically_correct(sig, 2, 64));
}

TEST(AnfSemanticTest, TwoVarXorVerifies) {
    std::vector< uint64_t > sig = { 0, 1, 1, 0 };
    EXPECT_TRUE(anf_semantically_correct(sig, 2, 64));
}

TEST(AnfSemanticTest, ThreeVarOrVerifies) {
    // a | b | c
    std::vector< uint64_t > sig = { 0, 1, 1, 1, 1, 1, 1, 1 };
    EXPECT_TRUE(anf_semantically_correct(sig, 3, 64));
}

TEST(AnfSemanticTest, ThreeVarAndVerifies) {
    std::vector< uint64_t > sig = { 0, 0, 0, 0, 0, 0, 0, 1 };
    EXPECT_TRUE(anf_semantically_correct(sig, 3, 64));
}

TEST(AnfSemanticTest, ThreeVarXorVerifies) {
    std::vector< uint64_t > sig = { 0, 1, 1, 0, 1, 0, 0, 1 };
    EXPECT_TRUE(anf_semantically_correct(sig, 3, 64));
}

TEST(AnfSemanticTest, ThreeVarMajorityVerifies) {
    // majority(a,b,c) = (a&b)|(a&c)|(b&c)
    // truth table: [0, 0, 0, 1, 0, 1, 1, 1]
    std::vector< uint64_t > sig = { 0, 0, 0, 1, 0, 1, 1, 1 };
    EXPECT_TRUE(anf_semantically_correct(sig, 3, 64));
}

TEST(AnfSemanticTest, ConstantZeroVerifies) {
    std::vector< uint64_t > sig = { 0, 0, 0, 0 };
    EXPECT_TRUE(anf_semantically_correct(sig, 2, 64));
}

TEST(AnfSemanticTest, ConstantOneVerifies) {
    std::vector< uint64_t > sig = { 1, 1, 1, 1 };
    EXPECT_TRUE(anf_semantically_correct(sig, 2, 64));
}

TEST(AnfSemanticTest, FourVarOrVerifies) {
    // a | b | c | d: all 1 except sig[0]=0
    std::vector< uint64_t > sig(16, 1);
    sig[0] = 0;
    EXPECT_TRUE(anf_semantically_correct(sig, 4, 64));
}

TEST(AnfSemanticTest, Bitwidth16Verifies) {
    // a | b | c at 16-bit
    std::vector< uint64_t > sig = { 0, 1, 1, 1, 1, 1, 1, 1 };
    EXPECT_TRUE(anf_semantically_correct(sig, 3, 16));
}

TEST(AnfSemanticTest, AllBooleanFunctions2Var) {
    // Exhaustively test all 16 Boolean functions on 2 variables
    for (uint32_t f = 0; f < 16; ++f) {
        std::vector< uint64_t > sig(4);
        for (uint32_t i = 0; i < 4; ++i) { sig[i] = (f >> i) & 1; }
        EXPECT_TRUE(anf_semantically_correct(sig, 2, 64)) << "Failed for function " << f;
    }
}
