#include "cobra/core/BitwiseDecomposer.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/SignatureChecker.h"
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

} // namespace

TEST(BitwiseDecomposerTest, DOrCA) {
    // d | (c * a): vars d=0, c=1, a=2
    auto f = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] | (v[1] * v[2]); };
    auto sig = make_sig(3, f);
    auto ctx = make_ctx({ "d", "c", "a" }, f);
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = TryBitwiseDecomposition(sig, ctx, opts, 0, nullptr);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.Payload().verification == VerificationState::kVerified);
    EXPECT_EQ(result.Payload().expr->kind, Expr::Kind::kOr);
}

TEST(BitwiseDecomposerTest, ESquaredAndD) {
    // (e*e) & d: On {0,1}: e*e = e, so e&d
    auto f = [](const std::vector< uint64_t > &v) -> uint64_t { return (v[0] * v[0]) & v[1]; };
    auto sig = make_sig(2, f);
    auto ctx = make_ctx({ "e", "d" }, f);
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = TryBitwiseDecomposition(sig, ctx, opts, 0, nullptr);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.Payload().verification == VerificationState::kVerified);
    EXPECT_EQ(result.Payload().expr->kind, Expr::Kind::kAnd);
}

TEST(BitwiseDecomposerTest, DSquaredXorA) {
    // (d*d) ^ a: On {0,1}: d*d = d, so d^a
    auto f = [](const std::vector< uint64_t > &v) -> uint64_t { return (v[0] * v[0]) ^ v[1]; };
    auto sig = make_sig(2, f);
    auto ctx = make_ctx({ "d", "a" }, f);
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = TryBitwiseDecomposition(sig, ctx, opts, 0, nullptr);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.Payload().verification == VerificationState::kVerified);
    EXPECT_EQ(result.Payload().expr->kind, Expr::Kind::kXor);
}

TEST(BitwiseDecomposerTest, AddDecompXPlusY) {
    // x + y: sig = [0, 1, 1, 2], ADD decomposition finds 1*x + y.
    auto f   = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] + v[1]; };
    auto sig = make_sig(2, f);
    auto ctx = make_ctx({ "x", "y" }, f);
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = TryBitwiseDecomposition(sig, ctx, opts, 0, nullptr);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.Payload().verification == VerificationState::kVerified);
    EXPECT_EQ(result.Payload().expr->kind, Expr::Kind::kAdd);
}

TEST(BitwiseDecomposerTest, DepthCapReturnsNullopt) {
    auto f = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] | (v[1] * v[2]); };
    auto sig = make_sig(3, f);
    auto ctx = make_ctx({ "d", "c", "a" }, f);
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = TryBitwiseDecomposition(sig, ctx, opts, 2, nullptr);
    EXPECT_FALSE(result.Succeeded());
}

TEST(BitwiseDecomposerTest, BaselineGateRejectsWorse) {
    auto f   = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] & v[1]; };
    auto sig = make_sig(2, f);
    auto ctx = make_ctx({ "x", "y" }, f);
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    ExprCost baseline{ 1, 0, 1 };
    auto result = TryBitwiseDecomposition(sig, ctx, opts, 0, &baseline);
    EXPECT_FALSE(result.Succeeded());
}

TEST(BitwiseDecomposerTest, NoEvaluatorReturnsNullopt) {
    auto f = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] | (v[1] * v[2]); };
    auto sig = make_sig(3, f);
    SignatureContext ctx;
    ctx.vars             = { "d", "c", "a" };
    ctx.original_indices = { 0, 1, 2 };
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = TryBitwiseDecomposition(sig, ctx, opts, 0, nullptr);
    EXPECT_FALSE(result.Succeeded());
}

TEST(BitwiseDecomposerTest, CofactorOrdering) {
    // Verify cofactor extraction indexing is correct
    auto f   = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] & v[1]; };
    auto sig = make_sig(2, f);
    ASSERT_EQ(sig.size(), 4u);
    EXPECT_EQ(sig[0], 0u); // x=0, y=0
    EXPECT_EQ(sig[1], 0u); // x=1, y=0
    EXPECT_EQ(sig[2], 0u); // x=0, y=1
    EXPECT_EQ(sig[3], 1u); // x=1, y=1
}

TEST(BitwiseDecomposerTest, WordValuedOrAccepted) {
    // v | (a + b): word-valued g = a+b, now accepted by generalized
    // OR check: cofactor1[j] == (cofactor0[j] | 1).
    auto f = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] | (v[1] + v[2]); };
    auto sig = make_sig(3, f);
    auto ctx = make_ctx({ "v", "a", "b" }, f);
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    opts.evaluator = f;

    auto result = TryBitwiseDecomposition(sig, ctx, opts, 0, nullptr);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.Payload().verification == VerificationState::kVerified);
    EXPECT_EQ(result.Payload().expr->kind, Expr::Kind::kOr);
}

TEST(BitwiseDecomposerTest, MultiLevelPureBitwise) {
    // a & (b | (c & d)): purely bitwise, decomposable at outer level
    // with ANF solving the inner sub-problem b | (c & d).
    // The bitwise-over-polynomial multi-level case (e.g. a & (b | (c*d)))
    // requires the decomposer wired into simplify_from_signature (Task 5).
    auto f = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] & (v[1] | (v[2] & v[3]));
    };
    auto sig = make_sig(4, f);
    auto ctx = make_ctx({ "a", "b", "c", "d" }, f);
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = TryBitwiseDecomposition(sig, ctx, opts, 0, nullptr);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.Payload().verification == VerificationState::kVerified);
    EXPECT_EQ(result.Payload().expr->kind, Expr::Kind::kAnd);
}

