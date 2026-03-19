#include "cobra/core/Expr.h"
#include "cobra/core/PatternMatcher.h"
#include "cobra/core/SignatureChecker.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(PatternMatcherTest, ConstantAllEqual) {
    std::vector< uint64_t > sig = { 42, 42, 42, 42 };
    auto result                 = MatchPattern(sig, 2, 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value()->kind, Expr::Kind::kConstant);
    EXPECT_EQ(result.value()->constant_val, 42u);
}

TEST(PatternMatcherTest, ConstantSingleEntry) {
    std::vector< uint64_t > sig = { 7 };
    auto result                 = MatchPattern(sig, 0, 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value()->kind, Expr::Kind::kConstant);
    EXPECT_EQ(result.value()->constant_val, 7u);
}

TEST(PatternMatcherTest, ConstantManyVars) {
    std::vector< uint64_t > sig(8, 99);
    auto result = MatchPattern(sig, 3, 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value()->constant_val, 99u);
}

// 2-var Boolean table: all 14 non-constant functions
TEST(PatternMatcherTest, TwoVarBooleanAnd) {
    auto r = MatchPattern({ 0, 0, 0, 1 }, 2, 64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value()->kind, Expr::Kind::kAnd);
}

TEST(PatternMatcherTest, TwoVarBooleanOr) {
    auto r = MatchPattern({ 0, 1, 1, 1 }, 2, 64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value()->kind, Expr::Kind::kOr);
}

TEST(PatternMatcherTest, TwoVarBooleanXor) {
    auto r = MatchPattern({ 0, 1, 1, 0 }, 2, 64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value()->kind, Expr::Kind::kXor);
}

TEST(PatternMatcherTest, TwoVarBooleanNand) {
    auto r = MatchPattern({ 1, 1, 1, 0 }, 2, 64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value()->kind, Expr::Kind::kNot);
}

TEST(PatternMatcherTest, TwoVarBooleanNor) {
    auto r = MatchPattern({ 1, 0, 0, 0 }, 2, 64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value()->kind, Expr::Kind::kNot);
}

TEST(PatternMatcherTest, TwoVarBooleanXnor) {
    auto r = MatchPattern({ 1, 0, 0, 1 }, 2, 64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value()->kind, Expr::Kind::kNot);
}

TEST(PatternMatcherTest, TwoVarBooleanIdentityX) {
    auto r = MatchPattern({ 0, 1, 0, 1 }, 2, 64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value()->kind, Expr::Kind::kVariable);
    EXPECT_EQ(r.value()->var_index, 0u);
}

TEST(PatternMatcherTest, TwoVarBooleanIdentityY) {
    auto r = MatchPattern({ 0, 0, 1, 1 }, 2, 64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value()->kind, Expr::Kind::kVariable);
    EXPECT_EQ(r.value()->var_index, 1u);
}

// Non-Boolean 2-var sigs defer to CoB
TEST(PatternMatcherTest, TwoVarNonBooleanDefersToCoB) {
    EXPECT_FALSE(MatchPattern({ 0, 3, 5, 8 }, 2, 64).has_value());
    EXPECT_FALSE(MatchPattern({ 0, 200, 100, 44 }, 2, 8).has_value());
    EXPECT_FALSE(MatchPattern({ 0, 1, 1, 2 }, 2, 64).has_value());
}

// 3-var Boolean table: symmetric gates
TEST(PatternMatcherTest, ThreeVarAnd) {
    auto r = MatchPattern({ 0, 0, 0, 0, 0, 0, 0, 1 }, 3, 64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value()->kind, Expr::Kind::kAnd);
}

TEST(PatternMatcherTest, ThreeVarOr) {
    auto r = MatchPattern({ 0, 1, 1, 1, 1, 1, 1, 1 }, 3, 64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value()->kind, Expr::Kind::kOr);
}

TEST(PatternMatcherTest, ThreeVarXor) {
    auto r = MatchPattern({ 0, 1, 1, 0, 1, 0, 0, 1 }, 3, 64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value()->kind, Expr::Kind::kXor);
}

TEST(PatternMatcherTest, ThreeVarNand) {
    auto r = MatchPattern({ 1, 1, 1, 1, 1, 1, 1, 0 }, 3, 64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value()->kind, Expr::Kind::kNot);
}

