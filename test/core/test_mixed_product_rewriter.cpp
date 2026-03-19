#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/MixedProductRewriter.h"
#include "cobra/core/SignatureChecker.h"
#include <gtest/gtest.h>
#include <random>

using namespace cobra;

// Helper: evaluate at random full-width points
static bool semantically_equal(
    const Expr &a, const Expr &b, uint32_t num_vars, uint32_t bitwidth, int samples = 16
) {
    std::mt19937_64 rng(42);
    uint64_t mask = (bitwidth >= 64) ? UINT64_MAX : ((1ULL << bitwidth) - 1);
    for (int s = 0; s < samples; ++s) {
        std::vector< uint64_t > v(num_vars);
        for (uint32_t i = 0; i < num_vars; ++i) {
            v[i] = rng() & mask;
        }
        if (EvalExpr(a, v, bitwidth) != EvalExpr(b, v, bitwidth)) {
            return false;
        }
    }
    return true;
}

TEST(MixedProductRewriterTest, NodeCount) {
    // x + y: Add -> Var, Var = 3 nodes
    auto e = Expr::Add(Expr::Variable(0), Expr::Variable(1));
    EXPECT_EQ(NodeCount(*e), 3u);
}

TEST(MixedProductRewriterTest, NodeCountDeep) {
    // (x & y) * z: Mul -> And -> Var, Var ; Var = 5 nodes
    auto e =
        Expr::Mul(Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2));
    EXPECT_EQ(NodeCount(*e), 5u);
}

TEST(MixedProductRewriterTest, XorLoweringInMixedContext) {
    // (x ^ y) * z -> XOR lowered
    auto e =
        Expr::Mul(Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2));
    auto original = CloneExpr(*e);

    RewriteOptions opts;
    opts.bitwidth = 64;
    auto result   = RewriteMixedProducts(std::move(e), opts);

    EXPECT_TRUE(result.structure_changed);
    EXPECT_GE(result.rounds_applied, 1u);
    EXPECT_TRUE(semantically_equal(*original, *result.expr, 3, 64));
}

TEST(MixedProductRewriterTest, TopLevelXorUnchanged) {
    // x ^ y at top level -> NOT in mixed context, unchanged
    auto e        = Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1));
    auto original = CloneExpr(*e);

    RewriteOptions opts;
    opts.bitwidth = 64;
    auto result   = RewriteMixedProducts(std::move(e), opts);

    EXPECT_FALSE(result.structure_changed);
    EXPECT_EQ(result.rounds_applied, 0u);
}

TEST(MixedProductRewriterTest, AndTimesZUnchanged) {
    // (x & y) * z -> no XOR to lower
    auto e =
        Expr::Mul(Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2));
    auto original = CloneExpr(*e);

    RewriteOptions opts;
    opts.bitwidth = 64;
    auto result   = RewriteMixedProducts(std::move(e), opts);

    EXPECT_FALSE(result.structure_changed);
    EXPECT_EQ(result.rounds_applied, 0u);
}

TEST(MixedProductRewriterTest, XorInAddContext) {
    // ((x ^ y) * z) + w -> XOR lowered in mixed subtree
    auto e = Expr::Add(
        Expr::Mul(Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2)),
        Expr::Variable(3)
    );
    auto original = CloneExpr(*e);

    RewriteOptions opts;
    opts.bitwidth = 64;
    auto result   = RewriteMixedProducts(std::move(e), opts);

    EXPECT_TRUE(result.structure_changed);
    EXPECT_TRUE(semantically_equal(*original, *result.expr, 4, 64));
}

TEST(MixedProductRewriterTest, GrowthCapRespected) {
    auto e =
        Expr::Mul(Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2));
    uint32_t initial = NodeCount(*e);

    RewriteOptions opts;
    opts.max_node_growth = 2;
    opts.bitwidth        = 64;
    auto result          = RewriteMixedProducts(std::move(e), opts);

    if (result.structure_changed) {
        EXPECT_LE(NodeCount(*result.expr), initial * 2);
    }
}

TEST(MixedProductRewriterTest, RoundCapRespected) {
    auto e =
        Expr::Mul(Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2));

    RewriteOptions opts;
    opts.max_rounds = 2;
    opts.bitwidth   = 64;
    auto result     = RewriteMixedProducts(std::move(e), opts);

    EXPECT_LE(result.rounds_applied, 2u);
}

TEST(MixedProductRewriterTest, NoNewUnsupportedFlags) {
    auto e =
        Expr::Mul(Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2));
    auto old_flags = ClassifyStructural(*e).flags & kUnsupportedFlagMask;

    RewriteOptions opts;
    opts.bitwidth = 64;
    auto result   = RewriteMixedProducts(std::move(e), opts);

    auto new_flags = ClassifyStructural(*result.expr).flags & kUnsupportedFlagMask;
    // No new unsupported flags introduced
    EXPECT_EQ(static_cast< uint32_t >(new_flags & ~old_flags), 0u);
}

TEST(MixedProductRewriterTest, FineProgressAccepted) {
    // (x ^ y) * z: XOR lowering produces (x+y-2*(x&y))*z
    // which still has kSfHasMixedProduct but zero rewriteable sites
    auto e =
        Expr::Mul(Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2));
    auto original_sites = CountRewriteableSites(*e);
    EXPECT_GE(original_sites, 1u);

    RewriteOptions opts;
    opts.bitwidth = 64;
    auto result   = RewriteMixedProducts(std::move(e), opts);

    // Structure changed via fine progress
    EXPECT_TRUE(result.structure_changed);
    // Sites should have decreased
    EXPECT_LT(CountRewriteableSites(*result.expr), original_sites);
}

TEST(MixedProductRewriterTest, SupportedRouteUnchanged) {
    // x * y -> Multilinear, should not be rewritten
    auto e = Expr::Mul(Expr::Variable(0), Expr::Variable(1));

    RewriteOptions opts;
    opts.bitwidth = 64;
    auto result   = RewriteMixedProducts(std::move(e), opts);

    EXPECT_FALSE(result.structure_changed);
    EXPECT_EQ(result.rounds_applied, 0u);
}

TEST(MixedProductRewriterTest, CountRewriteableSites) {
    // (x ^ y) * z -> 1 rewriteable site
    auto e1 =
        Expr::Mul(Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2));
    EXPECT_EQ(CountRewriteableSites(*e1), 1u);

    // x ^ y (top-level) -> 0 rewriteable sites
    auto e2 = Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1));
    EXPECT_EQ(CountRewriteableSites(*e2), 0u);

    // (x & y) * z -> 0 rewriteable sites (no XOR)
    auto e3 =
        Expr::Mul(Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2));
    EXPECT_EQ(CountRewriteableSites(*e3), 0u);
}
