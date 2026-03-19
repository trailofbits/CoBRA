#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(ExprTest, RenderConstant) {
    auto e = Expr::Constant(42);
    EXPECT_EQ(Render(*e, {}), "42");
}

TEST(ExprTest, RenderVariable) {
    auto e = Expr::Variable(0);
    EXPECT_EQ(Render(*e, { "x" }), "x");
}

TEST(ExprTest, RenderAdd) {
    auto e = Expr::Add(Expr::Variable(0), Expr::Variable(1));
    EXPECT_EQ(Render(*e, { "x", "y" }), "x + y");
}

TEST(ExprTest, RenderMul) {
    auto e = Expr::Mul(Expr::Constant(3), Expr::Variable(0));
    EXPECT_EQ(Render(*e, { "x" }), "3 * x");
}

TEST(ExprTest, RenderComplex) {
    auto e = Expr::Add(
        Expr::Add(Expr::Constant(5), Expr::Mul(Expr::Constant(3), Expr::Variable(0))),
        Expr::Mul(Expr::Constant(7), Expr::Variable(1))
    );
    EXPECT_EQ(Render(*e, { "x", "y" }), "5 + 3 * x + 7 * y");
}

TEST(ExprTest, RenderNot) {
    auto e = Expr::BitwiseNot(Expr::Variable(0));
    EXPECT_EQ(Render(*e, { "x" }), "~x");
}

TEST(ExprTest, RenderZeroConstant) {
    auto e = Expr::Constant(0);
    EXPECT_EQ(Render(*e, {}), "0");
}

TEST(ExprTest, RenderNeg) {
    auto e = Expr::Negate(Expr::Variable(0));
    EXPECT_EQ(Render(*e, { "x" }), "-x");
}

TEST(ExprTest, RenderAnd) {
    auto e = Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1));
    EXPECT_EQ(Render(*e, { "x", "y" }), "x & y");
}

TEST(ExprTest, RenderOr) {
    auto e = Expr::BitwiseOr(Expr::Variable(0), Expr::Variable(1));
    EXPECT_EQ(Render(*e, { "x", "y" }), "x | y");
}

TEST(ExprTest, RenderXor) {
    auto e = Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1));
    EXPECT_EQ(Render(*e, { "x", "y" }), "x ^ y");
}

TEST(ExprTest, RenderMixedPrecedenceParens) {
    auto e = Expr::Mul(Expr::Add(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2));
    EXPECT_EQ(Render(*e, { "x", "y", "z" }), "(x + y) * z");
}

TEST(ExprTest, RenderNegativeConstant64) {
    auto e = Expr::Constant(UINT64_MAX);
    EXPECT_EQ(Render(*e, {}, 64), "-1");
}

TEST(ExprTest, RenderNegativeConstant8) {
    auto e = Expr::Constant(255);
    EXPECT_EQ(Render(*e, {}, 8), "-1");
}

TEST(ExprTest, RenderPositiveConstant8) {
    auto e = Expr::Constant(127);
    EXPECT_EQ(Render(*e, {}, 8), "127");
}

TEST(ExprTest, LogicalShrConstruction) {
    auto shr = Expr::LogicalShr(Expr::Variable(0), 3);
    EXPECT_EQ(shr->kind, Expr::Kind::kShr);
    EXPECT_EQ(shr->constant_val, 3u);
    EXPECT_EQ(shr->children.size(), 1u);
    EXPECT_EQ(shr->children[0]->kind, Expr::Kind::kVariable);
}

TEST(ExprTest, LogicalShrRender) {
    auto shr  = Expr::LogicalShr(Expr::Variable(0), 3);
    auto text = Render(*shr, { "x" }, 64);
    EXPECT_EQ(text, "x >> 3");
}

TEST(ExprTest, LogicalShrClone) {
    auto shr = Expr::LogicalShr(Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)), 4);
    auto cloned = CloneExpr(*shr);
    EXPECT_EQ(cloned->kind, Expr::Kind::kShr);
    EXPECT_EQ(cloned->constant_val, 4u);
    EXPECT_EQ(cloned->children[0]->kind, Expr::Kind::kAnd);
}

TEST(ExprTest, EvalConstantShr) {
    auto shr = Expr::LogicalShr(Expr::Constant(0xFF), 4);
    EXPECT_EQ(EvalConstantExpr(*shr, 64), 0xFu);
}

TEST(ExprTest, EvalConstantShr8bit) {
    auto shr = Expr::LogicalShr(Expr::Constant(0xFF), 4);
    EXPECT_EQ(EvalConstantExpr(*shr, 8), 0xFu);
}
