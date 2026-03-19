#include "ExprParser.h"
#include "cobra/core/Expr.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(ExprParserTest, SimpleAdd) {
    auto result = ParseAndEvaluate("x + y", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().vars.size(), 2u);
    EXPECT_EQ(result.value().sig.size(), 4u);
    EXPECT_EQ(result.value().sig[0], 0u);
    EXPECT_EQ(result.value().sig[1], 1u);
    EXPECT_EQ(result.value().sig[2], 1u);
    EXPECT_EQ(result.value().sig[3], 2u);
}

TEST(ExprParserTest, XorExpression) {
    auto result = ParseAndEvaluate("x ^ y", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().sig[3], 0u);
}

TEST(ExprParserTest, ComplexMBA) {
    auto result = ParseAndEvaluate("(x ^ y) + 2 * (x & y)", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().sig[0], 0u);
    EXPECT_EQ(result.value().sig[1], 1u);
    EXPECT_EQ(result.value().sig[2], 1u);
    EXPECT_EQ(result.value().sig[3], 2u);
}

TEST(ExprParserTest, WithConstants) {
    auto result = ParseAndEvaluate("5 + x", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().sig[0], 5u);
    EXPECT_EQ(result.value().sig[1], 6u);
}

TEST(ExprParserTest, BitwiseNot) {
    auto result = ParseAndEvaluate("~x", 8);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().sig[0], 255u);
    EXPECT_EQ(result.value().sig[1], 254u);
}

TEST(ExprParserTest, Precedence) {
    auto result = ParseAndEvaluate("x + y * 2", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().sig[0], 0u);
    EXPECT_EQ(result.value().sig[1], 1u);
    EXPECT_EQ(result.value().sig[2], 2u);
    EXPECT_EQ(result.value().sig[3], 3u);
}

TEST(ExprParserTest, UnaryNeg) {
    auto result = ParseAndEvaluate("-x", 8);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().sig[0], 0u);
    EXPECT_EQ(result.value().sig[1], 255u);
}

TEST(ExprParserTest, ParseError) {
    auto result = ParseAndEvaluate("x ++ y", 64);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, CobraError::kParseError);
}

TEST(ExprParserTest, EmptyInput) {
    auto result = ParseAndEvaluate("", 64);
    EXPECT_FALSE(result.has_value());
}

TEST(ExprParserTest, VariableOrdering) {
    auto result = ParseAndEvaluate("x + a + b", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().vars[0], "a");
    EXPECT_EQ(result.value().vars[1], "b");
    EXPECT_EQ(result.value().vars[2], "x");
}

// Unmatched left parenthesis
TEST(ExprParserTest, UnmatchedLeftParen) {
    auto result = ParseAndEvaluate("(x + y", 64);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, CobraError::kParseError);
}

// Unmatched right parenthesis
TEST(ExprParserTest, UnmatchedRightParen) {
    auto result = ParseAndEvaluate("x + y)", 64);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, CobraError::kParseError);
}

// Too many variables guard (>20)
TEST(ExprParserTest, TooManyVariables) {
    // Build expression with 21 unique variables: v0 + v1 + ... + v20
    std::string expr;
    for (int i = 0; i <= 20; ++i) {
        if (i > 0) { expr += " + "; }
        expr += "v" + std::to_string(i);
    }
    auto result = ParseAndEvaluate(expr, 64);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, CobraError::kTooManyVariables);
}

// Nested parentheses
TEST(ExprParserTest, NestedParens) {
    auto result = ParseAndEvaluate("((x + y))", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().sig[0], 0u);
    EXPECT_EQ(result.value().sig[3], 2u);
}

// All operators combined
TEST(ExprParserTest, AllOperators) {
    auto result = ParseAndEvaluate("(x + y) * 2 - (x & y) | (x ^ y)", 8);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().vars.size(), 2u);
    EXPECT_EQ(result.value().sig.size(), 4u);
}

// Subtraction operator
TEST(ExprParserTest, Subtraction) {
    auto result = ParseAndEvaluate("x - y", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().sig[0], 0u);
    EXPECT_EQ(result.value().sig[1], 1u);
    EXPECT_EQ(result.value().sig[2], UINT64_MAX); // 0 - 1 mod 2^64
    EXPECT_EQ(result.value().sig[3], 0u);
}

TEST(ExprParserTest, HexLiteralBasic) {
    auto result = ParseAndEvaluate("x + 0xFF", 64);
    ASSERT_TRUE(result.has_value());
    // x=0: 0+255=255, x=1: 1+255=256
    EXPECT_EQ(result.value().sig[0], 255u);
    EXPECT_EQ(result.value().sig[1], 256u);
}

TEST(ExprParserTest, HexLiteralInBitwise) {
    auto result = ParseAndEvaluate("x & 0xFF", 64);
    ASSERT_TRUE(result.has_value());
    // x=0: 0&255=0, x=1: 1&255=1
    EXPECT_EQ(result.value().sig[0], 0u);
    EXPECT_EQ(result.value().sig[1], 1u);
}

