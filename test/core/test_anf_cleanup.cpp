#include "cobra/core/AnfCleanup.h"
#include "cobra/core/AnfTransform.h"
#include "cobra/core/Expr.h"
#include "cobra/core/SignatureChecker.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(AnfFormTest, FromAnfCoeffs_ConstantZero) {
    PackedAnf anf = { 0, 0, 0, 0 };
    auto form     = AnfForm::FromAnfCoeffs(anf, 2);
    EXPECT_EQ(form.constant_bit, 0);
    EXPECT_TRUE(form.monomials.empty());
}

TEST(AnfFormTest, FromAnfCoeffs_ConstantOne) {
    PackedAnf anf = { 1, 0, 0, 0 };
    auto form     = AnfForm::FromAnfCoeffs(anf, 2);
    EXPECT_EQ(form.constant_bit, 1);
    EXPECT_TRUE(form.monomials.empty());
}

TEST(AnfFormTest, FromAnfCoeffs_TwoVarOr) {
    // x | y = x ^ y ^ xy -> ANF coeffs: [0,1,1,1]
    PackedAnf anf = { 0, 1, 1, 1 };
    auto form     = AnfForm::FromAnfCoeffs(anf, 2);
    EXPECT_EQ(form.constant_bit, 0);
    EXPECT_EQ(form.num_vars, 2u);
    // monomials: {1, 2, 3} (x, y, x&y) sorted by degree then mask
    ASSERT_EQ(form.monomials.size(), 3u);
    EXPECT_EQ(form.monomials[0], 1u); // x
    EXPECT_EQ(form.monomials[1], 2u); // y
    EXPECT_EQ(form.monomials[2], 3u); // xy
}

TEST(AnfFormTest, RawEmit_SingleVariable) {
    AnfForm form;
    form.constant_bit = 0;
    form.num_vars     = 1;
    form.monomials    = { 1 }; // just x
    auto expr         = EmitRawAnf(form);
    EXPECT_EQ(expr->kind, Expr::Kind::kVariable);
    EXPECT_EQ(expr->var_index, 0u);
}

TEST(AnfFormTest, RawEmit_TwoVarXor) {
    AnfForm form;
    form.constant_bit = 0;
    form.num_vars     = 2;
    form.monomials    = { 1, 2 }; // x ^ y
    auto expr         = EmitRawAnf(form);
    EXPECT_EQ(expr->kind, Expr::Kind::kXor);
}

TEST(AnfFormTest, RawEmit_ConstantOnePlusMonomials) {
    AnfForm form;
    form.constant_bit = 1;
    form.num_vars     = 2;
    form.monomials    = { 1 }; // 1 ^ x
    auto expr         = EmitRawAnf(form);
    EXPECT_EQ(expr->kind, Expr::Kind::kXor);
}

TEST(ExprCostTest, ConstantCost) {
    auto e = Expr::Constant(42);
    EXPECT_EQ(ExprCost(*e), 1u);
}

TEST(ExprCostTest, VariableCost) {
    auto e = Expr::Variable(0);
    EXPECT_EQ(ExprCost(*e), 1u);
}

TEST(ExprCostTest, NotCost) {
    auto e = Expr::BitwiseNot(Expr::Variable(0));
    EXPECT_EQ(ExprCost(*e), 2u);
}

TEST(ExprCostTest, BinaryOpCost) {
    auto e = Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1));
    EXPECT_EQ(ExprCost(*e), 3u);
}

TEST(ExprCostTest, NestedTreeCost) {
    // (x & y) ^ z  => Xor(And(x, y), z) => 1 + (1+1+1) + 1 = 5
    auto e = Expr::BitwiseXor(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2)
    );
    EXPECT_EQ(ExprCost(*e), 5u);
}

