#include "cobra/core/Expr.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureSimplifier.h"
#include "cobra/core/TemplateDecomposer.h"
#include <gtest/gtest.h>

using namespace cobra;

namespace {

    SignatureContext
    make_ctx(uint32_t nv, const Evaluator &eval, const std::vector< std::string > &vars) {
        SignatureContext ctx;
        ctx.vars = vars;
        ctx.original_indices.resize(nv);
        for (uint32_t i = 0; i < nv; ++i) {
            ctx.original_indices[i] = i;
        }
        ctx.eval = eval;
        return ctx;
    }

    Options make_opts(uint32_t bw = 64) {
        Options opts;
        opts.bitwidth   = bw;
        opts.max_vars   = 12;
        opts.spot_check = true;
        return opts;
    }

} // namespace

TEST(TemplateDecomposer, DirectAtomMatch) {
    // f(x) = x  → should match directly
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0]; };
    auto ctx  = make_ctx(1, eval, { "x" });
    auto opts = make_opts();

    auto r = TryTemplateDecomposition(ctx, opts, 1, nullptr);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->verified);
}

TEST(TemplateDecomposer, BinaryXor) {
    // f(x, y) = x ^ y
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] ^ v[1]; };
    auto ctx  = make_ctx(2, eval, { "x", "y" });
    auto opts = make_opts();

    auto r = TryTemplateDecomposition(ctx, opts, 2, nullptr);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->verified);
}

TEST(TemplateDecomposer, BinaryMul) {
    // f(x, y) = x * y
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] * v[1]; };
    auto ctx  = make_ctx(2, eval, { "x", "y" });
    auto opts = make_opts();

    auto r = TryTemplateDecomposition(ctx, opts, 2, nullptr);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->verified);
}

TEST(TemplateDecomposer, MulOrNested) {
    // f(b, c) = b * c | -(b ^ c)
    // A classic QSynth_EA pattern needing Layer 2
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return (v[0] * v[1]) | (-(v[0] ^ v[1]));
    };
    auto ctx  = make_ctx(2, eval, { "b", "c" });
    auto opts = make_opts();

    auto r = TryTemplateDecomposition(ctx, opts, 2, nullptr);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->verified);

    // Verify the result computes the same function
    auto check = FullWidthCheckEval(*ctx.eval, 2, *r->expr, 64);
    EXPECT_TRUE(check.passed);
}

TEST(TemplateDecomposer, RespectsBaselineCost) {
    // f(x, y) = x ^ y — very cheap
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] ^ v[1]; };
    auto ctx  = make_ctx(2, eval, { "x", "y" });
    auto opts = make_opts();

    // Set an impossibly low baseline cost
    ExprCost baseline{ 0, 0, 0 };
    auto r = TryTemplateDecomposition(ctx, opts, 2, &baseline);
    // Should not return anything better than cost 0
    EXPECT_FALSE(r.has_value());
}

TEST(TemplateDecomposer, NoEvaluator) {
    SignatureContext ctx;
    ctx.vars  = { "x" };
    auto opts = make_opts();

    auto r = TryTemplateDecomposition(ctx, opts, 1, nullptr);
    EXPECT_FALSE(r.has_value());
}

TEST(TemplateDecomposer, TooManyVars) {
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0]; };
    std::vector< std::string > vars(7, "x");
    auto ctx  = make_ctx(7, eval, vars);
    auto opts = make_opts();

    auto r = TryTemplateDecomposition(ctx, opts, 7, nullptr);
    EXPECT_FALSE(r.has_value());
}

TEST(TemplateDecomposer, NarrowBitwidth) {
    // f(x, y) = (x + y) & 0xFF at 8-bit width
    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return (v[0] + v[1]) & 0xFF;
    };
    auto ctx  = make_ctx(2, eval, { "x", "y" });
    auto opts = make_opts(8);

    auto r = TryTemplateDecomposition(ctx, opts, 2, nullptr);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->verified);
}
