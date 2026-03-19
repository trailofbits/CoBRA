#pragma once

#include <cstdint>

namespace cobra {

    inline uint64_t Bitmask(uint32_t bitwidth) {
        if (bitwidth >= 64) { return UINT64_MAX; }
        return (1ULL << bitwidth) - 1;
    }

    inline uint64_t ModAdd(uint64_t a, uint64_t b, uint32_t bitwidth) {
        return (a + b) & Bitmask(bitwidth);
    }

    inline uint64_t ModSub(uint64_t a, uint64_t b, uint32_t bitwidth) {
        return (a - b) & Bitmask(bitwidth);
    }

    inline uint64_t ModMul(uint64_t a, uint64_t b, uint32_t bitwidth) {
        return (a * b) & Bitmask(bitwidth);
    }

    inline uint64_t ModNeg(uint64_t a, uint32_t bitwidth) { return ModSub(0, a, bitwidth); }

    inline uint64_t ModNot(uint64_t a, uint32_t bitwidth) { return (~a) & Bitmask(bitwidth); }

    inline uint64_t ModShr(uint64_t a, uint64_t k, uint32_t bitwidth) {
        if (k >= 64) { return 0; }
        return (a >> k) & Bitmask(bitwidth);
    }

} // namespace cobra
