#pragma once

#include <cstdint>

namespace cobra {

    enum class SemanticClass { kLinear, kSemilinear, kPolynomial, kNonPolynomial };

    enum StructuralFlag : uint32_t {
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

    enum class Route {
        kBitwiseOnly,
        kMultilinear,
        kPowerRecovery,
        kMixedRewrite,
        kUnsupported,
    };

    struct Classification
    {
        SemanticClass semantic;
        StructuralFlag flags;
        Route route;
    };

    // Route derivation from flags (used by classifier and rewrite loop)
    inline Route DeriveRoute(StructuralFlag flags) {
        if (HasFlag(flags, kSfHasUnknownShape)) {
            return Route::kUnsupported;
        }
        if (HasFlag(flags, kSfHasBitwiseOverArith) && HasFlag(flags, kSfHasMul)) {
            return Route::kMixedRewrite;
        }
        if (HasFlag(flags, kSfHasMixedProduct)) {
            return Route::kMixedRewrite;
        }
        // kSfHasMultivarHighPower is an informational flag, not an
        // automatic Unsupported. Pure polynomial multivar high power
        // (no hybrid blockers) is handled by the supported pipeline
        // via PowerRecovery.
        if (HasFlag(flags, kSfHasSingletonPower) || HasFlag(flags, kSfHasMultivarHighPower)) {
            return Route::kPowerRecovery;
        }
        if (HasFlag(flags, kSfHasMultilinearProduct)) {
            return Route::kMultilinear;
        }
        return Route::kBitwiseOnly;
    }

} // namespace cobra
