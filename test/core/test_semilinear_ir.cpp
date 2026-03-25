#include "cobra/core/SemilinearIR.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(AtomKeyTest, SameAtomKeysAreEqual) {
    AtomKey k1{
        { 0 },
        { 0, 1 }
    };
    AtomKey k2{
        { 0 },
        { 0, 1 }
    };
    EXPECT_EQ(k1, k2);
}

TEST(AtomKeyTest, DifferentTruthTablesNotEqual) {
    AtomKey k1{
        { 0 },
        { 0, 1 }
    }; // identity: x
    AtomKey k2{
        { 0 },
        { 1, 0 }
    }; // NOT: ~x (in 1-bit)
    EXPECT_NE(k1, k2);
}

TEST(AtomKeyTest, DifferentSupportNotEqual) {
    AtomKey k1{
        { 0 },
        { 0, 1 }
    };
    AtomKey k2{
        { 1 },
        { 0, 1 }
    };
    EXPECT_NE(k1, k2);
}

TEST(AtomKeyTest, TwoVarAtom) {
    AtomKey k{
        { 0, 1 },
        { 0, 0, 0, 1 }
    };
    EXPECT_EQ(k.support.size(), 2);
    EXPECT_EQ(k.truth_table.size(), 4);
}

TEST(AtomKeyTest, HashConsistency) {
    AtomKey k1{
        { 0 },
        { 0, 1 }
    };
    AtomKey k2{
        { 0 },
        { 0, 1 }
    };
    std::hash< AtomKey > hasher;
    EXPECT_EQ(hasher(k1), hasher(k2));
}

TEST(SemilinearIRTest, EmptyIR) {
    SemilinearIR ir;
    ir.constant = 0;
    ir.bitwidth = 64;
    EXPECT_EQ(ir.terms.size(), 0);
    EXPECT_EQ(ir.atom_table.size(), 0);
}

TEST(TruthTableTest, EvalSingleVarIdentity) {
    auto expr = Expr::Variable(0);
    auto tt   = ComputeAtomTruthTable(*expr, { 0 }, 64);
    EXPECT_EQ(tt.size(), 2);
    EXPECT_EQ(tt[0], 0); // x=0 -> 0
    EXPECT_EQ(tt[1], 1); // x=1 -> 1
}

TEST(TruthTableTest, EvalAndWithConstant) {
    auto expr = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF));
    auto tt   = ComputeAtomTruthTable(*expr, { 0 }, 64);
    EXPECT_EQ(tt.size(), 2);
    EXPECT_EQ(tt[0], 0); // x=0: 0 & 0xFF = 0
    EXPECT_EQ(tt[1], 1); // x=1: 1 & 0xFF = 1 (Boolean input)
}

TEST(TruthTableTest, EvalTwoVarXor) {
    auto expr = Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1));
    auto tt   = ComputeAtomTruthTable(*expr, { 0, 1 }, 64);
    EXPECT_EQ(tt.size(), 4);
    EXPECT_EQ(tt[0], 0); // x=0,y=0
    EXPECT_EQ(tt[1], 1); // x=1,y=0
    EXPECT_EQ(tt[2], 1); // x=0,y=1
    EXPECT_EQ(tt[3], 0); // x=1,y=1
}

TEST(TruthTableTest, EvalNotVariable) {
    auto expr = Expr::BitwiseNot(Expr::Variable(0));
    auto tt   = ComputeAtomTruthTable(*expr, { 0 }, 8);
    EXPECT_EQ(tt.size(), 2);
    EXPECT_EQ(tt[0], 255u); // ~0 = all ones for 8-bit
    EXPECT_EQ(tt[1], 254u); // ~1 = 0xFE
}

TEST(TruthTableTest, EvalOrWithConstant) {
    auto expr = Expr::BitwiseOr(Expr::Variable(0), Expr::Constant(0x80));
    auto tt   = ComputeAtomTruthTable(*expr, { 0 }, 64);
    EXPECT_EQ(tt.size(), 2);
    EXPECT_EQ(tt[0], 0x80u); // 0 | 0x80
    EXPECT_EQ(tt[1], 0x81u); // 1 | 0x80
}

TEST(SemilinearIRTest, TruthTableShr) {
    auto atom = Expr::LogicalShr(Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)), 4);
    auto tt   = ComputeAtomTruthTable(*atom, { 0 }, 64);
    ASSERT_EQ(tt.size(), 2u);
    EXPECT_EQ(tt[0], 0u);
    EXPECT_EQ(tt[1], 0u);
}

TEST(SemilinearIRTest, TruthTableShrIdentity) {
    auto atom = Expr::LogicalShr(Expr::Variable(0), 0);
    auto tt   = ComputeAtomTruthTable(*atom, { 0 }, 64);
    ASSERT_EQ(tt.size(), 2u);
    EXPECT_EQ(tt[0], 0u);
    EXPECT_EQ(tt[1], 1u);
}
