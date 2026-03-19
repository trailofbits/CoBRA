#pragma once

#include "cobra/core/BitWidth.h"
#include <bit>
#include <cassert>
#include <cstdint>

namespace cobra {

    // Number of factors of 2 in k! (Legendre's formula, p=2).
    // Closed form: k - popcount(k).
    constexpr uint32_t TwosInFactorial(uint32_t k) {
        return k - static_cast< uint32_t >(std::popcount(k));
    }

    // Smallest d such that TwosInFactorial(d!) >= bitwidth.
    // Concrete values: 8→10, 16→18, 32→34, 64→66.
    constexpr uint32_t DegreeCap(uint32_t bitwidth) {
        uint32_t d = 0;
        while (TwosInFactorial(d) < bitwidth) { ++d; }
        return d;
    }

    // Odd part of k! modulo 2^bitwidth.
    // Computes k! / 2^{TwosInFactorial(k)} mod 2^bitwidth
    // by multiplying the odd residues of 1..k.
    inline uint64_t OddPartFactorial(uint32_t k, uint32_t bitwidth) {
        const uint64_t kMask = Bitmask(bitwidth);
        uint64_t result      = 1;
        for (uint32_t i = 1; i <= k; ++i) {
            uint32_t x = i;
            while ((x & 1) == 0) { x >>= 1; }
            result = (result * x) & kMask;
        }
        return result;
    }

    // Modular inverse of an odd number x modulo 2^bits.
    // Hensel lifting: x*x = 1 mod 8 for all odd x, so 3 bits are
    // correct initially. Each iteration doubles the number of
    // correct bits.
    inline uint64_t ModInverseOdd(uint64_t x, uint32_t bits) {
        assert(bits >= 1);
        assert(x & 1);
        const uint64_t kModMask = (bits >= 64) ? UINT64_MAX : (1ULL << bits) - 1;
        uint64_t inv            = x & kModMask;
        for (uint32_t b = 3; b < bits; b *= 2) { inv = (inv * (2 - (x * inv))) & kModMask; }
        return inv & kModMask;
    }

} // namespace cobra
