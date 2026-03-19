#include "cobra/core/BitPartitioner.h"
#include "cobra/core/Expr.h"
#include "cobra/core/SemilinearIR.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cobra {

    namespace {

        constexpr uint64_t kOpaqueSentinelBase = 0xDEAD000000000000ULL;

        struct ProfileHash
        {
            size_t operator()(const std::vector< AtomSemanticId > &v) const {
                size_t h = std::hash< size_t >{}(v.size());
                for (auto id : v) {
                    h ^= std::hash< uint64_t >{}(id) + 0x9e3779b9 + (h << 6) + (h >> 2);
                }
                return h;
            }
        };

        /// Evaluate a single-bit slice of an Expr tree.
        /// Constants are reduced to their bit at position `b`.
        /// Variables are looked up from the Boolean assignment.
        /// All arithmetic is 1-bit (AND/OR/XOR/NOT over {0,1}).
        uint64_t EvalAtomAtBitImpl(
            const Expr &e, const std::vector< GlobalVarIdx > &support, uint64_t assignment,
            uint32_t bit_pos, uint32_t bitwidth
        ) {
            switch (e.kind) {
                case Expr::Kind::kConstant:
                    return (e.constant_val >> bit_pos) & 1;
                case Expr::Kind::kVariable: {
                    for (size_t i = 0; i < support.size(); ++i) {
                        if (support[i] == e.var_index) { return (assignment >> i) & 1; }
                    }
                    return 0;
                }
                case Expr::Kind::kAnd:
                    return EvalAtomAtBitImpl(
                               *e.children[0], support, assignment, bit_pos, bitwidth
                           )
                        & EvalAtomAtBitImpl(
                               *e.children[1], support, assignment, bit_pos, bitwidth
                        );
                case Expr::Kind::kOr:
                    return EvalAtomAtBitImpl(
                               *e.children[0], support, assignment, bit_pos, bitwidth
                           )
                        | EvalAtomAtBitImpl(
                               *e.children[1], support, assignment, bit_pos, bitwidth
                        );
                case Expr::Kind::kXor:
                    return EvalAtomAtBitImpl(
                               *e.children[0], support, assignment, bit_pos, bitwidth
                           )
                        ^ EvalAtomAtBitImpl(
                               *e.children[1], support, assignment, bit_pos, bitwidth
                        );
                case Expr::Kind::kNot:
                    return EvalAtomAtBitImpl(
                               *e.children[0], support, assignment, bit_pos, bitwidth
                           )
                        ^ 1;
                case Expr::Kind::kShr: {
                    const uint32_t kSrc = bit_pos + static_cast< uint32_t >(e.constant_val);
                    if (kSrc >= bitwidth) { return 0; }
                    return EvalAtomAtBitImpl(
                        *e.children[0], support, assignment, kSrc, bitwidth
                    );
                }
                case Expr::Kind::kAdd:
                case Expr::Kind::kMul:
                case Expr::Kind::kNeg:
                    std::unreachable();
            }
            std::unreachable();
        }

        /// Build the 1-bit truth table for an atom at a specific
        /// bit position. Returns a packed uint64_t where bit i holds
        /// the atom's single-bit output for Boolean assignment i.
        uint64_t EvalAtomAtBit(
            const Expr &atom, const std::vector< GlobalVarIdx > &support, uint32_t bit_pos,
            uint32_t bitwidth
        ) {
            const size_t kN   = support.size();
            const size_t kLen = size_t{ 1 } << kN;
            uint64_t packed   = 0;
            for (size_t i = 0; i < kLen; ++i) {
                const uint64_t kVal  = EvalAtomAtBitImpl(atom, support, i, bit_pos, bitwidth);
                packed              |= (kVal & 1) << i;
            }
            return packed;
        }

    } // namespace

    std::vector< PartitionClass > ComputePartitions(const SemilinearIR &ir) {
        if (ir.atom_table.empty()) { return {}; }

        if (ir.bitwidth == 0 || ir.bitwidth > 64) { return {}; }

        const size_t kAtomCount = ir.atom_table.size();

        // Precompute opaque flags for large-support atoms.
        struct AtomMeta
        {
            bool opaque       = false;
            uint64_t sentinel = 0;
        };

        std::vector< AtomMeta > meta(kAtomCount);
        for (size_t a = 0; a < kAtomCount; ++a) {
            if (ir.atom_table[a].key.support.size() > 5) {
                meta[a].opaque   = true;
                meta[a].sentinel = kOpaqueSentinelBase | a;
            }
        }

        // Group bit positions by their profile vectors.
        std::unordered_map< std::vector< AtomSemanticId >, uint64_t, ProfileHash >
            profile_to_mask;

        std::vector< AtomSemanticId > profile(kAtomCount);

        for (uint32_t b = 0; b < ir.bitwidth; ++b) {
            for (size_t a = 0; a < kAtomCount; ++a) {
                if (meta[a].opaque) {
                    profile[a] = meta[a].sentinel;
                } else {
                    const auto &info = ir.atom_table[a];
                    profile[a] =
                        EvalAtomAtBit(*info.original_subtree, info.key.support, b, ir.bitwidth);
                }
            }
            profile_to_mask[profile] |= (1ULL << b);
        }

        std::vector< PartitionClass > result;
        result.reserve(profile_to_mask.size());
        for (auto &[prof, mask] : profile_to_mask) {
            result.push_back({ .mask = mask, .profile = prof });
        }
        return result;
    }

} // namespace cobra