TEST(PatternMatcherTest, ThreeVarNor) {
    auto r = MatchPattern({ 1, 0, 0, 0, 0, 0, 0, 0 }, 3, 64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value()->kind, Expr::Kind::kNot);
}

TEST(PatternMatcherTest, ThreeVarXnor) {
    auto r = MatchPattern({ 1, 0, 0, 1, 0, 1, 1, 0 }, 3, 64);
    ASSERT_TRUE(r.has_value());
    // x ^ y ^ ~z (equivalent to ~(x ^ y ^ z))
    EXPECT_EQ(r.value()->kind, Expr::Kind::kXor);
}

// 3-var Boolean table: nested gate combos
TEST(PatternMatcherTest, ThreeVarOrAnd) {
    // z & (x | y)
    auto r = MatchPattern({ 0, 0, 0, 0, 0, 1, 1, 1 }, 3, 64);
    ASSERT_TRUE(r.has_value());
    auto text = Render(*r.value(), { "x", "y", "z" });
    EXPECT_EQ(text, "z & (x | y)");
}

TEST(PatternMatcherTest, ThreeVarXorOr) {
    // (x ^ y) | z
    auto r = MatchPattern({ 0, 1, 1, 0, 1, 1, 1, 1 }, 3, 64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value()->kind, Expr::Kind::kOr);
}

TEST(PatternMatcherTest, ThreeVarAndXor) {
    // (x & y) ^ z
    auto r = MatchPattern({ 0, 0, 0, 1, 1, 1, 1, 0 }, 3, 64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value()->kind, Expr::Kind::kXor);
}

// 3-var Boolean table: majority & mux
TEST(PatternMatcherTest, ThreeVarMajority) {
    // majority(x,y,z): at least 2 of 3
    auto r = MatchPattern({ 0, 0, 0, 1, 0, 1, 1, 1 }, 3, 64);
    ASSERT_TRUE(r.has_value());
    // Generated form: x ^ ((x ^ y) & (x ^ z))
    EXPECT_EQ(r.value()->kind, Expr::Kind::kXor);
}

TEST(PatternMatcherTest, ThreeVarMinority) {
    // minority(x,y,z): complement of majority
    auto r = MatchPattern({ 1, 1, 1, 0, 1, 0, 0, 0 }, 3, 64);
    ASSERT_TRUE(r.has_value());
    // Generated form: x ^ ((x ^ y) | (x ^ z))
    EXPECT_EQ(r.value()->kind, Expr::Kind::kXor);
}

TEST(PatternMatcherTest, ThreeVarMux) {
    // z ? y : x = x ^ (z & (x ^ y))
    auto r = MatchPattern({ 0, 1, 0, 1, 0, 0, 1, 1 }, 3, 64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value()->kind, Expr::Kind::kXor);
}

// Complement of a nested gate (handled via NOT fallback)
TEST(PatternMatcherTest, ThreeVarComplementNestedGate) {
    // ~((x & y) | z) — complement of key 0xF8 is 0x07
    auto r = MatchPattern({ 1, 1, 1, 0, 0, 0, 0, 0 }, 3, 64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value()->kind, Expr::Kind::kNot);
}

// 3-var non-Boolean defers to later pipeline
TEST(PatternMatcherTest, ThreeVarNonBooleanNoMatch) {
    std::vector< uint64_t > sig = { 0, 1, 2, 3, 4, 5, 6, 7 };
    auto result                 = MatchPattern(sig, 3, 64);
    EXPECT_FALSE(result.has_value());
}

TEST(PatternMatcherTest, SingleVarIdentity) {
    std::vector< uint64_t > sig = { 0, 1 };
    auto result                 = MatchPattern(sig, 1, 64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value()->kind, Expr::Kind::kVariable);
}

// Gap #6: match_1var non-trivial affine forms
TEST(PatternMatcherTest, SingleVarScaled) {
    // f(x) = 3*x at 8-bit: sig = [0, 3]
    std::vector< uint64_t > sig = { 5, 8 };
    auto result                 = MatchPattern(sig, 1, 8);
    ASSERT_TRUE(result.has_value());
    // Should match 5 + 3*x
    auto text = Render(*result.value(), { "x" });
    EXPECT_EQ(text, "5 + 3 * x");
}