TEST(ExprCostTest, OrChainBonus) {
    // x | y => Or(x, y) => 3 (no bonus for 2-var)
    auto e2 = Expr::BitwiseOr(Expr::Variable(0), Expr::Variable(1));
    EXPECT_EQ(ExprCost(*e2), 3u);
    // x | y | z => Or(Or(x,y), z) => 5 - 1 = 4 (bonus for 3+)
    auto e3 = Expr::BitwiseOr(
        Expr::BitwiseOr(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2)
    );
    EXPECT_EQ(ExprCost(*e3), 4u);
}

TEST(OrRecognizerTest, TwoVarOr) {
    // x ^ y ^ xy → x | y
    AnfForm form;
    form.constant_bit = 0;
    form.num_vars     = 2;
    form.monomials    = { 1, 2, 3 };
    auto expr         = CleanupAnf(form);
    EXPECT_EQ(expr->kind, Expr::Kind::kOr);
}

TEST(OrRecognizerTest, ThreeVarOr) {
    // all nonempty subsets of {a,b,c} → a | b | c
    AnfForm form;
    form.constant_bit           = 0;
    form.num_vars               = 3;
    form.monomials              = { 1, 2, 4, 3, 5, 6, 7 };
    auto expr                   = CleanupAnf(form);
    // Should be an OR tree, verify semantically
    std::vector< uint64_t > sig = { 0, 1, 1, 1, 1, 1, 1, 1 };
    auto check                  = SignatureCheck(sig, *expr, 3, 64);
    EXPECT_TRUE(check.passed);
    EXPECT_LE(ExprCost(*expr), 5u);
}

TEST(OrRecognizerTest, FourVarOr) {
    // all 15 nonempty subsets of {a,b,c,d} → a | b | c | d
    AnfForm form;
    form.constant_bit = 0;
    form.num_vars     = 4;
    // Must be sorted by monomial_less: degree 1 first, then 2, etc.
    form.monomials    = { 1, 2, 4, 8, 3, 5, 6, 9, 10, 12, 7, 11, 13, 14, 15 };
    auto expr         = CleanupAnf(form);
    std::vector< uint64_t > sig(16, 1);
    sig[0]     = 0;
    auto check = SignatureCheck(sig, *expr, 4, 64);
    EXPECT_TRUE(check.passed);
}

TEST(OrRecognizerTest, ComplementedTwoVarNor) {
    // 1 ^ x ^ y ^ xy → 1 ^ (x | y)
    AnfForm form;
    form.constant_bit = 1;
    form.num_vars     = 2;
    form.monomials    = { 1, 2, 3 };
    auto expr         = CleanupAnf(form);
    EXPECT_EQ(expr->kind, Expr::Kind::kXor);
    std::vector< uint64_t > sig = { 1, 0, 0, 0 };
    auto check                  = SignatureCheck(sig, *expr, 2, 1);
    EXPECT_TRUE(check.passed);
}

TEST(OrRecognizerTest, ComplementedThreeVarNor) {
    // 1 ^ all nonempty subsets of {a,b,c} → 1 ^ (a | b | c)
    AnfForm form;
    form.constant_bit = 1;
    form.num_vars     = 3;
    form.monomials    = { 1, 2, 4, 3, 5, 6, 7 };
    auto expr         = CleanupAnf(form);
    EXPECT_EQ(expr->kind, Expr::Kind::kXor);
}

TEST(OrRecognizerTest, PartialSubsetNotOr) {
    // x ^ y (no xy term) — NOT an OR pattern, stays as XOR
    AnfForm form;
    form.constant_bit = 0;
    form.num_vars     = 2;
    form.monomials    = { 1, 2 };
    auto expr         = CleanupAnf(form);
    EXPECT_EQ(expr->kind, Expr::Kind::kXor);
}

