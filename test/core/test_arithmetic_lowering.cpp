#include "cobra/core/ArithmeticLowering.h"
#include <gtest/gtest.h>

using namespace cobra;

namespace {

    MonomialKey make_exp(std::initializer_list< uint8_t > exps) {
        auto data = exps.begin();
        return MonomialKey::FromExponents(data, static_cast< uint8_t >(exps.size()));
    }

} // namespace

TEST(ArithmeticLoweringTest, SingletonLinearFolded) {
    // 2 vars: and_c[1]=5 (x_0 linear), rest zero
    std::vector< Coeff > and_c = { 0, 5, 0, 0 };
    std::vector< Coeff > mul_c = { 0, 0, 0, 0 };
    auto result                = LowerArithmeticFragment(and_c, mul_c, 2, 64);
    ASSERT_TRUE(result.has_value());
    auto &r = result.value();

    EXPECT_EQ(r.poly.terms.count(make_exp({ 1, 0 })), 1u);
    EXPECT_EQ(r.poly.terms.at(make_exp({ 1, 0 })), 5u);
    EXPECT_EQ(r.residual_and_coeffs[1], 0u); // zeroed
    EXPECT_EQ(r.residual_and_coeffs[0], 0u); // constant preserved
}

TEST(ArithmeticLoweringTest, SingletonSquareFolded) {
    std::vector< Coeff > and_c = { 0, 3, 0, 0 };
    std::vector< Coeff > mul_c = { 0, 7, 0, 0 };
    auto result                = LowerArithmeticFragment(and_c, mul_c, 2, 64);
    ASSERT_TRUE(result.has_value());
    auto &r = result.value();

    EXPECT_EQ(r.poly.terms.at(make_exp({ 1, 0 })), 3u);
    EXPECT_EQ(r.poly.terms.at(make_exp({ 2, 0 })), 7u);
    EXPECT_EQ(r.residual_and_coeffs[1], 0u);
}

TEST(ArithmeticLoweringTest, MultiVarProductTransferred) {
    // mul_c[0b11]=9 -> x_0 * x_1 product
    std::vector< Coeff > and_c = { 0, 0, 0, 0 };
    std::vector< Coeff > mul_c = { 0, 0, 0, 9 };
    auto result                = LowerArithmeticFragment(and_c, mul_c, 2, 64);
    ASSERT_TRUE(result.has_value());
    auto &r = result.value();

    EXPECT_EQ(r.poly.terms.at(make_exp({ 1, 1 })), 9u);
}

TEST(ArithmeticLoweringTest, ResidualAndPreserved) {
    // and_c[3]=42 (|m|=2) should stay in residual
    std::vector< Coeff > and_c = { 10, 0, 0, 42 };
    std::vector< Coeff > mul_c = { 0, 0, 0, 0 };
    auto result                = LowerArithmeticFragment(and_c, mul_c, 2, 64);
    ASSERT_TRUE(result.has_value());
    auto &r = result.value();

    EXPECT_EQ(r.residual_and_coeffs[0], 10u); // constant preserved
    EXPECT_EQ(r.residual_and_coeffs[3], 42u); // |m|>=2 preserved
    EXPECT_TRUE(r.poly.terms.empty());
}

TEST(ArithmeticLoweringTest, ZeroInputsEmptyPoly) {
    std::vector< Coeff > and_c = { 0, 0, 0, 0 };
    std::vector< Coeff > mul_c = { 0, 0, 0, 0 };
    auto result                = LowerArithmeticFragment(and_c, mul_c, 2, 64);
    ASSERT_TRUE(result.has_value());
    auto &r = result.value();

    EXPECT_TRUE(r.poly.terms.empty());
}

TEST(ArithmeticLoweringTest, ThreeVarMixed) {
    // 3 vars: and_c[1]=2 (x_0), mul_c[1]=3 (x_0^2),
    //         mul_c[0b110]=5 (x_1*x_2)
    std::vector< Coeff > and_c(8, 0);
    std::vector< Coeff > mul_c(8, 0);
    and_c[1]    = 2;
    mul_c[1]    = 3;
    mul_c[6]    = 5; // 0b110 = x_1 * x_2
    auto result = LowerArithmeticFragment(and_c, mul_c, 3, 64);
    ASSERT_TRUE(result.has_value());
    auto &r = result.value();

    EXPECT_EQ(r.poly.terms.at(make_exp({ 1, 0, 0 })), 2u);
    EXPECT_EQ(r.poly.terms.at(make_exp({ 2, 0, 0 })), 3u);
    EXPECT_EQ(r.poly.terms.at(make_exp({ 0, 1, 1 })), 5u);
    EXPECT_EQ(r.residual_and_coeffs[1], 0u);
}
