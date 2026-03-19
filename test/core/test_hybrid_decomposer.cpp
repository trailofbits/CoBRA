#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/HybridDecomposer.h"
#include "cobra/core/SignatureSimplifier.h"
#include <gtest/gtest.h>

using namespace cobra;

namespace {

    std::vector< uint64_t >
    make_sig(uint32_t n, std::function< uint64_t(const std::vector< uint64_t > &) > f) {
        std::vector< uint64_t > sig(1u << n);
        for (uint32_t i = 0; i < (1u << n); ++i) {
            std::vector< uint64_t > vals(n);
            for (uint32_t j = 0; j < n; ++j) { vals[j] = (i >> j) & 1; }
            sig[i] = f(vals);
        }
        return sig;
    }

    SignatureContext make_ctx(
        const std::vector< std::string > &vars,
        std::function< uint64_t(const std::vector< uint64_t > &) > eval
    ) {
        SignatureContext ctx;
        ctx.vars = vars;
        ctx.original_indices.resize(vars.size());
        for (uint32_t i = 0; i < vars.size(); ++i) { ctx.original_indices[i] = i; }
        if (eval) { ctx.eval = eval; }
        return ctx;
    }

    Options default_opts() {
        Options opts{};
        opts.bitwidth                     = 64;
        opts.max_vars                     = 12;
        opts.spot_check                   = true;
        opts.enable_bitwise_decomposition = true;
        return opts;
    }

} // namespace

TEST(HybridDecomposerTest, XorExtraction) {
    // f = x ^ (y & z): residual after XOR-extracting x is y&z.
    auto f = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] ^ (v[1] & v[2]); };
    auto sig  = make_sig(3, f);
    auto ctx  = make_ctx({ "x", "y", "z" }, f);
    auto opts = default_opts();

    auto result = TryHybridDecomposition(sig, ctx, opts, 0, nullptr);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->verified);
    EXPECT_EQ(result->expr->kind, Expr::Kind::kXor);
}

TEST(HybridDecomposerTest, AddExtraction) {
    // f = x + (y ^ z): residual after ADD-extracting x is y^z.
    auto f = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] + (v[1] ^ v[2]); };
    auto sig  = make_sig(3, f);
    auto ctx  = make_ctx({ "x", "y", "z" }, f);
    auto opts = default_opts();

    auto result = TryHybridDecomposition(sig, ctx, opts, 0, nullptr);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->verified);
    EXPECT_EQ(result->expr->kind, Expr::Kind::kAdd);
}

TEST(HybridDecomposerTest, DepthGateBlocksAtDepth1) {
    // Hybrid extraction only fires at depth 0.
    auto f = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] ^ (v[1] & v[2]); };
    auto sig  = make_sig(3, f);
    auto ctx  = make_ctx({ "x", "y", "z" }, f);
    auto opts = default_opts();

    auto result = TryHybridDecomposition(sig, ctx, opts, 1, nullptr);
    EXPECT_FALSE(result.has_value());
}

TEST(HybridDecomposerTest, NoEvaluatorReturnsNullopt) {
    auto f   = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] ^ v[1]; };
    auto sig = make_sig(2, f);
    SignatureContext ctx;
    ctx.vars             = { "x", "y" };
    ctx.original_indices = { 0, 1 };
    auto opts            = default_opts();

    auto result = TryHybridDecomposition(sig, ctx, opts, 0, nullptr);
    EXPECT_FALSE(result.has_value());
}

TEST(HybridDecomposerTest, BaselineCostRejectsWorse) {
    // x ^ y is already optimal; extraction shouldn't beat it.
    auto f    = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] ^ v[1]; };
    auto sig  = make_sig(2, f);
    auto ctx  = make_ctx({ "x", "y" }, f);
    auto opts = default_opts();

    ExprCost baseline{ 3, 0, 2 };
    auto result = TryHybridDecomposition(sig, ctx, opts, 0, &baseline);
    EXPECT_FALSE(result.has_value());
}

TEST(HybridDecomposerTest, SkipsIdentityExtraction) {
    // For a constant signature, XOR/ADD with any variable should
    // not produce a simpler residual (or the residual equals sig).
    auto f    = [](const std::vector< uint64_t > &) -> uint64_t { return 42; };
    auto sig  = make_sig(2, f);
    auto ctx  = make_ctx({ "x", "y" }, f);
    auto opts = default_opts();

    auto result = TryHybridDecomposition(sig, ctx, opts, 0, nullptr);
    // May or may not find a result, but must not crash.
    // The residual sig differs from the original for non-zero
    // constants, so it might still find a decomposition.
    // Just verify no crash and that any result is verified.
    if (result.has_value()) { EXPECT_TRUE(result->verified); }
}