TEST(PatternMatcherTest, SingleVarWrapping8bit) {
    // f(x) = 200 + 50*x at 8-bit wraps: sig = [200, 250]
    // a = ModSub(250, 200, 8) = 50
    std::vector< uint64_t > sig = { 200, 250 };
    auto result                 = MatchPattern(sig, 1, 8);
    ASSERT_TRUE(result.has_value());
    auto text = Render(*result.value(), { "x" }, 8);
    EXPECT_EQ(text, "-56 + 50 * x");
}

// Non-zero constant offset: no pattern match (handled by CoB)
TEST(PatternMatcherTest, TwoVarWithConstantNoMatch) {
    std::vector< uint64_t > sig = { 5, 6, 6, 7 };
    auto result                 = MatchPattern(sig, 2, 64);
    EXPECT_FALSE(result.has_value());
}

// 4-var Boolean: basic gates
TEST(PatternMatcherTest, FourVarAndAll) {
    // x & y & z & w: only set at input 0b1111 = 15
    std::vector< uint64_t > sig(16, 0);
    sig[15] = 1;
    auto r  = MatchPattern(sig, 4, 64);
    ASSERT_TRUE(r.has_value());
    for (uint32_t i = 0; i < 16; ++i) {
        uint64_t got =
            EvalExpr(
                *r.value(),
                { (i >> 0) & 1ULL, (i >> 1) & 1ULL, (i >> 2) & 1ULL, (i >> 3) & 1ULL }, 64
            )
            & 1;
        EXPECT_EQ(got, sig[i]);
    }
}

TEST(PatternMatcherTest, FourVarOrAll) {
    // x | y | z | w: clear only at input 0
    std::vector< uint64_t > sig(16, 1);
    sig[0] = 0;
    auto r = MatchPattern(sig, 4, 64);
    ASSERT_TRUE(r.has_value());
    for (uint32_t i = 0; i < 16; ++i) {
        uint64_t got =
            EvalExpr(
                *r.value(),
                { (i >> 0) & 1ULL, (i >> 1) & 1ULL, (i >> 2) & 1ULL, (i >> 3) & 1ULL }, 64
            )
            & 1;
        EXPECT_EQ(got, sig[i]);
    }
}

TEST(PatternMatcherTest, FourVarXorAll) {
    // x ^ y ^ z ^ w (parity)
    std::vector< uint64_t > sig(16);
    for (int i = 0; i < 16; ++i) { sig[i] = ((i >> 0) ^ (i >> 1) ^ (i >> 2) ^ (i >> 3)) & 1; }
    auto r = MatchPattern(sig, 4, 64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value()->kind, Expr::Kind::kXor);
}

TEST(PatternMatcherTest, FourVarJustW) {
    // f = w: sig[i] = (i >> 3) & 1
    std::vector< uint64_t > sig(16);
    for (int i = 0; i < 16; ++i) { sig[i] = (i >> 3) & 1; }
    auto r = MatchPattern(sig, 4, 64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value()->kind, Expr::Kind::kVariable);
    EXPECT_EQ(r.value()->var_index, 3u);
}

TEST(PatternMatcherTest, FourVarXandW) {
    // f = x & w
    std::vector< uint64_t > sig(16);
    for (int i = 0; i < 16; ++i) { sig[i] = ((i >> 0) & 1) & ((i >> 3) & 1); }
    auto r = MatchPattern(sig, 4, 64);
    ASSERT_TRUE(r.has_value());
    for (uint32_t i = 0; i < 16; ++i) {
        uint64_t got =
            EvalExpr(
                *r.value(),
                { (i >> 0) & 1ULL, (i >> 1) & 1ULL, (i >> 2) & 1ULL, (i >> 3) & 1ULL }, 64
            )
            & 1;
        EXPECT_EQ(got, sig[i]);
    }
}

// 5-var Boolean: basic gates
TEST(PatternMatcherTest, FiveVarAndAll) {
    // x & y & z & w & v: only set at input 0b11111 = 31
    std::vector< uint64_t > sig(32, 0);
    sig[31] = 1;
    auto r  = MatchPattern(sig, 5, 64);
    ASSERT_TRUE(r.has_value());
    for (uint32_t i = 0; i < 32; ++i) {
        uint64_t got = EvalExpr(
                           *r.value(),
                           { (i >> 0) & 1ULL, (i >> 1) & 1ULL, (i >> 2) & 1ULL, (i >> 3) & 1ULL,
                             (i >> 4) & 1ULL },
                           64
                       )
            & 1;
        EXPECT_EQ(got, sig[i]);
    }
}