TEST(OrRecognizerTest, SubsetOrOverTwoOfThreeVars) {
    // x ^ z ^ xz → x | z (over 3-var space, but OR uses only 2)
    AnfForm form;
    form.constant_bit           = 0;
    form.num_vars               = 3;
    form.monomials              = { 1, 4, 5 }; // x=1, z=4, xz=5
    auto expr                   = CleanupAnf(form);
    // sig[i] = x|z where x=bit0, y=bit1, z=bit2
    // i=0(000):0 i=1(001):1 i=2(010):0 i=3(011):1
    // i=4(100):1 i=5(101):1 i=6(110):1 i=7(111):1
    std::vector< uint64_t > sig = { 0, 1, 0, 1, 1, 1, 1, 1 };
    auto check                  = SignatureCheck(sig, *expr, 3, 64);
    EXPECT_TRUE(check.passed);
}

TEST(CommonCubeTest, SimpleFactorAB_AC) {
    // ab ^ ac → a & (b ^ c)
    AnfForm form;
    form.constant_bit           = 0;
    form.num_vars               = 3;
    form.monomials              = { 3, 5 }; // ab=3(011), ac=5(101)
    auto expr                   = CleanupAnf(form);
    // Verify semantics: sig[i] for ab^ac
    std::vector< uint64_t > sig = { 0, 0, 0, 1, 0, 1, 0, 0 };
    auto check                  = SignatureCheck(sig, *expr, 3, 1);
    EXPECT_TRUE(check.passed);
    // Should be cheaper than raw: a&(b^c)=5 vs (a&b)^(a&c)=7
    auto raw = EmitRawAnf(form);
    EXPECT_LT(ExprCost(*expr), ExprCost(*raw));
}

TEST(CommonCubeTest, ThreeTermFactor) {
    // ab ^ ac ^ ad → a & (b ^ c ^ d)
    AnfForm form;
    form.constant_bit = 0;
    form.num_vars     = 4;
    form.monomials    = { 3, 5, 9 }; // ab=3, ac=5, ad=9
    auto expr         = CleanupAnf(form);
    auto raw          = EmitRawAnf(form);
    EXPECT_LT(ExprCost(*expr), ExprCost(*raw));
}

TEST(CommonCubeTest, TwoVarCommonFactor) {
    // abc ^ abd → ab & (c ^ d)
    AnfForm form;
    form.constant_bit = 0;
    form.num_vars     = 4;
    form.monomials    = { 7, 11 }; // abc=0111=7, abd=1011=11
    auto expr         = CleanupAnf(form);
    auto raw          = EmitRawAnf(form);
    EXPECT_LT(ExprCost(*expr), ExprCost(*raw));
}

TEST(CommonCubeTest, NoCommonFactor) {
    // ab ^ cd — no shared variables, stays as-is
    AnfForm form;
    form.constant_bit = 0;
    form.num_vars     = 4;
    form.monomials    = { 3, 12 }; // ab=0011=3, cd=1100=12
    auto expr         = CleanupAnf(form);
    auto raw          = EmitRawAnf(form);
    EXPECT_EQ(ExprCost(*expr), ExprCost(*raw));
}

TEST(CommonCubeTest, FactorPlusOrRecognition) {
    // ab ^ ac ^ abc → factor a → inner {b,c,bc} → a & (b|c)
    AnfForm form;
    form.constant_bit           = 0;
    form.num_vars               = 3;
    form.monomials              = { 3, 5, 7 }; // ab=3, ac=5, abc=7
    auto expr                   = CleanupAnf(form);
    // sig: ab^ac^abc where a=bit0, b=bit1, c=bit2
    std::vector< uint64_t > sig = { 0, 0, 0, 1, 0, 1, 0, 1 };
    auto check                  = SignatureCheck(sig, *expr, 3, 1);
    EXPECT_TRUE(check.passed);
    // a & (b|c) = And(a, Or(b,c)) = 5 nodes
    EXPECT_LE(ExprCost(*expr), 5u);
}

