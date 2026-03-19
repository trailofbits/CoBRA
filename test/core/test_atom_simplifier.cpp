#include "cobra/core/AtomSimplifier.h"
#include <gtest/gtest.h>

using namespace cobra;

// --- Atom simplification tests ---

TEST(AtomSimplifierTest, DoubleNotElimination) {
    auto atom       = Expr::BitwiseNot(Expr::BitwiseNot(Expr::Variable(0)));
    auto simplified = SimplifyAtom(std::move(atom));
    EXPECT_EQ(simplified->kind, Expr::Kind::kVariable);
}

TEST(AtomSimplifierTest, AndWithZero) {
    auto atom       = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0));
    auto simplified = SimplifyAtom(std::move(atom));
    EXPECT_EQ(simplified->kind, Expr::Kind::kConstant);
    EXPECT_EQ(simplified->constant_val, 0u);
}

TEST(AtomSimplifierTest, OrWithAllOnes) {
    auto atom       = Expr::BitwiseOr(Expr::Variable(0), Expr::Constant(UINT64_MAX));
    auto simplified = SimplifyAtom(std::move(atom));
    EXPECT_EQ(simplified->kind, Expr::Kind::kConstant);
    EXPECT_EQ(simplified->constant_val, UINT64_MAX);
}

TEST(AtomSimplifierTest, XorWithZero) {
    auto atom       = Expr::BitwiseXor(Expr::Variable(0), Expr::Constant(0));
    auto simplified = SimplifyAtom(std::move(atom));
    EXPECT_EQ(simplified->kind, Expr::Kind::kVariable);
}

TEST(AtomSimplifierTest, AndWithAllOnes) {
    auto atom       = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(UINT64_MAX));
    auto simplified = SimplifyAtom(std::move(atom));
    EXPECT_EQ(simplified->kind, Expr::Kind::kVariable);
}

TEST(AtomSimplifierTest, NestedConstantFold) {
    // (x & 0xFF) & 0x0F stays as And(And(x, 0xFF), 0x0F)
    // -- no reassociation, but should not break
    auto atom = Expr::BitwiseAnd(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)), Expr::Constant(0x0F)
    );
    auto simplified = SimplifyAtom(std::move(atom));
    EXPECT_EQ(simplified->kind, Expr::Kind::kAnd);
}

TEST(AtomSimplifierTest, NoChangeForIrreducible) {
    auto atom       = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF));
    auto simplified = SimplifyAtom(std::move(atom));
    EXPECT_EQ(simplified->kind, Expr::Kind::kAnd);
}

TEST(AtomSimplifierTest, ConstantOnlyFolds) {
    // ~0 & 7 = 7
    auto atom       = Expr::BitwiseAnd(Expr::BitwiseNot(Expr::Constant(0)), Expr::Constant(7));
    auto simplified = SimplifyAtom(std::move(atom));
    EXPECT_EQ(simplified->kind, Expr::Kind::kConstant);
    EXPECT_EQ(simplified->constant_val, 7u);
}

// --- Narrow-bitwidth tests ---

// At 8-bit width, ~0 evaluates to 0xFF via constant folding
TEST(AtomSimplifierTest, NarrowBitwidthNotConstant) {
    auto atom   = Expr::BitwiseNot(Expr::Constant(0));
    auto result = SimplifyAtom(std::move(atom), 8);
    ASSERT_EQ(result->kind, Expr::Kind::kConstant);
    EXPECT_EQ(result->constant_val, 0xFFULL);
}

// At 8-bit width, 0xFF is all-ones, so x & 0xFF = x
TEST(AtomSimplifierTest, NarrowBitwidthAndAllOnes) {
    auto atom   = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF));
    auto result = SimplifyAtom(std::move(atom), 8);
    EXPECT_EQ(result->kind, Expr::Kind::kVariable);
}

// At 8-bit width, x | 0xFF folds to constant 0xFF
TEST(AtomSimplifierTest, NarrowBitwidthOrAllOnes) {
    auto atom   = Expr::BitwiseOr(Expr::Variable(0), Expr::Constant(0xFF));
    auto result = SimplifyAtom(std::move(atom), 8);
    ASSERT_EQ(result->kind, Expr::Kind::kConstant);
    EXPECT_EQ(result->constant_val, 0xFFULL);
}

// --- De Morgan tests ---

TEST(AtomSimplifierTest, DeMorganAndToOr) {
    auto atom = Expr::BitwiseNot(
        Expr::BitwiseAnd(
            Expr::BitwiseNot(Expr::Variable(0)), Expr::BitwiseNot(Expr::Variable(1))
        )
    );
    auto result = SimplifyAtom(std::move(atom));
    ASSERT_EQ(result->kind, Expr::Kind::kOr);
    EXPECT_EQ(result->children[0]->kind, Expr::Kind::kVariable);
    EXPECT_EQ(result->children[0]->var_index, 0);
    EXPECT_EQ(result->children[1]->kind, Expr::Kind::kVariable);
    EXPECT_EQ(result->children[1]->var_index, 1);
}