TEST(ExprParserTest, HexLiteralUpperCase) {
    auto result = ParseAndEvaluate("x + 0xABCD", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().sig[0], 0xABCDu);
}

TEST(ExprParserTest, HexLiteralZero) {
    auto result = ParseAndEvaluate("x + 0x0", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().sig[0], 0u);
    EXPECT_EQ(result.value().sig[1], 1u);
}

TEST(ExprParserTest, HexLiteralEmpty) {
    auto result = ParseAndEvaluate("0x + x", 64);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, CobraError::kParseError);
}

TEST(ExprParserTest, HexLiteralOverflow) {
    auto result = ParseAndEvaluate("x + 0x10000000000000000", 64);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, CobraError::kParseError);
}

// --- parse_to_ast tests ---

TEST(ExprParserTest, ParseToAstVariable) {
    auto result = ParseToAst("x", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().expr->kind, Expr::Kind::kVariable);
    EXPECT_EQ(result.value().vars.size(), 1u);
    EXPECT_EQ(result.value().vars[0], "x");
}

TEST(ExprParserTest, ParseToAstBinaryAdd) {
    auto result = ParseToAst("x + y", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().expr->kind, Expr::Kind::kAdd);
    EXPECT_EQ(result.value().vars.size(), 2u);
}

TEST(ExprParserTest, ParseToAstConstantInBitwise) {
    auto result = ParseToAst("x & 0xFF", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().expr->kind, Expr::Kind::kAnd);
    auto &children = result.value().expr->children;
    EXPECT_EQ(children.size(), 2u);
    EXPECT_EQ(children[0]->kind, Expr::Kind::kVariable);
    EXPECT_EQ(children[1]->kind, Expr::Kind::kConstant);
    EXPECT_EQ(children[1]->constant_val, 0xFFu);
}

TEST(ExprParserTest, ParseToAstNested) {
    auto result = ParseToAst("3 * (x ^ 0x55) + y", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().expr->kind, Expr::Kind::kAdd);
}

TEST(ExprParserTest, ParseToAstSubtraction) {
    // Subtraction is Add(L, Neg(R))
    auto result = ParseToAst("x - y", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().expr->kind, Expr::Kind::kAdd);
    EXPECT_EQ(result.value().expr->children[1]->kind, Expr::Kind::kNeg);
}

TEST(ExprParserTest, ParseToAstUnaryNeg) {
    auto result = ParseToAst("-x", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().expr->kind, Expr::Kind::kNeg);
    EXPECT_EQ(result.value().expr->children.size(), 1u);
}

TEST(ExprParserTest, ParseToAstBitwiseNot) {
    auto result = ParseToAst("~x", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().expr->kind, Expr::Kind::kNot);
}

TEST(ExprParserTest, ParseToAstConstantOnly) {
    auto result = ParseToAst("42", 8);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().expr->kind, Expr::Kind::kConstant);
    EXPECT_EQ(result.value().expr->constant_val, 42u);
    EXPECT_EQ(result.value().vars.size(), 0u);
}

TEST(ExprParserTest, ParseToAstConstantMasked) {
    auto result = ParseToAst("0x1FF", 8);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().expr->kind, Expr::Kind::kConstant);
    // 0x1FF = 511, masked to 8 bits = 255
    EXPECT_EQ(result.value().expr->constant_val, 255u);
}

TEST(ExprParserTest, ParseToAstEmptyInput) {
    auto result = ParseToAst("", 64);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, CobraError::kParseError);
}

TEST(ExprParserTest, ParseToAstParseError) {
    auto result = ParseToAst("x ++ y", 64);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, CobraError::kParseError);
}

TEST(ExprParserTest, ParseToAstTooManyVariables) {
    std::string expr;
    for (int i = 0; i <= 20; ++i) {
        if (i > 0) { expr += " + "; }
        expr += "v" + std::to_string(i);
    }
    auto result = ParseToAst(expr, 64);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, CobraError::kTooManyVariables);
}

// --- Shift tests ---

TEST(ExprParserTest, LeftShiftEvaluates) {
    auto result = ParseAndEvaluate("x << 3", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().sig[0], 0u);
    EXPECT_EQ(result.value().sig[1], 8u);
}

TEST(ExprParserTest, RightShiftEvaluates) {
    auto result = ParseAndEvaluate("x >> 2", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().sig[0], 0u);
    EXPECT_EQ(result.value().sig[1], 0u);
}

