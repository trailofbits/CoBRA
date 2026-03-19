#include "cobra/core/Expr.h"
#include "cobra/core/MaskedAtomReconstructor.h"
#include "cobra/core/SemilinearIR.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(ReconstructorTest, EmptyIR) {
    SemilinearIR ir;
    ir.bitwidth = 64;
    ir.constant = 42;
    auto result = ReconstructMaskedAtoms(ir, {});
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->kind, Expr::Kind::kConstant);
    EXPECT_EQ(result->constant_val, 42u);
}

TEST(ReconstructorTest, SingleAtomBareCoeff) {
    SemilinearIR ir;
    ir.bitwidth = 64;
    ir.constant = 0;
    auto atom   = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF));
    AtomKey key{ { 0 }, ComputeAtomTruthTable(*atom, { 0 }, 64) };
    ir.atom_table.push_back(AtomInfo{ 0, key, std::move(atom), 0, OperatorFamily::kAnd });
    ir.terms.push_back({ 1, 0 });

    auto result = ReconstructMaskedAtoms(ir, {});
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->kind, Expr::Kind::kAnd);
}

TEST(ReconstructorTest, WithConstant) {
    SemilinearIR ir;
    ir.bitwidth = 64;
    ir.constant = 42;
    auto atom   = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF));
    AtomKey key{ { 0 }, ComputeAtomTruthTable(*atom, { 0 }, 64) };
    ir.atom_table.push_back(AtomInfo{ 0, key, std::move(atom), 0, OperatorFamily::kAnd });
    ir.terms.push_back({ 1, 0 });

    auto result = ReconstructMaskedAtoms(ir, {});
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->kind, Expr::Kind::kAdd);
}

TEST(ReconstructorTest, CoefficientApplied) {
    SemilinearIR ir;
    ir.bitwidth = 64;
    ir.constant = 0;
    auto atom   = Expr::BitwiseXor(Expr::Variable(0), Expr::Constant(0x55));
    AtomKey key{ { 0 }, ComputeAtomTruthTable(*atom, { 0 }, 64) };
    ir.atom_table.push_back(AtomInfo{ 0, key, std::move(atom), 0, OperatorFamily::kXor });
    ir.terms.push_back({ 5, 0 });

    auto result = ReconstructMaskedAtoms(ir, {});
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->kind, Expr::Kind::kMul);
    EXPECT_EQ(result->children[0]->kind, Expr::Kind::kConstant);
    EXPECT_EQ(result->children[0]->constant_val, 5u);
}

TEST(ReconstructorTest, NegativeCoefficient) {
    SemilinearIR ir;
    ir.bitwidth = 64;
    ir.constant = 0;
    auto atom   = Expr::Variable(0);
    AtomKey key{ { 0 }, ComputeAtomTruthTable(*atom, { 0 }, 64) };
    ir.atom_table.push_back(AtomInfo{ 0, key, std::move(atom), 0, OperatorFamily::kMixed });
    ir.terms.push_back({ UINT64_MAX, 0 });

    auto result = ReconstructMaskedAtoms(ir, {});
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->kind, Expr::Kind::kNeg);
}

TEST(ReconstructorTest, TwoAtomsAddition) {
    SemilinearIR ir;
    ir.bitwidth = 64;
    ir.constant = 0;
    auto a1     = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF));
    auto a2     = Expr::BitwiseOr(Expr::Variable(1), Expr::Constant(0x80));
    AtomKey k1{ { 0 }, ComputeAtomTruthTable(*a1, { 0 }, 64) };
    AtomKey k2{ { 1 }, ComputeAtomTruthTable(*a2, { 1 }, 64) };
    ir.atom_table.push_back(AtomInfo{ 0, k1, std::move(a1), 0, OperatorFamily::kAnd });
    ir.atom_table.push_back(AtomInfo{ 1, k2, std::move(a2), 0, OperatorFamily::kOr });
    ir.terms.push_back({ 1, 0 });
    ir.terms.push_back({ 1, 1 });

    auto result = ReconstructMaskedAtoms(ir, {});
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->kind, Expr::Kind::kAdd);
}

