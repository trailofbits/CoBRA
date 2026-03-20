#include "cobra/core/PolyIR.h"
#include "cobra/core/PolyNormalizer.h"
#include <gtest/gtest.h>

using namespace cobra;

namespace {

    MonomialKey make_exp(std::initializer_list< uint8_t > exps) {
        auto data = exps.begin();
        return MonomialKey::FromExponents(data, static_cast< uint8_t >(exps.size()));
    }

} // namespace

// --- NormalizedPoly validator tests ---

TEST(NormalizedPolyValidatorTest, EmptyIsValid) {
    NormalizedPoly np{ 2, 8, {} };
    EXPECT_TRUE(np.IsValid());
}

TEST(NormalizedPolyValidatorTest, SimpleLinearValid) {
    NormalizedPoly np{ 1, 8, { { make_exp({ 1 }), 5 } } };
    EXPECT_TRUE(np.IsValid());
}

TEST(NormalizedPolyValidatorTest, CoefficientExceedsBound) {
    // exp=(2): one two, bound = 2^{8-1} = 128. Coeff 200 > 128.
    NormalizedPoly np{ 1, 8, { { make_exp({ 2 }), 200 } } };
    EXPECT_FALSE(np.IsValid());
}

TEST(NormalizedPolyValidatorTest, ZeroEntryInvalid) {
    NormalizedPoly np{ 1, 8, { { make_exp({ 1 }), 0 } } };
    EXPECT_FALSE(np.IsValid());
}

TEST(NormalizedPolyValidatorTest, TwoSquaresBitwidth2) {
    // Two exponent-2 coords at w=2: bound = 2^{2-2} = 1.
    // Any nonzero coefficient is invalid.
    NormalizedPoly np{ 2, 2, { { make_exp({ 2, 2 }), 1 } } };
    EXPECT_FALSE(np.IsValid());
}

TEST(NormalizedPolyValidatorTest, BitwidthTooLow) {
    NormalizedPoly np{ 1, 1, { { make_exp({ 1 }), 1 } } };
    EXPECT_FALSE(np.IsValid());
}

TEST(NormalizedPolyValidatorTest, Equality) {
    NormalizedPoly a{
        2, 8, { { make_exp({ 1, 0 }), 3 }, { make_exp({ 0, 1 }), 5 } }
    };
    NormalizedPoly b{
        2, 8, { { make_exp({ 1, 0 }), 3 }, { make_exp({ 0, 1 }), 5 } }
    };
    EXPECT_EQ(a, b);
}

TEST(NormalizedPolyValidatorTest, InequalityDifferentCoeff) {
    NormalizedPoly a{ 2, 8, { { make_exp({ 1, 0 }), 3 } } };
    NormalizedPoly b{ 2, 8, { { make_exp({ 1, 0 }), 4 } } };
    EXPECT_NE(a, b);
}

TEST(NormalizedPolyValidatorTest, InequalityDifferentBitwidth) {
    NormalizedPoly a{ 1, 8, { { make_exp({ 1 }), 1 } } };
    NormalizedPoly b{ 1, 16, { { make_exp({ 1 }), 1 } } };
    EXPECT_NE(a, b);
}

// --- Normalization correctness tests ---

TEST(PolyNormalizerTest, IdentityX) {
    // x -> x_(1)
    PolyIR poly{ 1, 8, { { make_exp({ 1 }), 1 } } };
    auto np = NormalizePolynomial(poly);
    EXPECT_TRUE(np.IsValid());
    EXPECT_EQ(np.coeffs.size(), 1u);
    EXPECT_EQ(np.coeffs.at(make_exp({ 1 })), 1u);
}

TEST(PolyNormalizerTest, XSquared) {
    // x^2 -> x_(1) + x_(2)
    PolyIR poly{ 1, 8, { { make_exp({ 2 }), 1 } } };
    auto np = NormalizePolynomial(poly);
    EXPECT_TRUE(np.IsValid());
    EXPECT_EQ(np.coeffs.size(), 2u);
    EXPECT_EQ(np.coeffs.at(make_exp({ 1 })), 1u);
    EXPECT_EQ(np.coeffs.at(make_exp({ 2 })), 1u);
}

TEST(PolyNormalizerTest, NullPolynomialW3) {
    // 4*x^2 + 4*x at w=3 -> zero
    // (because 4*x_(2) is null mod 8)
    PolyIR poly{
        1, 3, { { make_exp({ 2 }), 4 }, { make_exp({ 1 }), 4 } }
    };
    auto np = NormalizePolynomial(poly);
    EXPECT_TRUE(np.IsValid());
    EXPECT_TRUE(np.coeffs.empty());
}

TEST(PolyNormalizerTest, EquivalenceUnderNull) {
    // x^2 and 5*x^2 + 4*x at w=3 normalize identically
    PolyIR p1{ 1, 3, { { make_exp({ 2 }), 1 } } };
    PolyIR p2{
        1, 3, { { make_exp({ 2 }), 5 }, { make_exp({ 1 }), 4 } }
    };
    auto n1 = NormalizePolynomial(p1);
    auto n2 = NormalizePolynomial(p2);
    EXPECT_EQ(n1, n2);
}

TEST(PolyNormalizerTest, MultivariateXY) {
    // x*y -> tuple (1,1), no reduction needed
    PolyIR poly{ 2, 8, { { make_exp({ 1, 1 }), 3 } } };
    auto np = NormalizePolynomial(poly);
    EXPECT_TRUE(np.IsValid());
    EXPECT_EQ(np.coeffs.size(), 1u);
    EXPECT_EQ(np.coeffs.at(make_exp({ 1, 1 })), 3u);
}