TEST(AtomSimplifierTest, DeMorganOrToAnd) {
    auto atom = Expr::BitwiseNot(
        Expr::BitwiseOr(
            Expr::BitwiseNot(Expr::Variable(0)), Expr::BitwiseNot(Expr::Variable(1))
        )
    );
    auto result = SimplifyAtom(std::move(atom));
    ASSERT_EQ(result->kind, Expr::Kind::kAnd);
    EXPECT_EQ(result->children[0]->var_index, 0);
    EXPECT_EQ(result->children[1]->var_index, 1);
}

TEST(AtomSimplifierTest, DeMorganNested) {
    auto atom = Expr::BitwiseNot(
        Expr::BitwiseAnd(
            Expr::BitwiseNot(Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF))),
            Expr::BitwiseNot(Expr::Variable(1))
        )
    );
    auto result = SimplifyAtom(std::move(atom));
    ASSERT_EQ(result->kind, Expr::Kind::kOr);
}

// --- Idempotency tests ---

TEST(AtomSimplifierTest, IdempotentAnd) {
    auto atom   = Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(0));
    auto result = SimplifyAtom(std::move(atom));
    ASSERT_EQ(result->kind, Expr::Kind::kVariable);
    EXPECT_EQ(result->var_index, 0);
}

TEST(AtomSimplifierTest, IdempotentOr) {
    auto atom   = Expr::BitwiseOr(Expr::Variable(0), Expr::Variable(0));
    auto result = SimplifyAtom(std::move(atom));
    ASSERT_EQ(result->kind, Expr::Kind::kVariable);
    EXPECT_EQ(result->var_index, 0);
}

TEST(AtomSimplifierTest, IdempotentOrComplex) {
    auto atom = Expr::BitwiseOr(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)),
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF))
    );
    auto result = SimplifyAtom(std::move(atom));
    ASSERT_EQ(result->kind, Expr::Kind::kAnd);
}

// --- Structure simplification tests ---

TEST(StructSimplifierTest, MergeLikeAtoms) {
    SemilinearIR ir;
    ir.bitwidth = 64;
    ir.constant = 0;
    AtomKey key{
        { 0 },
        { 0, 1 }
    };
    ir.atom_table.push_back(AtomInfo{ 0, key, Expr::Variable(0), 0, OperatorFamily::kMixed });
    ir.terms.push_back({ 3, 0 });
    ir.terms.push_back({ 5, 0 });

    SimplifyStructure(ir);
    EXPECT_EQ(ir.terms.size(), 1u);
    EXPECT_EQ(ir.terms[0].coeff, 8u);
}

TEST(StructSimplifierTest, ZeroCoefficientEliminated) {
    SemilinearIR ir;
    ir.bitwidth = 64;
    ir.constant = 0;
    AtomKey key{
        { 0 },
        { 0, 1 }
    };
    ir.atom_table.push_back(AtomInfo{ 0, key, Expr::Variable(0), 0, OperatorFamily::kMixed });
    ir.terms.push_back({ 0, 0 });

    SimplifyStructure(ir);
    EXPECT_EQ(ir.terms.size(), 0u);
}

TEST(StructSimplifierTest, CoefficientOverflowMerge) {
    // 2^64 - 1 + 1 = 0 mod 2^64 -> term eliminated
    SemilinearIR ir;
    ir.bitwidth = 64;
    ir.constant = 0;
    AtomKey key{
        { 0 },
        { 0, 1 }
    };
    ir.atom_table.push_back(AtomInfo{ 0, key, Expr::Variable(0), 0, OperatorFamily::kMixed });
    ir.terms.push_back({ UINT64_MAX, 0 });
    ir.terms.push_back({ 1, 0 });

    SimplifyStructure(ir);
    EXPECT_EQ(ir.terms.size(), 0u);
}

// --- Complement recognition tests ---

TEST(StructSimplifierTest, ComplementRecognition) {
    // 8-bit: atom0 = f with tt {0x0F, 0xF0},
    //        atom1 = ~f with tt {0xF0, 0x0F}
    // Both have coeff 3. Result: constant += 3 * 0xFF.
    SemilinearIR ir;
    ir.bitwidth   = 8;
    ir.constant   = 0;
    uint64_t mask = 0xFF;

    AtomKey k0{
        { 0 },
        { 0x0F, 0xF0 }
    };
    ir.atom_table.push_back(AtomInfo{ 0, k0, Expr::Variable(0), 0, OperatorFamily::kMixed });

    AtomKey k1{
        { 0 },
        { 0xF0, 0x0F }
    };
    ir.atom_table.push_back(AtomInfo{ 1, k1, Expr::Variable(0), 0, OperatorFamily::kMixed });

    ir.terms.push_back({ 3, 0 });
    ir.terms.push_back({ 3, 1 });

    SimplifyStructure(ir);
    EXPECT_EQ(ir.terms.size(), 0u);
    EXPECT_EQ(ir.constant, (3 * mask) & mask);
}

