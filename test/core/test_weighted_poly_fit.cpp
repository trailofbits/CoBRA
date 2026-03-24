#include "cobra/core/GhostBasis.h"
#include "cobra/core/PolyExprBuilder.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/WeightedPolyFit.h"
#include <gtest/gtest.h>

using namespace cobra;

// --- Ghost-family weight tests (primary) ---
// Each test builds Mul(q_expr, g_expr) and verifies semantic
// equivalence via FullWidthCheckEval against the target evaluator.

TEST(WeightedPolyFitTest, ConstantQuotientMulSubAnd) {
    // target(x,y) = 5 * (x*y - (x&y))
    // quotient q = 5 (constant), weight = mul_sub_and
    const auto &basis = GetGhostBasis();
    WeightFn weight   = basis[0].eval; // mul_sub_and
    Evaluator target  = [](const std::vector< uint64_t > &v) -> uint64_t {
        return 5 * ((v[0] * v[1]) - (v[0] & v[1]));
    };
    std::vector< uint32_t > support = { 0, 1 };
    auto result                     = RecoverWeightedPoly(target, weight, support, 2, 64, 0, 2);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.Payload().poly.IsValid());
    EXPECT_EQ(result.Payload().degree_used, 0);
    auto q_expr = BuildPolyExpr(result.Payload().poly);
    ASSERT_TRUE(q_expr.has_value());
    auto g_expr   = basis[0].build(support);
    auto combined = Expr::Mul(std::move(*q_expr), std::move(g_expr));
    EXPECT_TRUE(FullWidthCheckEval(target, 2, *combined, 64).passed);
}

TEST(WeightedPolyFitTest, AffineQuotientMulSubAnd) {
    // target(x,y) = (3*x + 7) * (x*y - (x&y))
    const auto &basis = GetGhostBasis();
    WeightFn weight   = basis[0].eval;
    Evaluator target  = [](const std::vector< uint64_t > &v) -> uint64_t {
        return (3 * v[0] + 7) * ((v[0] * v[1]) - (v[0] & v[1]));
    };
    std::vector< uint32_t > support = { 0, 1 };
    auto result                     = RecoverWeightedPoly(target, weight, support, 2, 64, 1, 2);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.Payload().poly.IsValid());
    EXPECT_LE(result.Payload().degree_used, 1);
    auto q_expr = BuildPolyExpr(result.Payload().poly);
    ASSERT_TRUE(q_expr.has_value());
    auto g_expr   = basis[0].build(support);
    auto combined = Expr::Mul(std::move(*q_expr), std::move(g_expr));
    EXPECT_TRUE(FullWidthCheckEval(target, 2, *combined, 64).passed);
}

TEST(WeightedPolyFitTest, QuadraticQuotientMulSubAnd) {
    // target(x,y) = (2*x*x + x + 1) * (x*y - (x&y))
    const auto &basis = GetGhostBasis();
    WeightFn weight   = basis[0].eval;
    Evaluator target  = [](const std::vector< uint64_t > &v) -> uint64_t {
        return (2 * v[0] * v[0] + v[0] + 1) * ((v[0] * v[1]) - (v[0] & v[1]));
    };
    std::vector< uint32_t > support = { 0, 1 };
    // grid_degree=3: C(4,2)=6 unknowns need >5 non-zero-weight rows;
    // grid_degree=2 only provides 5 (9 total minus 4 boolean-zeroed).
    auto result                     = RecoverWeightedPoly(target, weight, support, 2, 64, 2, 3);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.Payload().poly.IsValid());
    EXPECT_LE(result.Payload().degree_used, 2);
    auto q_expr = BuildPolyExpr(result.Payload().poly);
    ASSERT_TRUE(q_expr.has_value());
    auto g_expr   = basis[0].build(support);
    auto combined = Expr::Mul(std::move(*q_expr), std::move(g_expr));
    EXPECT_TRUE(FullWidthCheckEval(target, 2, *combined, 64).passed);
}

TEST(WeightedPolyFitTest, Arity3GhostMul3SubAnd3) {
    // target(x,y,z) = (x + 1) * (x*y*z - (x&y&z))
    const auto &basis = GetGhostBasis();
    WeightFn weight   = basis[1].eval; // mul3_sub_and3
    Evaluator target  = [](const std::vector< uint64_t > &v) -> uint64_t {
        return (v[0] + 1) * ((v[0] * v[1] * v[2]) - (v[0] & v[1] & v[2]));
    };
    std::vector< uint32_t > support = { 0, 1, 2 };
    auto result                     = RecoverWeightedPoly(target, weight, support, 3, 64, 1, 2);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.Payload().poly.IsValid());
    auto q_expr = BuildPolyExpr(result.Payload().poly);
    ASSERT_TRUE(q_expr.has_value());
    auto g_expr   = basis[1].build(support);
    auto combined = Expr::Mul(std::move(*q_expr), std::move(g_expr));
    EXPECT_TRUE(FullWidthCheckEval(target, 3, *combined, 64).passed);
}

TEST(WeightedPolyFitTest, MultiVarQuotientMulSubAnd) {
    // target(x,y) = (x + 2*y) * (x*y - (x&y))
    // quotient involves both support vars
    const auto &basis = GetGhostBasis();
    WeightFn weight   = basis[0].eval;
    Evaluator target  = [](const std::vector< uint64_t > &v) -> uint64_t {
        return (v[0] + 2 * v[1]) * ((v[0] * v[1]) - (v[0] & v[1]));
    };
    std::vector< uint32_t > support = { 0, 1 };
    auto result                     = RecoverWeightedPoly(target, weight, support, 2, 64, 1, 2);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.Payload().poly.IsValid());
    auto q_expr = BuildPolyExpr(result.Payload().poly);
    ASSERT_TRUE(q_expr.has_value());
    auto g_expr   = basis[0].build(support);
    auto combined = Expr::Mul(std::move(*q_expr), std::move(g_expr));
    EXPECT_TRUE(FullWidthCheckEval(target, 2, *combined, 64).passed);
}

