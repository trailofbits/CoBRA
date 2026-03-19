#include "cobra/core/BasisTransform.h"
#include "cobra/core/BitWidth.h"
#include <gtest/gtest.h>

using namespace cobra;

namespace {

    ExponentTuple make_exp(std::initializer_list< uint8_t > exps) {
        auto data = exps.begin();
        return ExponentTuple::FromExponents(data, static_cast< uint8_t >(exps.size()));
    }

} // namespace

TEST(BasisTransformTest, EmptyMapRoundtrips) {
    CoeffMap empty;
    auto f = ToFactorialBasis(empty, 2, 8);
    EXPECT_TRUE(f.empty());
    auto m = ToMonomialBasis(empty, 2, 8);
    EXPECT_TRUE(m.empty());
}

TEST(BasisTransformTest, LinearTermUnchanged) {
    // x_(1) = x in both bases (F_3 and C_3 are identity on degree <= 1)
    CoeffMap input = {
        { make_exp({ 1 }), 5 }
    };
    auto f = ToFactorialBasis(input, 1, 8);
    EXPECT_EQ(f.size(), 1u);
    EXPECT_EQ(f.at(make_exp({ 1 })), 5u);

    auto m = ToMonomialBasis(input, 1, 8);
    EXPECT_EQ(m.size(), 1u);
    EXPECT_EQ(m.at(make_exp({ 1 })), 5u);
}

TEST(BasisTransformTest, XSquaredToFactorial) {
    // x^2 in monomial -> x_(1) + x_(2) in factorial
    // F_3 column 2: [0, 1, 1] -> contributes c to both rows 1 and 2
    CoeffMap input = {
        { make_exp({ 2 }), 1 }
    };
    auto f = ToFactorialBasis(input, 1, 8);
    EXPECT_EQ(f.size(), 2u);
    EXPECT_EQ(f.at(make_exp({ 1 })), 1u);
    EXPECT_EQ(f.at(make_exp({ 2 })), 1u);
}

TEST(BasisTransformTest, XSquaredFactorialToMonomial) {
    // x_(2) in factorial -> x^2 - x in monomial
    // C_3 column 2: [0, -1, 1] -> -c to row 1, +c to row 2
    // At w=8: -1 mod 256 = 255
    CoeffMap input = {
        { make_exp({ 2 }), 1 }
    };
    auto m = ToMonomialBasis(input, 1, 8);
    EXPECT_EQ(m.size(), 2u);
    EXPECT_EQ(m.at(make_exp({ 1 })), 255u); // ModNeg(1, 8) = 255
    EXPECT_EQ(m.at(make_exp({ 2 })), 1u);
}

TEST(BasisTransformTest, BitwidthSensitivity) {
    // Same factorial input {x_(2): 1} at different bitwidths
    // w=8:  degree-1 coeff = 2^8 - 1 = 255
    // w=64: degree-1 coeff = 2^64 - 1 = UINT64_MAX
    CoeffMap input = {
        { make_exp({ 2 }), 1 }
    };

    auto m8 = ToMonomialBasis(input, 1, 8);
    EXPECT_EQ(m8.at(make_exp({ 1 })), 255u);

    auto m64 = ToMonomialBasis(input, 1, 64);
    EXPECT_EQ(m64.at(make_exp({ 1 })), UINT64_MAX);
}

TEST(BasisTransformTest, RoundtripMonomialToFactorialToMonomial) {
    // Start with monomial map, transform to factorial, back to monomial.
    // Should recover original.
    CoeffMap original = {
        { make_exp({ 1 }), 3 },
        { make_exp({ 2 }), 7 }
    };
    auto f = ToFactorialBasis(original, 1, 16);
    auto m = ToMonomialBasis(f, 1, 16);
    EXPECT_EQ(m.size(), original.size());
    for (const auto &[k, v] : original) {
        ASSERT_TRUE(m.count(k)) << "Missing tuple in roundtrip";
        EXPECT_EQ(m.at(k), v);
    }
}

TEST(BasisTransformTest, RoundtripFactorialToMonomialToFactorial) {
    // Start with factorial map, transform to monomial, back to factorial.
    CoeffMap original = {
        { make_exp({ 1 }), 10 },
        { make_exp({ 2 }),  4 }
    };
    auto m = ToMonomialBasis(original, 1, 16);
    auto f = ToFactorialBasis(m, 1, 16);
    EXPECT_EQ(f.size(), original.size());
    for (const auto &[k, v] : original) {
        ASSERT_TRUE(f.count(k)) << "Missing tuple in roundtrip";
        EXPECT_EQ(f.at(k), v);
    }
}

TEST(BasisTransformTest, MultivariateXSquaredY) {
    // x^2 * y in monomial -> F_3 along var 0 splits x^2,
    // var 1 leaves y unchanged.
    // Result: x_(1)*y_(1) + x_(2)*y_(1)
    CoeffMap input = {
        { make_exp({ 2, 1 }), 1 }
    };
    auto f = ToFactorialBasis(input, 2, 8);
    EXPECT_EQ(f.size(), 2u);
    EXPECT_EQ(f.at(make_exp({ 1, 1 })), 1u);
    EXPECT_EQ(f.at(make_exp({ 2, 1 })), 1u);
}

TEST(BasisTransformTest, MultivariateRoundtrip) {
    // Multivariate roundtrip: x^2*y + 3*x*y
    CoeffMap original = {
        { make_exp({ 2, 1 }), 1 },
        { make_exp({ 1, 1 }), 3 }
    };
    auto f = ToFactorialBasis(original, 2, 16);
    auto m = ToMonomialBasis(f, 2, 16);
    EXPECT_EQ(m.size(), original.size());
    for (const auto &[k, v] : original) {
        ASSERT_TRUE(m.count(k)) << "Missing tuple in multivariate roundtrip";
        EXPECT_EQ(m.at(k), v);
    }
}

TEST(BasisTransformTest, CrossCheckKnownValues) {
    // Hand-computed: 5*x^2 + 3*x at w=8.
    // Monomial: {(2):5, (1):3}.
    // F_3 on x^2: contributes 5 to row 1 and 5 to row 2.
    // F_3 on x:   contributes 3 to row 1.
    // Factorial: {(1): (3+5)=8, (2): 5}.
    CoeffMap input = {
        { make_exp({ 2 }), 5 },
        { make_exp({ 1 }), 3 }
    };
    auto f = ToFactorialBasis(input, 1, 8);
    EXPECT_EQ(f.size(), 2u);
    EXPECT_EQ(f.at(make_exp({ 1 })), 8u);
    EXPECT_EQ(f.at(make_exp({ 2 })), 5u);

    // Reverse: factorial {(1):8, (2):5} -> monomial.
    // C_3 on (2):5: contributes -5 to row 1 and 5 to row 2.
    // C_3 on (1):8: contributes 8 to row 1.
    // Monomial: {(1): (8 + (-5 mod 256)) = (8+251)=259 mod 256 = 3, (2): 5}.
    auto m = ToMonomialBasis(f, 1, 8);
    EXPECT_EQ(m.size(), 2u);
    EXPECT_EQ(m.at(make_exp({ 1 })), 3u);
    EXPECT_EQ(m.at(make_exp({ 2 })), 5u);
}
