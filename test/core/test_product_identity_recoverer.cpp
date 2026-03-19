#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ProductIdentityRecoverer.h"
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

// Helper: build the MBA product identity encoding of x*y
//   (x&y)*(x|y) + (x&~y)*(~x&y)
static std::unique_ptr< Expr >
build_mba_product(std::unique_ptr< Expr > x, std::unique_ptr< Expr > y) {
    auto I = Expr::BitwiseAnd(CloneExpr(*x), CloneExpr(*y));
    auto O = Expr::BitwiseOr(CloneExpr(*x), CloneExpr(*y));
    auto L = Expr::BitwiseAnd(CloneExpr(*x), Expr::BitwiseNot(CloneExpr(*y)));
    auto R = Expr::BitwiseAnd(Expr::BitwiseNot(CloneExpr(*x)), CloneExpr(*y));

    return Expr::Add(
        Expr::Mul(std::move(I), std::move(O)), Expr::Mul(std::move(L), std::move(R))
    );
}

// --- Basic collapse ---

TEST(ProductIdentityRecovererTest, BasicTwoVarCollapse) {
    // (a&b)*(a|b) + (a&~b)*(~a&b) → a*b
    auto mba      = build_mba_product(Expr::Variable(0), Expr::Variable(1));
    auto original = CloneExpr(*mba);
    auto expected = Expr::Mul(Expr::Variable(0), Expr::Variable(1));

    Options opts;
    opts.bitwidth = 64;
    auto result   = CollapseProductIdentities(std::move(mba), { "a", "b" }, opts);

    EXPECT_TRUE(result.changed);
    EXPECT_TRUE(semantically_equal(*original, *result.expr, 2, 64));
    EXPECT_TRUE(semantically_equal(*expected, *result.expr, 2, 64));
    EXPECT_TRUE(IsBetter(ComputeCost(*result.expr).cost, ComputeCost(*original).cost));
}

TEST(ProductIdentityRecovererTest, SwappedProductOrder) {
    // (a&~b)*(~a&b) + (a&b)*(a|b) → a*b
    auto x = Expr::Variable(0);
    auto y = Expr::Variable(1);

    auto L = Expr::BitwiseAnd(CloneExpr(*x), Expr::BitwiseNot(CloneExpr(*y)));
    auto R = Expr::BitwiseAnd(Expr::BitwiseNot(CloneExpr(*x)), CloneExpr(*y));
    auto I = Expr::BitwiseAnd(CloneExpr(*x), CloneExpr(*y));
    auto O = Expr::BitwiseOr(std::move(x), std::move(y));

    auto mba =
        Expr::Add(Expr::Mul(std::move(L), std::move(R)), Expr::Mul(std::move(I), std::move(O)));
    auto original = CloneExpr(*mba);

    Options opts;
    opts.bitwidth = 64;
    auto result   = CollapseProductIdentities(std::move(mba), { "a", "b" }, opts);

    EXPECT_TRUE(result.changed);
    EXPECT_TRUE(semantically_equal(*original, *result.expr, 2, 64));
}

TEST(ProductIdentityRecovererTest, SwappedFactorsInMul) {
    // (a|b)*(a&b) + (~a&b)*(a&~b) → a*b
    auto x = Expr::Variable(0);
    auto y = Expr::Variable(1);

    auto mba = Expr::Add(
        Expr::Mul(
            Expr::BitwiseOr(CloneExpr(*x), CloneExpr(*y)),
            Expr::BitwiseAnd(CloneExpr(*x), CloneExpr(*y))
        ),
        Expr::Mul(
            Expr::BitwiseAnd(Expr::BitwiseNot(CloneExpr(*x)), CloneExpr(*y)),
            Expr::BitwiseAnd(CloneExpr(*x), Expr::BitwiseNot(CloneExpr(*y)))
        )
    );
    auto original = CloneExpr(*mba);

    Options opts;
    opts.bitwidth = 64;
    auto result   = CollapseProductIdentities(std::move(mba), { "a", "b" }, opts);

    EXPECT_TRUE(result.changed);
    EXPECT_TRUE(semantically_equal(*original, *result.expr, 2, 64));
}

// --- Non-matching expressions ---