// --- Supplementary generic tests ---

TEST(WeightedPolyFitTest, OddWeightCleanInvertibility) {
    // weight(x) = 2*x + 1 (always odd on the grid)
    // target(x) = (3*x + 5) * (2*x + 1)
    // No GhostBuilder for this weight — build w_expr manually
    WeightFn weight = [](std::span< const uint64_t > args, uint32_t /*bw*/) -> uint64_t {
        return (2 * args[0]) + 1;
    };
    Evaluator target = [](const std::vector< uint64_t > &v) -> uint64_t {
        return ((3 * v[0]) + 5) * ((2 * v[0]) + 1);
    };
    std::vector< uint32_t > support = { 0 };
    auto result                     = RecoverWeightedPoly(target, weight, support, 1, 64, 1, 2);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.Payload().poly.IsValid());
    auto q_expr = BuildPolyExpr(result.Payload().poly);
    ASSERT_TRUE(q_expr.has_value());
    // w_expr = 2*x + 1
    auto w_expr = Expr::Add(Expr::Mul(Expr::Constant(2), Expr::Variable(0)), Expr::Constant(1));
    auto combined = Expr::Mul(std::move(*q_expr), std::move(w_expr));
    EXPECT_TRUE(FullWidthCheckEval(target, 1, *combined, 64).passed);
}

TEST(WeightedPolyFitTest, NulloptQuotientExceedsMaxDegree) {
    // target(x,y) = x * (x*y - (x&y))
    // True quotient is x (affine), but we cap at max_degree=0.
    // The 3 nonzero-weight grid points have varying quotient values
    // (x=1 and x=2), so no constant fits the overdetermined system.
    const auto &basis = GetGhostBasis();
    WeightFn weight   = basis[0].eval;
    Evaluator target  = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] * ((v[0] * v[1]) - (v[0] & v[1]));
    };
    std::vector< uint32_t > support = { 0, 1 };
    auto result                     = RecoverWeightedPoly(target, weight, support, 2, 64, 0, 2);
    EXPECT_FALSE(result.Succeeded());
}

TEST(WeightedPolyFitTest, NulloptIncompatibleTarget) {
    // target(x,y) = x ^ y — not representable as q * w for any q
    const auto &basis = GetGhostBasis();
    WeightFn weight   = basis[0].eval;
    Evaluator target = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] ^ v[1]; };
    std::vector< uint32_t > support = { 0, 1 };
    auto result                     = RecoverWeightedPoly(target, weight, support, 2, 64, 2, 2);
    EXPECT_FALSE(result.Succeeded());
}

TEST(WeightedPolyFitTest, NulloptEmptySupport) {
    WeightFn weight  = [](std::span< const uint64_t >, uint32_t) -> uint64_t { return 1; };
    Evaluator target = [](const std::vector< uint64_t > &) -> uint64_t { return 42; };
    auto result      = RecoverWeightedPoly(target, weight, {}, 0, 64, 0, 2);
    EXPECT_FALSE(result.Succeeded());
}

TEST(WeightedPolyFitTest, PrunedSupportRemapsVariables) {
    // Support at indices 1,3 in a 4-var space
    // target(v) = (v[1] + 1) * (v[1]*v[3] - (v[1]&v[3]))
    const auto &basis = GetGhostBasis();
    WeightFn weight   = basis[0].eval;
    Evaluator target  = [](const std::vector< uint64_t > &v) -> uint64_t {
        return (v[1] + 1) * ((v[1] * v[3]) - (v[1] & v[3]));
    };
    std::vector< uint32_t > support = { 1, 3 };
    auto result                     = RecoverWeightedPoly(target, weight, support, 4, 64, 1, 2);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.Payload().poly.IsValid());
    EXPECT_EQ(result.Payload().poly.num_vars, 4);
    auto q_expr = BuildPolyExpr(result.Payload().poly);
    ASSERT_TRUE(q_expr.has_value());
    auto g_expr   = basis[0].build(support);
    auto combined = Expr::Mul(std::move(*q_expr), std::move(g_expr));
    EXPECT_TRUE(FullWidthCheckEval(target, 4, *combined, 64).passed);
}

TEST(WeightedPolyFitTest, NarrowBitwidth8Bit) {
    uint32_t bw       = 8;
    uint64_t mask     = (1ULL << bw) - 1;
    const auto &basis = GetGhostBasis();
    WeightFn weight   = basis[0].eval;
    Evaluator target  = [mask](const std::vector< uint64_t > &v) -> uint64_t {
        return (3 * ((v[0] * v[1]) - (v[0] & v[1]))) & mask;
    };
    std::vector< uint32_t > support = { 0, 1 };
    auto result                     = RecoverWeightedPoly(target, weight, support, 2, bw, 0, 2);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.Payload().poly.IsValid());
    auto q_expr = BuildPolyExpr(result.Payload().poly);
    ASSERT_TRUE(q_expr.has_value());
    auto g_expr   = basis[0].build(support);
    auto combined = Expr::Mul(std::move(*q_expr), std::move(g_expr));
    EXPECT_TRUE(FullWidthCheckEval(target, 2, *combined, bw).passed);
}