TEST(StructSimplifierTest, ComplementRecognition64Bit) {
    SemilinearIR ir;
    ir.bitwidth   = 64;
    ir.constant   = 0;
    uint64_t mask = UINT64_MAX;

    AtomKey k0{
        { 0 },
        { 0xAAAAAAAAAAAAAAAAULL, 0x5555555555555555ULL }
    };
    ir.atom_table.push_back(AtomInfo{ 0, k0, Expr::Variable(0), 0, OperatorFamily::kMixed });

    AtomKey k1{
        { 0 },
        { 0x5555555555555555ULL, 0xAAAAAAAAAAAAAAAAULL }
    };
    ir.atom_table.push_back(AtomInfo{ 1, k1, Expr::Variable(0), 0, OperatorFamily::kMixed });

    ir.terms.push_back({ 3, 0 });
    ir.terms.push_back({ 3, 1 });

    SimplifyStructure(ir);
    EXPECT_EQ(ir.terms.size(), 0u);
    EXPECT_EQ(ir.constant, (3ULL * mask) & mask);
}

TEST(StructSimplifierTest, ComplementRecognitionDifferentCoeffs) {
    // Same complementary atoms but different coefficients -> no absorption
    SemilinearIR ir;
    ir.bitwidth = 8;
    ir.constant = 0;

    AtomKey k0{
        { 0 },
        { 0x0F, 0xF0 }
    };
    ir.atom_table.push_back(AtomInfo{ 0, k0, Expr::Variable(0), 0, OperatorFamily::kMixed });

    AtomKey k1{
        { 0 },
        { 0xF0, 0x0F }
    };
    ir.atom_table.push_back(AtomInfo{ 1, k1, Expr::Variable(0), 0, OperatorFamily::kMixed });

    ir.terms.push_back({ 3, 0 });
    ir.terms.push_back({ 5, 1 });

    SimplifyStructure(ir);
    EXPECT_EQ(ir.terms.size(), 2u);
    EXPECT_EQ(ir.constant, 0u);
}

TEST(StructSimplifierTest, ComplementRecognitionPartial) {
    // Three terms: atom0 and atom1 are complementary (coeff 2),
    // atom2 is unrelated -> atom0+atom1 absorbed, atom2 remains
    SemilinearIR ir;
    ir.bitwidth   = 8;
    ir.constant   = 0;
    uint64_t mask = 0xFF;

    AtomKey k0{
        { 0 },
        { 0xAA, 0x55 }
    };
    ir.atom_table.push_back(AtomInfo{ 0, k0, Expr::Variable(0), 0, OperatorFamily::kMixed });

    AtomKey k1{
        { 0 },
        { 0x55, 0xAA }
    };
    ir.atom_table.push_back(AtomInfo{ 1, k1, Expr::Variable(0), 0, OperatorFamily::kMixed });

    AtomKey k2{
        { 1 },
        { 0x33, 0xCC }
    };
    ir.atom_table.push_back(AtomInfo{ 2, k2, Expr::Variable(1), 0, OperatorFamily::kMixed });

    ir.terms.push_back({ 2, 0 });
    ir.terms.push_back({ 2, 1 });
    ir.terms.push_back({ 7, 2 });

    SimplifyStructure(ir);
    EXPECT_EQ(ir.terms.size(), 1u);
    EXPECT_EQ(ir.terms[0].atom_id, 2u);
    EXPECT_EQ(ir.terms[0].coeff, 7u);
    EXPECT_EQ(ir.constant, (2 * mask) & mask);
}

TEST(AtomSimplifierTest, ShrZeroIdentity) {
    auto atom   = Expr::LogicalShr(Expr::Variable(0), 0);
    auto result = SimplifyAtom(std::move(atom), 64);
    EXPECT_EQ(result->kind, Expr::Kind::kVariable);
    EXPECT_EQ(result->var_index, 0u);
}

TEST(AtomSimplifierTest, ShrConstantFolds) {
    auto atom   = Expr::LogicalShr(Expr::Constant(0xFF), 4);
    auto result = SimplifyAtom(std::move(atom), 64);
    EXPECT_EQ(result->kind, Expr::Kind::kConstant);
    EXPECT_EQ(result->constant_val, 0xFu);
}

TEST(AtomSimplifierTest, ShrPreserved) {
    auto atom = Expr::LogicalShr(Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)), 3);
    auto result = SimplifyAtom(std::move(atom), 64);
    EXPECT_EQ(result->kind, Expr::Kind::kShr);
    EXPECT_EQ(result->constant_val, 3u);
}