TEST(ProductIdentityRecovererTest, NonMatchingUnchanged) {
    // a*b + c*d — no MBA product identity
    auto e = Expr::Add(
        Expr::Mul(Expr::Variable(0), Expr::Variable(1)),
        Expr::Mul(Expr::Variable(2), Expr::Variable(3))
    );
    auto original = CloneExpr(*e);

    Options opts;
    opts.bitwidth = 64;
    auto result   = CollapseProductIdentities(std::move(e), { "a", "b", "c", "d" }, opts);

    EXPECT_FALSE(result.changed);
    EXPECT_TRUE(semantically_equal(*original, *result.expr, 4, 64));
}

TEST(ProductIdentityRecovererTest, NoAddMulMulStructure) {
    // Pure bitwise: a & b — no matching structure
    auto e        = Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1));
    auto original = CloneExpr(*e);

    Options opts;
    opts.bitwidth = 64;
    auto result   = CollapseProductIdentities(std::move(e), { "a", "b" }, opts);

    EXPECT_FALSE(result.changed);
    EXPECT_TRUE(semantically_equal(*original, *result.expr, 2, 64));
}

TEST(ProductIdentityRecovererTest, SingleMulUnchanged) {
    // Mul(a, b) — not wrapped in Add
    auto e        = Expr::Mul(Expr::Variable(0), Expr::Variable(1));
    auto original = CloneExpr(*e);

    Options opts;
    opts.bitwidth = 64;
    auto result   = CollapseProductIdentities(std::move(e), { "a", "b" }, opts);

    EXPECT_FALSE(result.changed);
}

// --- Three-variable case ---

TEST(ProductIdentityRecovererTest, ThreeVarCollapse) {
    // MBA-encode a*c (ignoring variable b)
    auto mba      = build_mba_product(Expr::Variable(0), Expr::Variable(2));
    auto original = CloneExpr(*mba);

    Options opts;
    opts.bitwidth = 64;
    auto result   = CollapseProductIdentities(std::move(mba), { "a", "b", "c" }, opts);

    EXPECT_TRUE(result.changed);
    EXPECT_TRUE(semantically_equal(*original, *result.expr, 3, 64));
}

// --- Nested identity ---

TEST(ProductIdentityRecovererTest, NestedCollapse) {
    // MBA(a, b) * c — inner Add(Mul,Mul) collapses,
    // leaving Mul(Mul(a,b), c) = (a*b)*c
    auto inner    = build_mba_product(Expr::Variable(0), Expr::Variable(1));
    auto outer    = Expr::Mul(std::move(inner), Expr::Variable(2));
    auto original = CloneExpr(*outer);

    Options opts;
    opts.bitwidth = 64;
    auto result   = CollapseProductIdentities(std::move(outer), { "a", "b", "c" }, opts);

    EXPECT_TRUE(result.changed);
    EXPECT_TRUE(semantically_equal(*original, *result.expr, 3, 64));
}

// --- Bitwidth variants ---

TEST(ProductIdentityRecovererTest, Bitwidth8Collapse) {
    auto mba      = build_mba_product(Expr::Variable(0), Expr::Variable(1));
    auto original = CloneExpr(*mba);

    Options opts;
    opts.bitwidth = 8;
    auto result   = CollapseProductIdentities(std::move(mba), { "a", "b" }, opts);

    EXPECT_TRUE(result.changed);
    EXPECT_TRUE(semantically_equal(*original, *result.expr, 2, 8));
}

// --- Factors that are bitwise combinations ---

TEST(ProductIdentityRecovererTest, XorFactorCollapse) {
    // MBA-encode (a^b) * c
    auto mba = build_mba_product(
        Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2)
    );
    auto original = CloneExpr(*mba);

    Options opts;
    opts.bitwidth = 64;
    auto result   = CollapseProductIdentities(std::move(mba), { "a", "b", "c" }, opts);

    EXPECT_TRUE(result.changed);
    EXPECT_TRUE(semantically_equal(*original, *result.expr, 3, 64));
}

// --- Semantic preservation on all cases ---

TEST(ProductIdentityRecovererTest, FullWidthSemanticPreservation) {
    // Verify with many random samples that collapsed result
    // matches original at full 64-bit width
    auto mba      = build_mba_product(Expr::Variable(0), Expr::Variable(1));
    auto original = CloneExpr(*mba);

    Options opts;
    opts.bitwidth = 64;
    auto result   = CollapseProductIdentities(std::move(mba), { "a", "b" }, opts);

    ASSERT_TRUE(result.changed);
    EXPECT_TRUE(semantically_equal(*original, *result.expr, 2, 64, 64));
}
