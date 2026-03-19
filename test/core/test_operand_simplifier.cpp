#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/OperandSimplifier.h"
#include "cobra/core/SignatureChecker.h"
#include <gtest/gtest.h>
#include <random>

using namespace cobra;

static bool semantically_equal(
    const Expr &a, const Expr &b, uint32_t num_vars, uint32_t bitwidth, int samples = 16
) {
    std::mt19937_64 rng(42);
    uint64_t mask = (bitwidth >= 64) ? UINT64_MAX : ((1ULL << bitwidth) - 1);
    for (int s = 0; s < samples; ++s) {
        std::vector< uint64_t > v(num_vars);
        for (uint32_t i = 0; i < num_vars; ++i) { v[i] = rng() & mask; }
        if (EvalExpr(a, v, bitwidth) != EvalExpr(b, v, bitwidth)) { return false; }
    }
    return true;
}

// --- Identity / no-change tests ---

TEST(OperandSimplifierTest, PlainVariableMulUnchanged) {
    // Mul(a, b): no bitwise, should be unchanged
    auto e        = Expr::Mul(Expr::Variable(0), Expr::Variable(1));
    auto original = CloneExpr(*e);

    Options opts;
    opts.bitwidth = 64;
    auto result   = SimplifyMixedOperands(std::move(e), { "a", "b" }, opts);

    EXPECT_FALSE(result.changed);
    EXPECT_TRUE(semantically_equal(*original, *result.expr, 2, 64));
}

TEST(OperandSimplifierTest, ConstTimesExprUnchanged) {
    // Mul(3, a&b): only one side variable-dependent
    auto e =
        Expr::Mul(Expr::Constant(3), Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)));
    auto original = CloneExpr(*e);

    Options opts;
    opts.bitwidth = 64;
    auto result   = SimplifyMixedOperands(std::move(e), { "a", "b" }, opts);

    EXPECT_FALSE(result.changed);
    EXPECT_TRUE(semantically_equal(*original, *result.expr, 2, 64));
}

TEST(OperandSimplifierTest, NoMulNodeUnchanged) {
    // a & b: no Mul node at all
    auto e        = Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1));
    auto original = CloneExpr(*e);

    Options opts;
    opts.bitwidth = 64;
    auto result   = SimplifyMixedOperands(std::move(e), { "a", "b" }, opts);

    EXPECT_FALSE(result.changed);
    EXPECT_TRUE(semantically_equal(*original, *result.expr, 2, 64));
}

// --- Single operand simplification ---

TEST(OperandSimplifierTest, LeftOperandMBACollapses) {
    // Mul((a&b)|(a&~b), y) = Mul(a, y)
    // Left operand (a&b)|(a&~b) == a
    auto lhs = Expr::BitwiseOr(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)),
        Expr::BitwiseAnd(Expr::Variable(0), Expr::BitwiseNot(Expr::Variable(1)))
    );
    auto e        = Expr::Mul(std::move(lhs), Expr::Variable(2));
    auto original = CloneExpr(*e);

    Options opts;
    opts.bitwidth = 64;
    auto result   = SimplifyMixedOperands(std::move(e), { "a", "b", "y" }, opts);

    EXPECT_TRUE(result.changed);
    EXPECT_TRUE(semantically_equal(*original, *result.expr, 3, 64));
    // Should be cheaper
    auto orig_cost = ComputeCost(*original).cost;
    auto new_cost  = ComputeCost(*result.expr).cost;
    EXPECT_TRUE(IsBetter(new_cost, orig_cost));
}

// --- Rebuild cleanup helpers ---

TEST(OperandSimplifierTest, MulByZeroFolds) {
    // Mul(0, a&b) -> 0
    auto e =
        Expr::Mul(Expr::Constant(0), Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)));

    Options opts;
    opts.bitwidth = 64;
    auto result   = SimplifyMixedOperands(std::move(e), { "a", "b" }, opts);

    EXPECT_EQ(result.expr->kind, Expr::Kind::kConstant);
    EXPECT_EQ(result.expr->constant_val, 0u);
}

TEST(OperandSimplifierTest, MulByOneFolds) {
    // Mul(1, a&b) -> a&b
    auto e =
        Expr::Mul(Expr::Constant(1), Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)));
    auto inner = Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1));

    Options opts;
    opts.bitwidth = 64;
    auto result   = SimplifyMixedOperands(std::move(e), { "a", "b" }, opts);

    EXPECT_TRUE(semantically_equal(*inner, *result.expr, 2, 64));
}