TEST(PatternMatcherTest, FiveVarOrAll) {
    std::vector< uint64_t > sig(32, 1);
    sig[0] = 0;
    auto r = MatchPattern(sig, 5, 64);
    ASSERT_TRUE(r.has_value());
    for (uint32_t i = 0; i < 32; ++i) {
        uint64_t got = EvalExpr(
                           *r.value(),
                           { (i >> 0) & 1ULL, (i >> 1) & 1ULL, (i >> 2) & 1ULL, (i >> 3) & 1ULL,
                             (i >> 4) & 1ULL },
                           64
                       )
            & 1;
        EXPECT_EQ(got, sig[i]);
    }
}

TEST(PatternMatcherTest, FiveVarXorAll) {
    std::vector< uint64_t > sig(32);
    for (int i = 0; i < 32; ++i) {
        sig[i] = ((i >> 0) ^ (i >> 1) ^ (i >> 2) ^ (i >> 3) ^ (i >> 4)) & 1;
    }
    auto r = MatchPattern(sig, 5, 64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value()->kind, Expr::Kind::kXor);
}

TEST(PatternMatcherTest, FiveVarJustV) {
    std::vector< uint64_t > sig(32);
    for (int i = 0; i < 32; ++i) { sig[i] = (i >> 4) & 1; }
    auto r = MatchPattern(sig, 5, 64);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value()->kind, Expr::Kind::kVariable);
    EXPECT_EQ(r.value()->var_index, 4u);
}

TEST(PatternMatcherTest, FiveVarMixed) {
    // f = (x ^ y) & (z | (w & v))
    std::vector< uint64_t > sig(32);
    for (int i = 0; i < 32; ++i) {
        int x = (i >> 0) & 1, y = (i >> 1) & 1, z = (i >> 2) & 1;
        int w = (i >> 3) & 1, v = (i >> 4) & 1;
        sig[i] = (x ^ y) & (z | (w & v));
    }
    auto r = MatchPattern(sig, 5, 64);
    ASSERT_TRUE(r.has_value());
    for (uint32_t i = 0; i < 32; ++i) {
        uint64_t got = EvalExpr(
                           *r.value(),
                           { (i >> 0) & 1ULL, (i >> 1) & 1ULL, (i >> 2) & 1ULL, (i >> 3) & 1ULL,
                             (i >> 4) & 1ULL },
                           64
                       )
            & 1;
        EXPECT_EQ(got, sig[i]);
    }
}

// 6+ vars: pattern matcher returns nullopt (defers to ANF/CoB)
TEST(PatternMatcherTest, SixVarReturnsNullopt) {
    std::vector< uint64_t > sig(64, 0);
    sig[63] = 1;
    EXPECT_FALSE(MatchPattern(sig, 6, 64).has_value());
}

// Scalar factoring: constant + scalar * boolean_pattern
TEST(PatternMatcherTest, ThreeVarOrPlusConstant) {
    // (a|b|c) + 67, bitwidth 16 → sig [67,68,68,68,68,68,68,68]
    std::vector< uint64_t > sig = { 67, 68, 68, 68, 68, 68, 68, 68 };
    auto r                      = MatchPattern(sig, 3, 16);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value()->kind, Expr::Kind::kAdd);
    for (uint32_t i = 0; i < 8; ++i) {
        uint64_t x   = (i >> 0) & 1;
        uint64_t y   = (i >> 1) & 1;
        uint64_t z   = (i >> 2) & 1;
        uint64_t got = EvalExpr(*r.value(), { x, y, z }, 16);
        EXPECT_EQ(got, sig[i]) << "input=(" << x << "," << y << "," << z << ")";
    }
}