TEST(ReconstructorTest, DisjointAtomsOrRewrite) {
    // Two atoms with non-overlapping active masks get OR-combined.
    // atom0 active on low 8 bits, atom1 active on bits 8-63.
    SemilinearIR ir;
    ir.bitwidth = 64;
    ir.constant = 0;
    auto a0     = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF));
    auto a1     = Expr::BitwiseAnd(Expr::Variable(1), Expr::Constant(~uint64_t(0xFF)));
    AtomKey k0{ { 0 }, ComputeAtomTruthTable(*a0, { 0 }, 64) };
    AtomKey k1{ { 1 }, ComputeAtomTruthTable(*a1, { 1 }, 64) };
    ir.atom_table.push_back(AtomInfo{ 0, k0, std::move(a0), 0, OperatorFamily::kAnd });
    ir.atom_table.push_back(AtomInfo{ 1, k1, std::move(a1), 0, OperatorFamily::kAnd });
    ir.terms.push_back({ 1, 0 });
    ir.terms.push_back({ 1, 1 });

    // Build partitions: low byte has atom0 active (id=1),
    // atom1 zero (id=0); high bits vice versa.
    PartitionClass low;
    low.mask    = 0xFF;
    low.profile = { 1, 0 }; // atom0=nonzero, atom1=zero
    PartitionClass high;
    high.mask    = ~uint64_t(0xFF);
    high.profile = { 0, 1 }; // atom0=zero, atom1=nonzero

    auto result = ReconstructMaskedAtoms(ir, { low, high });
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->kind, Expr::Kind::kOr);
}

TEST(ReconstructorTest, OverlappingAtomsNoOrRewrite) {
    // Two atoms sharing active bits should NOT be OR-combined.
    SemilinearIR ir;
    ir.bitwidth = 64;
    ir.constant = 0;
    auto a0     = Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1));
    auto a1     = Expr::BitwiseOr(Expr::Variable(0), Expr::Variable(1));
    AtomKey k0{
        { 0, 1 },
        ComputeAtomTruthTable(*a0, { 0, 1 },
        64)
    };
    AtomKey k1{
        { 0, 1 },
        ComputeAtomTruthTable(*a1, { 0, 1 },
        64)
    };
    ir.atom_table.push_back(AtomInfo{ 0, k0, std::move(a0), 0, OperatorFamily::kAnd });
    ir.atom_table.push_back(AtomInfo{ 1, k1, std::move(a1), 0, OperatorFamily::kOr });
    ir.terms.push_back({ 1, 0 });
    ir.terms.push_back({ 1, 1 });

    // Both atoms active everywhere.
    PartitionClass all;
    all.mask    = UINT64_MAX;
    all.profile = { 1, 1 };

    auto result = ReconstructMaskedAtoms(ir, { all });
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->kind, Expr::Kind::kAdd);
}

TEST(ReconstructorTest, ZeroConstantNoExtraNode) {
    // constant=0 should not produce an extra Add node.
    SemilinearIR ir;
    ir.bitwidth = 64;
    ir.constant = 0;
    auto atom   = Expr::Variable(0);
    AtomKey key{ { 0 }, ComputeAtomTruthTable(*atom, { 0 }, 64) };
    ir.atom_table.push_back(AtomInfo{ 0, key, std::move(atom), 0, OperatorFamily::kMixed });
    ir.terms.push_back({ 1, 0 });

    auto result = ReconstructMaskedAtoms(ir, {});
    ASSERT_NE(result, nullptr);
    // Should be bare Variable, not Add(0, Variable).
    EXPECT_EQ(result->kind, Expr::Kind::kVariable);
}

TEST(ReconstructorTest, NarrowBitwidthNegCoeff) {
    // -1 mod 2^8 = 0xFF
    SemilinearIR ir;
    ir.bitwidth = 8;
    ir.constant = 0;
    auto atom   = Expr::Variable(0);
    AtomKey key{ { 0 }, ComputeAtomTruthTable(*atom, { 0 }, 8) };
    ir.atom_table.push_back(AtomInfo{ 0, key, std::move(atom), 0, OperatorFamily::kMixed });
    ir.terms.push_back({ 0xFF, 0 }); // -1 mod 2^8

    auto result = ReconstructMaskedAtoms(ir, {});
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->kind, Expr::Kind::kNeg);
}
