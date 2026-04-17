#include "cobra/core/ExtensionLowering.h"

#include "cobra/core/BitWidth.h"

#include <cassert>

namespace cobra {
    namespace {

        struct ExtMasks
        {
            uint64_t low_mask;
            uint64_t sign_bit;
        };

        ExtMasks ComputeExtMasks(uint32_t source_bits) {
            assert(source_bits >= 1 && source_bits <= 64);
            return ExtMasks{
                .low_mask = Bitmask(source_bits),
                .sign_bit = 1ULL << (source_bits - 1),
            };
        }

    } // anonymous namespace

    uint64_t EvalZeroExtend(uint64_t val, uint32_t source_bits, uint64_t result_mask) {
        auto [low_mask, sign_bit] = ComputeExtMasks(source_bits);
        return (val & low_mask) & result_mask;
    }

    uint64_t EvalSignExtend(uint64_t val, uint32_t source_bits, uint64_t result_mask) {
        auto [low_mask, sign_bit] = ComputeExtMasks(source_bits);
        uint64_t masked           = val & low_mask;
        return ((masked ^ sign_bit) - sign_bit) & result_mask;
    }

    std::unique_ptr< Expr >
    LowerZeroExtend(std::unique_ptr< Expr > inner, uint32_t source_bits) {
        assert(source_bits >= 1 && source_bits <= 64);
        if (source_bits == 64) { return inner; }

        auto [low_mask, sign_bit] = ComputeExtMasks(source_bits);
        return Expr::BitwiseAnd(std::move(inner), Expr::Constant(low_mask));
    }

    std::unique_ptr< Expr >
    LowerSignExtend(std::unique_ptr< Expr > inner, uint32_t source_bits) {
        assert(source_bits >= 1 && source_bits <= 64);
        if (source_bits == 64) { return inner; }

        auto [low_mask, sign_bit] = ComputeExtMasks(source_bits);
        auto masked = Expr::BitwiseAnd(std::move(inner), Expr::Constant(low_mask));
        auto xored  = Expr::BitwiseXor(std::move(masked), Expr::Constant(sign_bit));
        return Expr::Add(std::move(xored), Expr::Negate(Expr::Constant(sign_bit)));
    }

} // namespace cobra
