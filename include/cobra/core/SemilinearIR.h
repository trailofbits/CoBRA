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

    /// Structural hash for Expr trees (stable basis grouping).
    uint64_t StructuralHash(const Expr &expr);

    /// Decomposition of an atom into (basis_expr & constant_mask) form.
    struct Decomposed
    {
        bool valid          = false;
        const Expr *basis   = nullptr;
        uint64_t mask       = 0;
        uint64_t basis_hash = 0;
    };

    /// Decompose an atom into (basis_expr & constant_mask) form.
    /// Returns invalid for opaque atoms that don't fit this pattern.
    Decomposed DecomposeAtom(const AtomInfo &info, uint64_t modmask);

    /// Collect variable indices from an expression tree.
    void CollectVarsFromExpr(const Expr &expr, std::vector< GlobalVarIdx > &out);

    /// Create a new atom entry in the IR and return its ID.
    AtomId
    CreateAtom(SemilinearIR &ir, std::unique_ptr< Expr > subtree, OperatorFamily provenance);

    /// Remove unreferenced atoms from the atom table and remap term IDs.
    /// Call after rewrite passes and before ComputePartitions to avoid
    /// paying partitioning cost for dead intermediate atoms.
    void CompactAtomTable(SemilinearIR &ir);

} // namespace cobra

template<>
struct std::hash< cobra::AtomKey >
{
    size_t operator()(const cobra::AtomKey &k) const;
};
