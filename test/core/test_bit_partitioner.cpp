#include "cobra/core/BitPartitioner.h"
#include "cobra/core/Expr.h"
#include "cobra/core/SemilinearIR.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(BitPartitionerTest, SinglePartitionForPureBitwise) {
    // x & y: no constants -> all bits same profile -> 1 partition
    SemilinearIR ir;
    ir.bitwidth = 64;
    ir.constant = 0;
    auto atom   = Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1));
    AtomKey key{
        { 0, 1 },
        ComputeAtomTruthTable(*atom, { 0, 1 },
        64)
    };
    ir.atom_table.push_back(AtomInfo{ 0, key, std::move(atom), 0, OperatorFamily::kAnd });
    ir.terms.push_back({ 1, 0 });

    auto partitions = ComputePartitions(ir);
    EXPECT_EQ(partitions.size(), 1u);
    EXPECT_EQ(partitions[0].mask, UINT64_MAX);
}

TEST(BitPartitionerTest, TwoPartitionsForMaskedAtom) {
    // x & 0xFF: bits 0-7 see identity, bits 8-63 see zero
    SemilinearIR ir;
    ir.bitwidth = 64;
    ir.constant = 0;
    auto atom   = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF));
    AtomKey key{ { 0 }, ComputeAtomTruthTable(*atom, { 0 }, 64) };
    ir.atom_table.push_back(AtomInfo{ 0, key, std::move(atom), 0, OperatorFamily::kAnd });
    ir.terms.push_back({ 1, 0 });

    auto partitions = ComputePartitions(ir);
    EXPECT_EQ(partitions.size(), 2u);
    uint64_t low_mask  = 0xFF;
    uint64_t high_mask = ~uint64_t(0xFF);
    bool found_low     = false;
    bool found_high    = false;
    for (const auto &p : partitions) {
        if (p.mask == low_mask) {
            found_low = true;
        }
        if (p.mask == high_mask) {
            found_high = true;
        }
    }
    EXPECT_TRUE(found_low);
    EXPECT_TRUE(found_high);
}

TEST(BitPartitionerTest, EmptyAtomTable) {
    SemilinearIR ir;
    ir.bitwidth     = 64;
    auto partitions = ComputePartitions(ir);
    EXPECT_EQ(partitions.size(), 0u);
}

TEST(BitPartitionerTest, NarrowBitwidth) {
    // x & 0x0F with bitwidth=8: bits 0-3 see identity, bits 4-7 zero
    SemilinearIR ir;
    ir.bitwidth = 8;
    ir.constant = 0;
    auto atom   = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0x0F));
    AtomKey key{ { 0 }, ComputeAtomTruthTable(*atom, { 0 }, 8) };
    ir.atom_table.push_back(AtomInfo{ 0, key, std::move(atom), 0, OperatorFamily::kAnd });
    ir.terms.push_back({ 1, 0 });

    auto partitions = ComputePartitions(ir);
    EXPECT_EQ(partitions.size(), 2u);
}

TEST(BitPartitionerTest, ProfileVectorMatchesAtomCount) {
    // Two atoms -> each partition's profile has exactly 2 entries
    SemilinearIR ir;
    ir.bitwidth = 8;
    ir.constant = 0;

    auto atom0 = Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1));
    AtomKey key0{
        { 0, 1 },
        ComputeAtomTruthTable(*atom0, { 0, 1 },
        8)
    };
    ir.atom_table.push_back(AtomInfo{ 0, key0, std::move(atom0), 0, OperatorFamily::kAnd });
    ir.terms.push_back({ 1, 0 });

    auto atom1 = Expr::BitwiseOr(Expr::Variable(0), Expr::Variable(1));
    AtomKey key1{
        { 0, 1 },
        ComputeAtomTruthTable(*atom1, { 0, 1 },
        8)
    };
    ir.atom_table.push_back(AtomInfo{ 1, key1, std::move(atom1), 0, OperatorFamily::kOr });
    ir.terms.push_back({ 1, 1 });

    auto partitions = ComputePartitions(ir);
    for (const auto &p : partitions) {
        EXPECT_EQ(p.profile.size(), 2u);
    }
}

TEST(BitPartitionerTest, MasksCoverAllBits) {
    // Union of all partition masks = all bits in bitwidth
    SemilinearIR ir;
    ir.bitwidth = 16;
    ir.constant = 0;
    auto atom   = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0x00FF));
    AtomKey key{ { 0 }, ComputeAtomTruthTable(*atom, { 0 }, 16) };
    ir.atom_table.push_back(AtomInfo{ 0, key, std::move(atom), 0, OperatorFamily::kAnd });
    ir.terms.push_back({ 1, 0 });

    auto partitions   = ComputePartitions(ir);
    uint64_t combined = 0;
    for (const auto &p : partitions) {
        combined |= p.mask;
    }
    uint64_t expected = (1ULL << 16) - 1;
    EXPECT_EQ(combined, expected);
}

TEST(BitPartitionerTest, ShrAtomBitRedirection) {
    // (x & 0xF0) >> 4 at bitwidth=8:
    // bits 0-3 of result come from bits 4-7 of child.
    // Child (x & 0xF0): bits 4-7 are identity(x), 0-3 are zero.
    // After >> 4: bits 0-3 are identity(x), bits 4-7 are zero.
    SemilinearIR ir;
    ir.bitwidth = 8;
    ir.constant = 0;
    auto atom = Expr::LogicalShr(Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xF0)), 4);
    AtomKey key{ { 0 }, {} };
    ir.atom_table.push_back(AtomInfo{ 0, key, std::move(atom), 0, OperatorFamily::kMixed });
    ir.terms.push_back({ 1, 0 });

    auto partitions = ComputePartitions(ir);
    EXPECT_EQ(partitions.size(), 2u);

    bool found_active = false;
    bool found_zero   = false;
    for (const auto &pc : partitions) {
        if (pc.mask == 0x0F) {
            found_active = true;
        }
        if (pc.mask == 0xF0) {
            found_zero = true;
        }
    }
    EXPECT_TRUE(found_active);
    EXPECT_TRUE(found_zero);
}

TEST(BitPartitionerTest, ShrBeyondBitwidthIsZero) {
    // x >> 7 at bitwidth=8: bit 0 comes from bit 7 of x (identity).
    // bits 1-7: src bit >= 8, so all zero.
    SemilinearIR ir;
    ir.bitwidth = 8;
    ir.constant = 0;
    auto atom   = Expr::LogicalShr(Expr::Variable(0), 7);
    AtomKey key{ { 0 }, {} };
    ir.atom_table.push_back(AtomInfo{ 0, key, std::move(atom), 0, OperatorFamily::kMixed });
    ir.terms.push_back({ 1, 0 });

    auto partitions = ComputePartitions(ir);
    EXPECT_EQ(partitions.size(), 2u);

    bool found_bit0 = false;
    for (const auto &pc : partitions) {
        if (pc.mask == 0x01) {
            found_bit0 = true;
        }
    }
    EXPECT_TRUE(found_bit0);
}
