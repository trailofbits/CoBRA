#include "cobra/core/AuxVarEliminator.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(AuxVarEliminatorTest, NoSpuriousVars) {
    std::vector< uint64_t > sig     = { 0, 1, 1, 2 };
    std::vector< std::string > vars = { "x", "y" };

    auto result = EliminateAuxVars(sig, vars);

    EXPECT_EQ(result.reduced_sig, sig);
    EXPECT_EQ(result.real_vars, vars);
    EXPECT_TRUE(result.spurious_vars.empty());
}

TEST(AuxVarEliminatorTest, OneSpuriousVar) {
    // f(x, a0) = x regardless of a0.
    // sig indices: (a0=0,x=0)=0, (a0=1,x=0)=0, (a0=0,x=1)=1, (a0=1,x=1)=1
    // Wait — vars are sorted lexicographically: a0 < x
    // a0 is bit 0, x is bit 1
    // index 0 = (a0=0,x=0)=0, index 1 = (a0=1,x=0)=0,
    // index 2 = (a0=0,x=1)=1, index 3 = (a0=1,x=1)=1
    std::vector< uint64_t > sig     = { 0, 0, 1, 1 };
    std::vector< std::string > vars = { "a0", "x" };

    auto result = EliminateAuxVars(sig, vars);

    EXPECT_EQ(result.real_vars.size(), 1u);
    EXPECT_EQ(result.real_vars[0], "x");
    EXPECT_EQ(result.spurious_vars.size(), 1u);
    EXPECT_EQ(result.spurious_vars[0], "a0");
    EXPECT_EQ(result.reduced_sig.size(), 2u);
    EXPECT_EQ(result.reduced_sig[0], 0u);
    EXPECT_EQ(result.reduced_sig[1], 1u);
}

TEST(AuxVarEliminatorTest, TwoSpuriousVars) {
    // 4 vars: a0 < a1 < x < y. a0 and a1 are spurious.
    // x + y with 4 vars. a0=bit0, a1=bit1, x=bit2, y=bit3
    // For each combo of (a0, a1), sig for (x, y) is [0, 1, 1, 2].
    // Build the 16-entry sig: iterate over all 16 combos
    std::vector< uint64_t > sig(16);
    for (uint32_t i = 0; i < 16; ++i) {
        uint64_t x = (i >> 2) & 1;
        uint64_t y = (i >> 3) & 1;
        sig[i]     = x + y;
    }
    std::vector< std::string > vars = { "a0", "a1", "x", "y" };

    auto result = EliminateAuxVars(sig, vars);

    EXPECT_EQ(result.real_vars.size(), 2u);
    EXPECT_EQ(result.real_vars[0], "x");
    EXPECT_EQ(result.real_vars[1], "y");
    EXPECT_EQ(result.spurious_vars.size(), 2u);
    EXPECT_EQ(result.reduced_sig.size(), 4u);
    EXPECT_EQ(result.reduced_sig[0], 0u);
    EXPECT_EQ(result.reduced_sig[1], 1u);
    EXPECT_EQ(result.reduced_sig[2], 1u);
    EXPECT_EQ(result.reduced_sig[3], 2u);
}

TEST(AuxVarEliminatorTest, AllSpurious) {
    std::vector< uint64_t > sig     = { 42, 42, 42, 42 };
    std::vector< std::string > vars = { "x", "y" };

    auto result = EliminateAuxVars(sig, vars);

    EXPECT_TRUE(result.real_vars.empty());
    EXPECT_EQ(result.spurious_vars.size(), 2u);
    EXPECT_EQ(result.reduced_sig.size(), 1u);
    EXPECT_EQ(result.reduced_sig[0], 42u);
}

TEST(AuxVarEliminatorTest, SingleVar) {
    std::vector< uint64_t > sig     = { 0, 1 };
    std::vector< std::string > vars = { "x" };

    auto result = EliminateAuxVars(sig, vars);

    EXPECT_EQ(result.real_vars.size(), 1u);
    EXPECT_EQ(result.reduced_sig.size(), 2u);
}

TEST(AuxVarEliminatorTest, NoVars) {
    std::vector< uint64_t > sig     = { 7 };
    std::vector< std::string > vars = {};

    auto result = EliminateAuxVars(sig, vars);

    EXPECT_TRUE(result.real_vars.empty());
    EXPECT_EQ(result.reduced_sig.size(), 1u);
    EXPECT_EQ(result.reduced_sig[0], 7u);
}