TEST(OperandSimplifierTest, AddZeroFolds) {
    // Add(0, x) -> x
    auto e = Expr::Add(Expr::Constant(0), Expr::Variable(0));

    Options opts;
    opts.bitwidth = 64;
    auto result   = SimplifyMixedOperands(std::move(e), { "x" }, opts);

    EXPECT_EQ(result.expr->kind, Expr::Kind::kVariable);
}

TEST(OperandSimplifierTest, DoubleNegFolds) {
    // Neg(Neg(x)) -> x
    auto e = Expr::Negate(Expr::Negate(Expr::Variable(0)));

    Options opts;
    opts.bitwidth = 64;
    auto result   = SimplifyMixedOperands(std::move(e), { "x" }, opts);

    EXPECT_EQ(result.expr->kind, Expr::Kind::kVariable);
}

TEST(OperandSimplifierTest, DoubleNotFolds) {
    // Not(Not(x)) -> x
    auto e = Expr::BitwiseNot(Expr::BitwiseNot(Expr::Variable(0)));

    Options opts;
    opts.bitwidth = 64;
    auto result   = SimplifyMixedOperands(std::move(e), { "x" }, opts);

    EXPECT_EQ(result.expr->kind, Expr::Kind::kVariable);
}

TEST(OperandSimplifierTest, AndWithAllOnesFolds) {
    // And(x, 0xFF) with bitwidth=8 -> x
    auto e = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF));

    Options opts;
    opts.bitwidth = 8;
    auto result   = SimplifyMixedOperands(std::move(e), { "x" }, opts);

    EXPECT_EQ(result.expr->kind, Expr::Kind::kVariable);
}

TEST(OperandSimplifierTest, OrWithZeroFolds) {
    // Or(x, 0) -> x
    auto e = Expr::BitwiseOr(Expr::Variable(0), Expr::Constant(0));

    Options opts;
    opts.bitwidth = 64;
    auto result   = SimplifyMixedOperands(std::move(e), { "x" }, opts);

    EXPECT_EQ(result.expr->kind, Expr::Kind::kVariable);
}

TEST(OperandSimplifierTest, XorWithZeroFolds) {
    // Xor(0, x) -> x
    auto e = Expr::BitwiseXor(Expr::Constant(0), Expr::Variable(0));

    Options opts;
    opts.bitwidth = 64;
    auto result   = SimplifyMixedOperands(std::move(e), { "x" }, opts);

    EXPECT_EQ(result.expr->kind, Expr::Kind::kVariable);
}

// --- Both operands simplify ---

TEST(OperandSimplifierTest, BothOperandsMBACollapse) {
    // Mul((a&b)|(a&~b), (a|b)+(a&b))
    //   = Mul(a, a+b)
    // Left:  (a&b)|(a&~b) == a
    // Right: (a|b)+(a&b)  == a+b
    auto lhs = Expr::BitwiseOr(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)),
        Expr::BitwiseAnd(Expr::Variable(0), Expr::BitwiseNot(Expr::Variable(1)))
    );
    auto rhs = Expr::Add(
        Expr::BitwiseOr(Expr::Variable(0), Expr::Variable(1)),
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1))
    );
    auto e        = Expr::Mul(std::move(lhs), std::move(rhs));
    auto original = CloneExpr(*e);

    Options opts;
    opts.bitwidth = 64;
    auto result   = SimplifyMixedOperands(std::move(e), { "a", "b" }, opts);

    EXPECT_TRUE(result.changed);
    EXPECT_TRUE(semantically_equal(*original, *result.expr, 2, 64));
    auto orig_cost = ComputeCost(*original).cost;
    auto new_cost  = ComputeCost(*result.expr).cost;
    EXPECT_TRUE(IsBetter(new_cost, orig_cost));
}

// --- Semantic preservation ---

TEST(OperandSimplifierTest, ResultAlwaysSemanticEqual) {
    // Complex mixed expression: ((a|b) + (a&b)) * (a^b)
    // Left operand (a|b)+(a&b) is MBA, should simplify
    auto lhs = Expr::Add(
        Expr::BitwiseOr(Expr::Variable(0), Expr::Variable(1)),
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1))
    );
    auto rhs      = Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1));
    auto e        = Expr::Mul(std::move(lhs), std::move(rhs));
    auto original = CloneExpr(*e);

    Options opts;
    opts.bitwidth = 64;
    auto result   = SimplifyMixedOperands(std::move(e), { "a", "b" }, opts);

    EXPECT_TRUE(semantically_equal(*original, *result.expr, 2, 64));
}