TEST(AbsorptionTest, SimpleAbsorption_A_AB) {
    // a ^ ab → a & ~b
    AnfForm form;
    form.constant_bit           = 0;
    form.num_vars               = 2;
    form.monomials              = { 1, 3 }; // a=1, ab=3
    auto expr                   = CleanupAnf(form);
    std::vector< uint64_t > sig = { 0, 1, 0, 0 };
    auto check                  = SignatureCheck(sig, *expr, 2, 1);
    EXPECT_TRUE(check.passed);
    // a & ~b = And(a, Not(b)) = 4 nodes
    EXPECT_LE(ExprCost(*expr), 4u);
}

TEST(AbsorptionTest, HigherDegree_AB_ABC) {
    // ab ^ abc → ab & ~c
    AnfForm form;
    form.constant_bit           = 0;
    form.num_vars               = 3;
    form.monomials              = { 3, 7 }; // ab=3, abc=7
    auto expr                   = CleanupAnf(form);
    std::vector< uint64_t > sig = { 0, 0, 0, 1, 0, 0, 0, 0 };
    auto check                  = SignatureCheck(sig, *expr, 3, 1);
    EXPECT_TRUE(check.passed);
    // ab & ~c = And(And(a,b), Not(c)) = 6 nodes
    EXPECT_LE(ExprCost(*expr), 6u);
}

TEST(AbsorptionTest, ABC_ABCD) {
    // abc ^ abcd → abc & ~d
    AnfForm form;
    form.constant_bit = 0;
    form.num_vars     = 4;
    form.monomials    = { 7, 15 }; // abc=7, abcd=15
    auto expr         = CleanupAnf(form);
    auto raw          = EmitRawAnf(form);
    EXPECT_LT(ExprCost(*expr), ExprCost(*raw));
}

TEST(AbsorptionTest, NoAbsorption_AB_CD) {
    // ab ^ cd — no containment, stays raw
    AnfForm form;
    form.constant_bit = 0;
    form.num_vars     = 4;
    form.monomials    = { 3, 12 };
    auto expr         = CleanupAnf(form);
    auto raw          = EmitRawAnf(form);
    EXPECT_EQ(ExprCost(*expr), ExprCost(*raw));
}

// --- Combined rewrite tests ---

TEST(CombinedRewriteTest, FactorThenOr_AB_AC_ABC) {
    // ab ^ ac ^ abc → factor a → a & (b ^ c ^ bc) → a & (b|c)
    AnfForm form;
    form.constant_bit           = 0;
    form.num_vars               = 3;
    form.monomials              = { 3, 5, 7 }; // ab=3, ac=5, abc=7
    auto expr                   = CleanupAnf(form);
    // sig[i] = (a&b)^(a&c)^(a&b&c) where a=bit0, b=bit1, c=bit2
    // i=0(000):0 i=1(001):0 i=2(010):0 i=3(011):1
    // i=4(100):0 i=5(101):1 i=6(110):0 i=7(111):1
    std::vector< uint64_t > sig = { 0, 0, 0, 1, 0, 1, 0, 1 };
    auto check                  = SignatureCheck(sig, *expr, 3, 1);
    EXPECT_TRUE(check.passed);
}

TEST(CombinedRewriteTest, AB_AC_BC_ABC) {
    // ab ^ ac ^ bc ^ abc — multiple factoring options
    AnfForm form;
    form.constant_bit           = 0;
    form.num_vars               = 3;
    form.monomials              = { 3, 5, 6, 7 }; // ab=3, ac=5, bc=6, abc=7
    auto expr                   = CleanupAnf(form);
    // Compute expected sig by evaluating XOR of monomials
    // i=0(000):0 i=1(001):0 i=2(010):0 i=3(011):1
    // i=4(100):0 i=5(101):1 i=6(110):1 i=7(111):0
    std::vector< uint64_t > sig = { 0, 0, 0, 1, 0, 1, 1, 0 };
    auto check                  = SignatureCheck(sig, *expr, 3, 1);
    EXPECT_TRUE(check.passed);
    auto raw = EmitRawAnf(form);
    EXPECT_LE(ExprCost(*expr), ExprCost(*raw));
}

