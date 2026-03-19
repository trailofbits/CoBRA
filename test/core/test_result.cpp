#include "cobra/core/Result.h"
#include <gtest/gtest.h>
#include <string>

using namespace cobra;

TEST(ResultTest, SuccessHoldsValue) {
    Result< int > r = Ok(42);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value(), 42);
}

TEST(ResultTest, ErrorHoldsError) {
    Result< int > r = Err< int >(CobraError::kParseError, "bad token");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, CobraError::kParseError);
    EXPECT_EQ(r.error().message, "bad token");
}

TEST(ResultTest, AllErrorCodes) {
    auto parse   = Err< int >(CobraError::kParseError, "");
    auto nonlin  = Err< int >(CobraError::kNonLinearInput, "");
    auto toomany = Err< int >(CobraError::kTooManyVariables, "");
    auto noreduc = Err< int >(CobraError::kNoReduction, "");
    auto verify  = Err< int >(CobraError::kVerificationFailed, "");

    EXPECT_EQ(parse.error().code, CobraError::kParseError);
    EXPECT_EQ(nonlin.error().code, CobraError::kNonLinearInput);
    EXPECT_EQ(toomany.error().code, CobraError::kTooManyVariables);
    EXPECT_EQ(noreduc.error().code, CobraError::kNoReduction);
    EXPECT_EQ(verify.error().code, CobraError::kVerificationFailed);
}

TEST(ResultTest, MoveOnlyValue) {
    Result< std::unique_ptr< int > > r = Ok(std::make_unique< int >(7));
    ASSERT_TRUE(r.has_value());
    auto ptr = std::move(r).value();
    EXPECT_EQ(*ptr, 7);
}
