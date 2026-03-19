#pragma once

#include "cobra/core/Expr.h"
#include <cstdint>
#include <vector>

namespace cobra {

    using AtomId       = uint32_t;
    using GlobalVarIdx = uint32_t;

    struct AtomKey
    {
        std::vector< GlobalVarIdx > support; // ascending global order
        std::vector< uint64_t > truth_table; // exact truth table over support

        bool operator==(const AtomKey &other) const {
            return support == other.support && truth_table == other.truth_table;
        }

        bool operator!=(const AtomKey &other) const { return !(*this == other); }
    };

    struct AtomKeyHash
    {
        size_t operator()(const AtomKey &k) const;
    };

    enum class OperatorFamily { kAnd, kOr, kXor, kNot, kMixed };

    struct AtomInfo
    {
        AtomId atom_id{};
        AtomKey key;
        std::unique_ptr< Expr > original_subtree;
        uint64_t structural_hash  = 0;
        OperatorFamily provenance = OperatorFamily::kMixed;
    };

    struct WeightedAtom
    {
        uint64_t coeff;
        AtomId atom_id;
    };

    struct SemilinearIR
    {
        uint64_t constant = 0;
        std::vector< WeightedAtom > terms;
        std::vector< AtomInfo > atom_table;
        uint32_t bitwidth = 64;
    };

    using AtomSemanticId = uint64_t;

    struct PartitionClass
    {
        uint64_t mask;
        std::vector< AtomSemanticId > profile;
    };

    /// Evaluate a pure-bitwise Expr at all 2^k Boolean assignments
    /// to the given support variables. Returns truth table of length 2^k.
    /// All entries are masked to Bitmask(bitwidth).
    std::vector< uint64_t > ComputeAtomTruthTable(
        const Expr &atom, const std::vector< GlobalVarIdx > &support, uint32_t bitwidth
    );

} // namespace cobra