// --- Partial OR recognition tests ---

TEST(PartialOrTest, TwoVarOrPlusRemainder) {
    // a ^ b ^ ab ^ c → (a|b) ^ c
    AnfForm form;
    form.constant_bit           = 0;
    form.num_vars               = 3;
    form.monomials              = { 1, 2, 4, 3 }; // a, b, c, ab
    auto expr                   = CleanupAnf(form);
    std::vector< uint64_t > sig = { 0, 1, 1, 1, 1, 0, 0, 0 };
    auto check                  = SignatureCheck(sig, *expr, 3, 1);
    EXPECT_TRUE(check.passed);
    auto raw = EmitRawAnf(form);
    EXPECT_LT(ExprCost(*expr), ExprCost(*raw));
}

TEST(PartialOrTest, ThreeVarOrPlusRemainder) {
    // (a|b|c) ^ d: all nonempty subsets of {a,b,c} plus d
    AnfForm form;
    form.constant_bit = 0;
    form.num_vars     = 4;
    form.monomials    = { 1, 2, 4, 8, 3, 5, 6, 7 };
    auto expr         = CleanupAnf(form);
    // sig: (a|b|c) ^ d
    std::vector< uint64_t > sig(16);
    for (uint32_t i = 0; i < 16; ++i) {
        uint32_t a = i & 1, b = (i >> 1) & 1;
        uint32_t c = (i >> 2) & 1, d = (i >> 3) & 1;
        sig[i] = (a | b | c) ^ d;
    }
    auto check = SignatureCheck(sig, *expr, 4, 1);
    EXPECT_TRUE(check.passed);
    auto raw = EmitRawAnf(form);
    EXPECT_LT(ExprCost(*expr), ExprCost(*raw));
}

TEST(PartialOrTest, TwoDisjointOrs) {
    // (a|b) ^ (c|d): {a,b,ab,c,d,cd}
    AnfForm form;
    form.constant_bit = 0;
    form.num_vars     = 4;
    form.monomials    = { 1, 2, 4, 8, 3, 12 };
    auto expr         = CleanupAnf(form);
    std::vector< uint64_t > sig(16);
    for (uint32_t i = 0; i < 16; ++i) {
        uint32_t a = i & 1, b = (i >> 1) & 1;
        uint32_t c = (i >> 2) & 1, d = (i >> 3) & 1;
        sig[i] = (a | b) ^ (c | d);
    }
    auto check = SignatureCheck(sig, *expr, 4, 1);
    EXPECT_TRUE(check.passed);
    auto raw = EmitRawAnf(form);
    EXPECT_LT(ExprCost(*expr), ExprCost(*raw));
}

TEST(PartialOrTest, WithConstant) {
    // 1 ^ a ^ b ^ ab ^ c → (a|b) ^ (1^c)
    AnfForm form;
    form.constant_bit = 1;
    form.num_vars     = 3;
    form.monomials    = { 1, 2, 4, 3 };
    auto expr         = CleanupAnf(form);
    std::vector< uint64_t > sig(8);
    for (uint32_t i = 0; i < 8; ++i) {
        uint32_t a = i & 1, b = (i >> 1) & 1, c = (i >> 2) & 1;
        sig[i] = 1 ^ a ^ b ^ (a & b) ^ c;
    }
    auto check = SignatureCheck(sig, *expr, 3, 1);
    EXPECT_TRUE(check.passed);
    auto raw = EmitRawAnf(form);
    EXPECT_LT(ExprCost(*expr), ExprCost(*raw));
}