TEST(PatternMatcherTest, ThreeVarOrTimesScalar) {
    // (a|b|c) * 67, bitwidth 16 → sig [0,67,67,67,67,67,67,67]
    std::vector< uint64_t > sig = { 0, 67, 67, 67, 67, 67, 67, 67 };
    auto r                      = MatchPattern(sig, 3, 16);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value()->kind, Expr::Kind::kMul);
    for (uint32_t i = 0; i < 8; ++i) {
        uint64_t x   = (i >> 0) & 1;
        uint64_t y   = (i >> 1) & 1;
        uint64_t z   = (i >> 2) & 1;
        uint64_t got = EvalExpr(*r.value(), { x, y, z }, 16);
        EXPECT_EQ(got, sig[i]) << "input=(" << x << "," << y << "," << z << ")";
    }
}

TEST(PatternMatcherTest, ThreeVarScaledMBA) {
    // 67*((a^b)|(a^c)) + 67*(a&b&c) = 67*(a|b|c)
    std::vector< uint64_t > sig = { 0, 67, 67, 67, 67, 67, 67, 67 };
    auto r                      = MatchPattern(sig, 3, 16);
    ASSERT_TRUE(r.has_value());
    for (uint32_t i = 0; i < 8; ++i) {
        uint64_t x   = (i >> 0) & 1;
        uint64_t y   = (i >> 1) & 1;
        uint64_t z   = (i >> 2) & 1;
        uint64_t got = EvalExpr(*r.value(), { x, y, z }, 16);
        EXPECT_EQ(got, sig[i]) << "input=(" << x << "," << y << "," << z << ")";
    }
}

TEST(PatternMatcherTest, ThreeVarMBAWithConstantOffset) {
    // ((a^b)|(a^c)) + 65469*~(a&b&c) + 65470*(a&b&c)
    // = (a|b|c) + 67
    std::vector< uint64_t > sig = { 67, 68, 68, 68, 68, 68, 68, 68 };
    auto r                      = MatchPattern(sig, 3, 16);
    ASSERT_TRUE(r.has_value());
    for (uint32_t i = 0; i < 8; ++i) {
        uint64_t x   = (i >> 0) & 1;
        uint64_t y   = (i >> 1) & 1;
        uint64_t z   = (i >> 2) & 1;
        uint64_t got = EvalExpr(*r.value(), { x, y, z }, 16);
        EXPECT_EQ(got, sig[i]) << "input=(" << x << "," << y << "," << z << ")";
    }
}

TEST(PatternMatcherTest, TwoVarXorTimesScalar) {
    // 42*(a^b), bitwidth 16 → sig [0,42,42,0]
    std::vector< uint64_t > sig = { 0, 42, 42, 0 };
    auto r                      = MatchPattern(sig, 2, 16);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value()->kind, Expr::Kind::kMul);
    for (uint32_t i = 0; i < 4; ++i) {
        uint64_t x   = (i >> 0) & 1;
        uint64_t y   = (i >> 1) & 1;
        uint64_t got = EvalExpr(*r.value(), { x, y }, 16);
        EXPECT_EQ(got, sig[i]) << "input=(" << x << "," << y << ")";
    }
}

TEST(PatternMatcherTest, ScalarFactoringNonFactorableNoMatch) {
    // Three unique values: not factorable as c + k*bool
    std::vector< uint64_t > sig = { 5, 6, 6, 7 };
    auto result                 = MatchPattern(sig, 2, 64);
    EXPECT_FALSE(result.has_value());
}

// Exhaustive: verify all 256 3-var Boolean functions produce correct
// expressions by evaluating at every input combination.
TEST(PatternMatcherTest, ThreeVarExhaustiveCorrectness) {
    int matched = 0;
    for (uint32_t key = 0; key < 256; ++key) {
        // Build signature from key
        std::vector< uint64_t > sig(8);
        for (int i = 0; i < 8; ++i) { sig[i] = (key >> i) & 1; }

        auto result = MatchPattern(sig, 3, 64);
        if (!result.has_value()) { continue; }
        ++matched;

        // Evaluate the expression at all 8 input points
        for (uint32_t i = 0; i < 8; ++i) {
            uint64_t x   = (i >> 0) & 1;
            uint64_t y   = (i >> 1) & 1;
            uint64_t z   = (i >> 2) & 1;
            uint64_t got = EvalExpr(*result.value(), { x, y, z }, 64) & 1;
            EXPECT_EQ(got, sig[i]) << "key=0x" << std::hex << key << " input=(" << x << "," << y
                                   << "," << z << ") expected=" << sig[i] << " got=" << got;
        }
    }
    // All 256 non-... well, all 256 including constants
    // Constants (0x00, 0xFF) handled by all_equal
    // Other 254 handled by 3var table
    EXPECT_EQ(matched, 256);
}