TEST(PolyNormalizerTest, MultivariateXSquaredY) {
    // x^2 * y -> x_(1)*y_(1) + x_(2)*y_(1)
    PolyIR poly{ 2, 8, { { make_exp({ 2, 1 }), 1 } } };
    auto np = NormalizePolynomial(poly);
    EXPECT_TRUE(np.IsValid());
    EXPECT_EQ(np.coeffs.size(), 2u);
    EXPECT_EQ(np.coeffs.at(make_exp({ 1, 1 })), 1u);
    EXPECT_EQ(np.coeffs.at(make_exp({ 2, 1 })), 1u);
}

TEST(PolyNormalizerTest, Idempotency) {
    // normalize(normalize(p)) == normalize(p)
    // Treat factorial-basis output as new monomial input.
    // Use multivariate input where all output exponents are
    // degree <= 1 (F_3 is identity on these), so the output
    // is a fixed point of re-normalization.
    // Note: outputs with degree-2 terms are NOT fixed points
    // because F_3(x^2) = x_(1) + x_(2), which shifts the
    // degree-1 coefficient on each application.
    PolyIR poly{
        2,
        8,
        { { make_exp({ 1, 0 }), 5 }, { make_exp({ 0, 1 }), 3 }, { make_exp({ 1, 1 }), 7 } }
    };
    auto n1 = NormalizePolynomial(poly);
    EXPECT_TRUE(n1.IsValid());

    // Build PolyIR from n1's coefficients (treating
    // factorial-basis values as monomial input)
    PolyIR as_monomial{ n1.num_vars, n1.bitwidth, {} };
    for (const auto &[k, v] : n1.coeffs) { as_monomial.terms[k] = v; }
    auto n2 = NormalizePolynomial(as_monomial);
    EXPECT_EQ(n1, n2);
}

TEST(PolyNormalizerTest, CanonicalBoundEnforced) {
    // At w=4, exp=(2) has 1 two -> bound = 2^{4-1} = 8.
    // Input: 12*x^2. After F_3: exp=1 gets 12, exp=2 gets 12.
    // Reduction: exp=1 (0 twos) -> mod 16 -> 12.
    //            exp=2 (1 two)  -> mod 8  -> 4.
    PolyIR poly{ 1, 4, { { make_exp({ 2 }), 12 } } };
    auto np = NormalizePolynomial(poly);
    EXPECT_TRUE(np.IsValid());
    EXPECT_EQ(np.coeffs.at(make_exp({ 1 })), 12u);
    EXPECT_EQ(np.coeffs.at(make_exp({ 2 })), 4u);
}

TEST(PolyNormalizerTest, Bitwidth2TwoSquares) {
    // At w=2: two squared vars -> bound is 2^{2-2}=1,
    // so coefficient forced to zero
    PolyIR poly{ 2, 2, { { make_exp({ 2, 2 }), 1 } } };
    auto np = NormalizePolynomial(poly);
    EXPECT_TRUE(np.IsValid());
    EXPECT_EQ(np.coeffs.count(make_exp({ 2, 2 })), 0u);
}

TEST(PolyNormalizerTest, EmptyInput) {
    PolyIR poly{ 2, 8, {} };
    auto np = NormalizePolynomial(poly);
    EXPECT_TRUE(np.IsValid());
    EXPECT_TRUE(np.coeffs.empty());
}

TEST(PolyNormalizerTest, CubicTerm) {
    // x^3 in monomial. S(3,j) distributes to factorial.
    // Factorial should have entries for exp 1, 2, 3.
    PolyIR poly{ 1, 64, { { make_exp({ 3 }), 1 } } };
    auto np = NormalizePolynomial(poly);
    EXPECT_TRUE(np.IsValid());
    EXPECT_FALSE(np.coeffs.empty());
}

TEST(PolyNormalizerTest, Degree4_NullSpaceErase) {
    // At w=4: v_2(4!) = 3. For exp=(4): q=3 >= w=4? No, 3 < 4.
    // But bound_bits = 4 - 3 = 1, so coefficient must be 0 or 1.
    // At w=3: q=3 >= w=3, so the term is null and erased.
    PolyIR poly{ 1, 3, { { make_exp({ 4 }), 1 } } };
    auto np = NormalizePolynomial(poly);
    EXPECT_TRUE(np.IsValid());
    // All degree-4 terms should be erased at w=3
    EXPECT_EQ(np.coeffs.count(make_exp({ 4 })), 0u);
}

TEST(NormalizedPolyValidatorTest, Degree3Valid) {
    // exp=(3): v_2(3!) = 1, bound = 2^{64-1} = huge. Any coefficient valid.
    NormalizedPoly np{ 1, 64, { { make_exp({ 3 }), 42 } } };
    EXPECT_TRUE(np.IsValid());
}

TEST(NormalizedPolyValidatorTest, Degree4_CoefficientBound) {
    // exp=(4): v_2(4!) = 3, bound = 2^{8-3} = 32. Coeff 50 > 32 => invalid.
    NormalizedPoly np{ 1, 8, { { make_exp({ 4 }), 50 } } };
    EXPECT_FALSE(np.IsValid());
    // Coeff 31 <= 32 => valid.
    NormalizedPoly np2{ 1, 8, { { make_exp({ 4 }), 31 } } };
    EXPECT_TRUE(np2.IsValid());
}
