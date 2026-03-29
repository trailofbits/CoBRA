#pragma once

#include <cstdint>

namespace cobra {

    enum class SemanticClass { kLinear, kSemilinear, kPolynomial, kNonPolynomial };

    enum StructuralFlag : uint32_t { // NOLINT(cppcoreguidelines-use-enum-class) - bitwise
                                     // operators need unscoped
        kSfNone = 0,

        kSfHasBitwise    = 1 << 0,
        kSfHasArithmetic = 1 << 1,
        kSfHasMul        = 1 << 2,

        kSfHasMultilinearProduct = 1 << 3,
        kSfHasSingletonPower     = 1 << 4,
        kSfHasSingletonPowerGt2  = 1 << 5,

        kSfHasMixedProduct      = 1 << 6,
        kSfHasBitwiseOverArith  = 1 << 7,
        kSfHasArithOverBitwise  = 1 << 8,
        kSfHasMultivarHighPower = 1 << 9,
        kSfHasUnknownShape      = 1 << 10,
    };

    constexpr StructuralFlag kUnsupportedFlagMask = static_cast< StructuralFlag >(
        kSfHasMixedProduct | kSfHasBitwiseOverArith | kSfHasUnknownShape
    );

    inline StructuralFlag operator|(StructuralFlag a, StructuralFlag b) {
        return static_cast< StructuralFlag >(
            static_cast< uint32_t >(a) | static_cast< uint32_t >(b)
        );
    }

    inline StructuralFlag operator&(StructuralFlag a, StructuralFlag b) {
        return static_cast< StructuralFlag >(
            static_cast< uint32_t >(a) & static_cast< uint32_t >(b)
        );
    }

    inline StructuralFlag operator~(StructuralFlag a) {
        return static_cast< StructuralFlag >(~static_cast< uint32_t >(a));
    }

    inline StructuralFlag &operator|=(StructuralFlag &a, StructuralFlag b) {
        a = a | b;
        return a;
    }

    inline bool HasFlag(StructuralFlag flags, StructuralFlag f) {
        return (static_cast< uint32_t >(flags) & static_cast< uint32_t >(f)) != 0;
    }

    struct Classification
    {
        SemanticClass semantic;
        StructuralFlag flags;
    };

    // Returns true if the expression has unrecovered mixed structure
    // that XOR lowering or other structural transforms could not reduce.
    inline bool NeedsStructuralRecovery(StructuralFlag flags) {
        if (HasFlag(flags, kSfHasUnknownShape)) { return true; }
        if (HasFlag(flags, kSfHasMixedProduct)) { return true; }
        if (HasFlag(flags, kSfHasBitwiseOverArith) && HasFlag(flags, kSfHasMul)) {
            return true;
        }
        return false;
    }

    // Returns true if the expression is a candidate for folded-AST
    // exploration passes (mixed-product or bitwise-over-arith structure
    // without unknown shape).
    inline bool IsFoldedAstExplorationCandidate(StructuralFlag flags) {
        if (HasFlag(flags, kSfHasUnknownShape)) { return false; }
        return HasFlag(flags, kSfHasMixedProduct) || HasFlag(flags, kSfHasBitwiseOverArith);
    }

} // namespace cobra