// Exhaustive: verify all 65536 4-var Boolean functions produce correct
// expressions by evaluating at every input combination.
TEST(PatternMatcherTest, FourVarExhaustiveCorrectness) {
    int matched = 0;
    for (uint32_t key = 0; key < 65536; ++key) {
        std::vector< uint64_t > sig(16);
        for (int i = 0; i < 16; ++i) { sig[i] = (key >> i) & 1; }

        auto result = MatchPattern(sig, 4, 64);
        if (!result.has_value()) { continue; }
        ++matched;

        for (uint32_t i = 0; i < 16; ++i) {
            uint64_t got =
                EvalExpr(
                    *result.value(),
                    { (i >> 0) & 1ULL, (i >> 1) & 1ULL, (i >> 2) & 1ULL, (i >> 3) & 1ULL }, 64
                )
                & 1;
            EXPECT_EQ(got, sig[i]) << "key=0x" << std::hex << key << " input=" << std::dec << i
                                   << " expected=" << sig[i] << " got=" << got;
        }
    }
    EXPECT_EQ(matched, 65536);
}

// Sampled verification of 5-var Boolean functions.
// 2^32 functions is too many for exhaustive, so we test:
// - all functions that embed a 4-var function (v-independent)
// - structured families (single-var, parity, AND/OR chains)
// - random sample via deterministic PRNG
TEST(PatternMatcherTest, FiveVarSampledCorrectness) {
    auto verify = [](uint32_t key) {
        std::vector< uint64_t > sig(32);
        for (int i = 0; i < 32; ++i) { sig[i] = (key >> i) & 1; }
        auto result = MatchPattern(sig, 5, 64);
        EXPECT_TRUE(result.has_value()) << "key=0x" << std::hex << key;
        if (!result.has_value()) { return; }
        for (uint32_t i = 0; i < 32; ++i) {
            uint64_t got = EvalExpr(
                               *result.value(),
                               { (i >> 0) & 1ULL, (i >> 1) & 1ULL, (i >> 2) & 1ULL,
                                 (i >> 3) & 1ULL, (i >> 4) & 1ULL },
                               64
                           )
                & 1;
            EXPECT_EQ(got, sig[i]) << "key=0x" << std::hex << key << " input=" << std::dec << i;
        }
    };

    // Constants
    verify(0x00000000);
    verify(0xFFFFFFFF);

    // Single variables and their complements
    verify(0xAAAAAAAA); // x
    verify(0x55555555); // ~x
    verify(0xCCCCCCCC); // y
    verify(0xF0F0F0F0); // z
    verify(0xFF00FF00); // w
    verify(0xFFFF0000); // v
    verify(0x0000FFFF); // ~v

    // 2-input gates across all variable pairs
    verify(0xAAAAAAAA & 0xCCCCCCCC); // x & y
    verify(0xAAAAAAAA | 0xFFFF0000); // x | v
    verify(0xFF00FF00 ^ 0xFFFF0000); // w ^ v
    verify(0xF0F0F0F0 & 0xFF00FF00); // z & w

    // 5-input gates
    verify(0x80000000); // x & y & z & w & v
    verify(0xFFFFFFFE); // x | y | z | w | v
    uint32_t parity = 0;
    for (int i = 0; i < 32; ++i) {
        if (((i >> 0) ^ (i >> 1) ^ (i >> 2) ^ (i >> 3) ^ (i >> 4)) & 1) {
            parity |= (uint32_t{ 1 } << i);
        }
    }
    verify(parity); // x ^ y ^ z ^ w ^ v

    // Majority-like: at least 3 of 5
    uint32_t maj = 0;
    for (int i = 0; i < 32; ++i) {
        int cnt =
            ((i >> 0) & 1) + ((i >> 1) & 1) + ((i >> 2) & 1) + ((i >> 3) & 1) + ((i >> 4) & 1);
        if (cnt >= 3) { maj |= (uint32_t{ 1 } << i); }
    }
    verify(maj);

    // Deterministic random sample (xorshift32)
    uint32_t rng = 0xDEADBEEF;
    for (int trial = 0; trial < 10000; ++trial) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        verify(rng);
    }
}
