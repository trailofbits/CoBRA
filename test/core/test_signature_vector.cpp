#include "cobra/core/SignatureVector.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(SignatureVectorTest, SingleVarIdentity) {
    SignatureVector sv(1, 64);
    auto sig = sv.FromValues({ 0, 1 });
    ASSERT_EQ(sig.size(), 2u);
    EXPECT_EQ(sig[0], 0u);
    EXPECT_EQ(sig[1], 1u);
}

TEST(SignatureVectorTest, TwoVarAdd) {
    SignatureVector sv(2, 64);
    auto sig = sv.FromValues({ 0, 1, 1, 2 });
    ASSERT_EQ(sig.size(), 4u);
    EXPECT_EQ(sig[0], 0u);
    EXPECT_EQ(sig[1], 1u);
    EXPECT_EQ(sig[2], 1u);
    EXPECT_EQ(sig[3], 2u);
}

TEST(SignatureVectorTest, TwoVarXor) {
    SignatureVector sv(2, 64);
    auto sig = sv.FromValues({ 0, 1, 1, 0 });
    ASSERT_EQ(sig.size(), 4u);
    EXPECT_EQ(sig[3], 0u);
}

TEST(SignatureVectorTest, TwoVarAnd) {
    SignatureVector sv(2, 64);
    auto sig = sv.FromValues({ 0, 0, 0, 1 });
    EXPECT_EQ(sig[3], 1u);
}

TEST(SignatureVectorTest, TwoVarOr) {
    SignatureVector sv(2, 64);
    auto sig = sv.FromValues({ 0, 1, 1, 1 });
    EXPECT_EQ(sig[3], 1u);
}

TEST(SignatureVectorTest, ConstantExpression) {
    SignatureVector sv(2, 64);
    auto sig = sv.FromValues({ 42, 42, 42, 42 });
    for (auto v : sig) { EXPECT_EQ(v, 42u); }
}

TEST(SignatureVectorTest, Bitwidth8Wraps) {
    SignatureVector sv(1, 8);
    auto sig = sv.FromValues({ 0, 200 });
    EXPECT_EQ(sig[1], 200u);
}

TEST(SignatureVectorTest, ThreeVars) {
    SignatureVector sv(3, 64);
    std::vector< uint64_t > vals = { 0, 1, 2, 3, 4, 5, 6, 7 };
    auto sig                     = sv.FromValues(vals);
    ASSERT_EQ(sig.size(), 8u);
    for (size_t i = 0; i < 8; ++i) { EXPECT_EQ(sig[i], i); }
}

TEST(SignatureVectorTest, NumVars) {
    SignatureVector sv(4, 64);
    EXPECT_EQ(sv.NumVars(), 4u);
    EXPECT_EQ(sv.Length(), 16u);
}