TEST(PartialOrTest, OrPlusAbsorption) {
    // a ^ b ^ ab ^ c ^ ac → (a|b) ^ (c & ~a)
    AnfForm form;
    form.constant_bit = 0;
    form.num_vars     = 3;
    form.monomials    = { 1, 2, 4, 3, 5 }; // a,b,c,ab,ac
    auto expr         = CleanupAnf(form);
    std::vector< uint64_t > sig(8);
    for (uint32_t i = 0; i < 8; ++i) {
        uint32_t a = i & 1, b = (i >> 1) & 1, c = (i >> 2) & 1;
        sig[i] = a ^ b ^ (a & b) ^ c ^ (a & c);
    }
    auto check = SignatureCheck(sig, *expr, 3, 1);
    EXPECT_TRUE(check.passed);
    auto raw = EmitRawAnf(form);
    EXPECT_LT(ExprCost(*expr), ExprCost(*raw));
}

TEST(PartialOrTest, NoPartialOr) {
    // ab ^ cd ^ ac — no OR family among these
    AnfForm form;
    form.constant_bit = 0;
    form.num_vars     = 4;
    form.monomials    = { 3, 5, 12 }; // ab, ac, cd
    auto expr         = CleanupAnf(form);
    auto raw          = EmitRawAnf(form);
    // Factoring may still help (factor a from ab,ac)
    EXPECT_LE(ExprCost(*expr), ExprCost(*raw));
}

// --- Negative tests: should stay as raw ANF ---

TEST(NegativeTest, PureXor) {
    // a ^ b ^ c — no structure to exploit
    AnfForm form;
    form.constant_bit = 0;
    form.num_vars     = 3;
    form.monomials    = { 1, 2, 4 };
    auto expr         = CleanupAnf(form);
    auto raw          = EmitRawAnf(form);
    EXPECT_EQ(ExprCost(*expr), ExprCost(*raw));
}

TEST(NegativeTest, DisjointProducts) {
    // ab ^ cd — no common factor
    AnfForm form;
    form.constant_bit = 0;
    form.num_vars     = 4;
    form.monomials    = { 3, 12 };
    auto expr         = CleanupAnf(form);
    auto raw          = EmitRawAnf(form);
    EXPECT_EQ(ExprCost(*expr), ExprCost(*raw));
}

// --- Semantic exhaustive tests ---

TEST(SemanticExhaustiveTest, AllBoolean2Var) {
    for (uint32_t f = 0; f < 16; ++f) {
        std::vector< uint64_t > sig(4);
        for (uint32_t i = 0; i < 4; ++i) {
            sig[i] = (f >> i) & 1;
        }
        auto anf   = ComputeAnf(sig, 2);
        auto form  = AnfForm::FromAnfCoeffs(anf, 2);
        auto expr  = CleanupAnf(form);
        auto check = SignatureCheck(sig, *expr, 2, 1);
        EXPECT_TRUE(check.passed) << "Failed for 2-var function " << f;
    }
}

TEST(SemanticExhaustiveTest, AllBoolean3Var) {
    for (uint32_t f = 0; f < 256; ++f) {
        std::vector< uint64_t > sig(8);
        for (uint32_t i = 0; i < 8; ++i) {
            sig[i] = (f >> i) & 1;
        }
        auto anf   = ComputeAnf(sig, 3);
        auto form  = AnfForm::FromAnfCoeffs(anf, 3);
        auto expr  = CleanupAnf(form);
        auto check = SignatureCheck(sig, *expr, 3, 1);
        EXPECT_TRUE(check.passed) << "Failed for 3-var function " << f;
    }
}

TEST(SemanticExhaustiveTest, AllBoolean4Var) {
    for (uint32_t f = 0; f < 65536; ++f) {
        std::vector< uint64_t > sig(16);
        for (uint32_t i = 0; i < 16; ++i) {
            sig[i] = (f >> i) & 1;
        }
        auto anf   = ComputeAnf(sig, 4);
        auto form  = AnfForm::FromAnfCoeffs(anf, 4);
        auto expr  = CleanupAnf(form);
        auto check = SignatureCheck(sig, *expr, 4, 1);
        EXPECT_TRUE(check.passed) << "Failed for 4-var function " << f;
    }
}