TEST(ExprParserTest, ShiftIdentity) {
    auto lsh = ParseAndEvaluate("x << 0", 64);
    ASSERT_TRUE(lsh.has_value());
    EXPECT_EQ(lsh.value().sig[0], 0u);
    EXPECT_EQ(lsh.value().sig[1], 1u);

    auto rsh = ParseAndEvaluate("x >> 0", 64);
    ASSERT_TRUE(rsh.has_value());
    EXPECT_EQ(rsh.value().sig[0], 0u);
    EXPECT_EQ(rsh.value().sig[1], 1u);
}

TEST(ExprParserTest, ShiftPrecedenceLooseThanAdd) {
    auto result = ParseAndEvaluate("a + b << 2", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().sig[0], 0u);
    EXPECT_EQ(result.value().sig[1], 4u);
    EXPECT_EQ(result.value().sig[2], 4u);
    EXPECT_EQ(result.value().sig[3], 8u);
}

TEST(ExprParserTest, ShiftPrecedenceTighterThanAnd) {
    auto result = ParseAndEvaluate("a << 2 & 3", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().sig[0], 0u);
    EXPECT_EQ(result.value().sig[1], 0u);
}

TEST(ExprParserTest, ShiftLeftAssociative) {
    auto result = ParseAndEvaluate("a << 2 << 3", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().sig[0], 0u);
    EXPECT_EQ(result.value().sig[1], 32u);
}

TEST(ExprParserTest, MixedShiftNesting) {
    auto result = ParseAndEvaluate("(x << 3) >> 2", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().sig[0], 0u);
    EXPECT_EQ(result.value().sig[1], 2u);
}

TEST(ExprParserTest, RejectVariableShiftAmount) {
    auto result = ParseAndEvaluate("x << y", 64);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, CobraError::kParseError);
}

TEST(ExprParserTest, RejectVariableRightShiftAmount) {
    auto result = ParseAndEvaluate("x >> y", 64);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, CobraError::kParseError);
}

TEST(ExprParserTest, RejectShiftOutOfRange64) {
    auto result = ParseAndEvaluate("x << 64", 64);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, CobraError::kParseError);
}

TEST(ExprParserTest, RejectRightShiftOutOfRange64) {
    auto result = ParseAndEvaluate("x >> 64", 64);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, CobraError::kParseError);
}

TEST(ExprParserTest, RejectShiftOutOfRange65) {
    auto result = ParseAndEvaluate("x << 65", 64);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, CobraError::kParseError);
}

TEST(ExprParserTest, RejectShiftOutOfRange8Bit) {
    auto result = ParseAndEvaluate("x << 8", 8);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, CobraError::kParseError);
}

TEST(ExprParserTest, ParseToAstLeftShift) {
    auto result = ParseToAst("x << 3", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().expr->kind, Expr::Kind::kMul);
    EXPECT_EQ(result.value().expr->children[1]->kind, Expr::Kind::kConstant);
    EXPECT_EQ(result.value().expr->children[1]->constant_val, 8u);
}

TEST(ExprParserTest, ParseToAstRightShift) {
    auto result = ParseToAst("x >> 2", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().expr->kind, Expr::Kind::kShr);
    EXPECT_EQ(result.value().expr->constant_val, 2u);
}

TEST(ExprParserTest, ParseToAstRejectVariableShift) {
    auto result = ParseToAst("x << y", 64);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, CobraError::kParseError);
}

TEST(ExprParserTest, RightShiftBoundary63) {
    auto result = ParseAndEvaluate("x >> 63", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().sig[0], 0u);
    EXPECT_EQ(result.value().sig[1], 0u);
}

TEST(ExprParserTest, RightShiftSmallBitwidth) {
    auto result = ParseAndEvaluate("0xFF >> 4", 8);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().sig[0], 0xFu);
}

TEST(ExprParserTest, ParseToAstVariableIndex) {
    // Variables sorted lexicographically: a=0, b=1, x=2
    auto result = ParseToAst("a + b + x", 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().vars[0], "a");
    EXPECT_EQ(result.value().vars[1], "b");
    EXPECT_EQ(result.value().vars[2], "x");
    // Root is Add(Add(a, b), x). Walk to verify var_index values.
    // Left child of root Add is Add(a, b)
    auto &root = result.value().expr;
    ASSERT_EQ(root->kind, Expr::Kind::kAdd);
    auto &left_add = root->children[0];
    ASSERT_EQ(left_add->kind, Expr::Kind::kAdd);
    // left_add->children[0] is 'a' (var_index 0)
    ASSERT_EQ(left_add->children[0]->kind, Expr::Kind::kVariable);
    EXPECT_EQ(left_add->children[0]->var_index, 0u);
    // left_add->children[1] is 'b' (var_index 1)
    ASSERT_EQ(left_add->children[1]->kind, Expr::Kind::kVariable);
    EXPECT_EQ(left_add->children[1]->var_index, 1u);
    // root->children[1] is 'x' (var_index 2)
    ASSERT_EQ(root->children[1]->kind, Expr::Kind::kVariable);
    EXPECT_EQ(root->children[1]->var_index, 2u);
}