// Gap #7: Interleaved spurious var in the middle
TEST(AuxVarEliminatorTest, InterleavedSpuriousVar) {
    // 3 vars: x (bit 0), a0 (bit 1), y (bit 2). a0 is spurious.
    // f = x + y, independent of a0.
    std::vector< uint64_t > sig(8);
    for (uint32_t i = 0; i < 8; ++i) {
        uint64_t x = (i >> 0) & 1;
        uint64_t y = (i >> 2) & 1;
        sig[i]     = x + y;
    }
    std::vector< std::string > vars = { "x", "a0", "y" };

    auto result = EliminateAuxVars(sig, vars);

    EXPECT_EQ(result.real_vars.size(), 2u);
    EXPECT_EQ(result.real_vars[0], "x");
    EXPECT_EQ(result.real_vars[1], "y");
    EXPECT_EQ(result.spurious_vars.size(), 1u);
    EXPECT_EQ(result.spurious_vars[0], "a0");
    EXPECT_EQ(result.reduced_sig.size(), 4u);
    EXPECT_EQ(result.reduced_sig[0], 0u);
    EXPECT_EQ(result.reduced_sig[1], 1u);
    EXPECT_EQ(result.reduced_sig[2], 1u);
    EXPECT_EQ(result.reduced_sig[3], 2u);
}

// Multiple non-adjacent interleaved spurious vars
TEST(AuxVarEliminatorTest, MultipleInterleavedSpurious) {
    // 5 vars: x (bit 0), a0 (bit 1), y (bit 2), a1 (bit 3), z (bit 4)
    // a0 and a1 are spurious. f = x + y + z.
    std::vector< uint64_t > sig(32);
    for (uint32_t i = 0; i < 32; ++i) {
        uint64_t x = (i >> 0) & 1;
        uint64_t y = (i >> 2) & 1;
        uint64_t z = (i >> 4) & 1;
        sig[i]     = x + y + z;
    }
    std::vector< std::string > vars = { "x", "a0", "y", "a1", "z" };

    auto result = EliminateAuxVars(sig, vars);

    EXPECT_EQ(result.real_vars.size(), 3u);
    EXPECT_EQ(result.real_vars[0], "x");
    EXPECT_EQ(result.real_vars[1], "y");
    EXPECT_EQ(result.real_vars[2], "z");
    EXPECT_EQ(result.spurious_vars.size(), 2u);
    EXPECT_EQ(result.reduced_sig.size(), 8u);
    // Reduced sig for x + y + z over 3 vars
    EXPECT_EQ(result.reduced_sig[0], 0u);
    EXPECT_EQ(result.reduced_sig[1], 1u);
    EXPECT_EQ(result.reduced_sig[2], 1u);
    EXPECT_EQ(result.reduced_sig[3], 2u);
    EXPECT_EQ(result.reduced_sig[4], 1u);
    EXPECT_EQ(result.reduced_sig[5], 2u);
    EXPECT_EQ(result.reduced_sig[6], 2u);
    EXPECT_EQ(result.reduced_sig[7], 3u);
}

// Full-width sensitivity: x*y looks spurious for y on {0,1}
// because x*y == x&y on {0,1}, but y matters at full width.
TEST(AuxVarEliminatorTest, FullWidthKeepsLiveVar) {
    // g(x,y) = x*y - x&y is zero on all {0,1} inputs, so both x and y
    // look spurious. But g(2,3) = 6 - 2 = 4, so both matter at full width.
    std::vector< uint64_t > sig     = { 0, 0, 0, 0 }; // g=0 on all {0,1}
    std::vector< std::string > vars = { "x", "y" };

    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return (v[0] * v[1]) - (v[0] & v[1]);
    };

    auto result = EliminateAuxVars(sig, vars, eval, 64);

    // Both x and y must be kept as real vars
    EXPECT_EQ(result.real_vars.size(), 2u);
    EXPECT_EQ(result.spurious_vars.size(), 0u);
}

// Full-width sensitivity: y is truly spurious at all widths.
TEST(AuxVarEliminatorTest, FullWidthEliminatesGenuineSpurious) {
    // f(x,y) = x + 0*y = x. y never affects the output.
    std::vector< uint64_t > sig     = { 0, 1, 0, 1 }; // f(0,0)=0,f(1,0)=1,f(0,1)=0,f(1,1)=1
    std::vector< std::string > vars = { "x", "y" };

    auto eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0]; };

    auto result = EliminateAuxVars(sig, vars, eval, 64);

    EXPECT_EQ(result.real_vars.size(), 1u);
    EXPECT_EQ(result.real_vars[0], "x");
    EXPECT_EQ(result.spurious_vars.size(), 1u);
    EXPECT_EQ(result.spurious_vars[0], "y");
    // Reduced sig: f(0)=0, f(1)=1
    EXPECT_EQ(result.reduced_sig.size(), 2u);
    EXPECT_EQ(result.reduced_sig[0], 0u);
    EXPECT_EQ(result.reduced_sig[1], 1u);
}