TEST(BitwiseDecomposerTest, VerifiedButNotBetterRejected) {
    auto f   = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] & v[1]; };
    auto sig = make_sig(2, f);
    auto ctx = make_ctx({ "x", "y" }, f);
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    ExprCost baseline{ 3, 0, 2 }; // cost of x & y itself
    auto result = TryBitwiseDecomposition(sig, ctx, opts, 0, &baseline);
    EXPECT_FALSE(result.Succeeded());
}

// --- Phase 2: Mul-based decomposition (polynomial cofactors) ---

TEST(BitwiseDecomposerTest, MulDecompBTimesAplusA) {
    // b * (a + a) = b * 2a: cofactor b=0 → 0, b=1 → 2a (polynomial)
    auto f = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] * (v[1] + v[1]); };
    auto sig = make_sig(2, f);
    auto ctx = make_ctx({ "b", "a" }, f);
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = TryBitwiseDecomposition(sig, ctx, opts, 0, nullptr);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.Payload().verification == VerificationState::kVerified);
    EXPECT_EQ(result.Payload().expr->kind, Expr::Kind::kMul);
}

TEST(BitwiseDecomposerTest, MulDecompBTimesAplusOne) {
    // b * (a + 1): cofactor b=0 → 0, b=1 → {1, 2} (polynomial)
    auto f   = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] * (v[1] + 1); };
    auto sig = make_sig(2, f);
    auto ctx = make_ctx({ "b", "a" }, f);
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = TryBitwiseDecomposition(sig, ctx, opts, 0, nullptr);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.Payload().verification == VerificationState::kVerified);
    EXPECT_EQ(result.Payload().expr->kind, Expr::Kind::kMul);
}

TEST(BitwiseDecomposerTest, MulDecompThreeVars) {
    // a * (b + c): cofactor a=0 → 0, a=1 → b+c (polynomial)
    auto f = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] * (v[1] + v[2]); };
    auto sig = make_sig(3, f);
    auto ctx = make_ctx({ "a", "b", "c" }, f);
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = TryBitwiseDecomposition(sig, ctx, opts, 0, nullptr);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.Payload().verification == VerificationState::kVerified);
    EXPECT_EQ(result.Payload().expr->kind, Expr::Kind::kMul);
}

TEST(BitwiseDecomposerTest, MulRejectsNonZeroCof0ButAddMatches) {
    // a + b: MUL rejects (cof0 not all zero), but ADD matches.
    // cofactor a=0 → b, cofactor a=1 → 1+b: diff = 1 (constant).
    auto f   = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] + v[1]; };
    auto sig = make_sig(2, f);
    auto ctx = make_ctx({ "a", "b" }, f);
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    auto result = TryBitwiseDecomposition(sig, ctx, opts, 0, nullptr);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.Payload().verification == VerificationState::kVerified);
    EXPECT_EQ(result.Payload().expr->kind, Expr::Kind::kAdd);
}

TEST(BitwiseDecomposerTest, ShapeMatchesButGFailsToImprove) {
    auto f = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] & (v[1] | v[2]); };
    auto sig = make_sig(3, f);
    auto ctx = make_ctx({ "a", "b", "c" }, f);
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

    ExprCost baseline{ 1, 0, 1 }; // impossible to beat
    auto result = TryBitwiseDecomposition(sig, ctx, opts, 0, &baseline);
    EXPECT_FALSE(result.Succeeded());
}

// --- Word-valued cofactor decomposition ---

TEST(BitwiseDecomposerTest, WordValuedXorDecomp) {
    // d ^ (a + b): cofactor d=0 -> a+b, cofactor d=1 -> (a+b)^1
    // g = a+b is word-valued.
    // Current code requires cofactor1 == 1 - cofactor0, so this fails.
    auto f = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] ^ (v[1] + v[2]); };
    auto sig = make_sig(3, f);
    auto ctx = make_ctx({ "d", "a", "b" }, f);
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    opts.evaluator = f;

    auto result = TryBitwiseDecomposition(sig, ctx, opts, 0, nullptr);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.Payload().verification == VerificationState::kVerified);
    EXPECT_EQ(result.Payload().expr->kind, Expr::Kind::kXor);
}

TEST(BitwiseDecomposerTest, WordValuedOrTwoVars) {
    // a | (a * b): cofactor a=0 -> 0, cofactor a=1 -> b|1
    // Actually: a=0 -> 0|(0*b) = 0, a=1 -> 1|(1*b) = 1|b
    // cofactor0 = [0,0], cofactor1 = [1,1] -> all ones, boolean OR match.
    // Let's use: a | (b*b): cofactor a=0 -> b*b, cofactor a=1 -> (b*b)|1
    // On {0,1}: b*b = b, so sig = [0,1,1,1] — boolean.
    // We need full-width: use evaluator where b*b != b.
    // Actually on the {0,1} cube b*b=b, so sig is boolean.
    // For a true word-valued test, we need a polynomial sub-expression.
    // a | (b + 1): cofactor a=0 -> b+1 = {1,2}, a=1 -> (b+1)|1 = {1,3}
    auto f   = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] | (v[1] + 1); };
    auto sig = make_sig(2, f);
    auto ctx = make_ctx({ "a", "b" }, f);
    Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    opts.evaluator = f;

    auto result = TryBitwiseDecomposition(sig, ctx, opts, 0, nullptr);
    ASSERT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.Payload().verification == VerificationState::kVerified);
    EXPECT_EQ(result.Payload().expr->kind, Expr::Kind::kOr);
}
