#pragma once

#include "cobra/core/MathUtils.h"
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace cobra {

    inline constexpr uint8_t kMaxPolyVars = 20;

    struct MonomialKey
    {
        std::array< uint8_t, kMaxPolyVars > exponents{};

        static MonomialKey FromExponents(const uint8_t *exps, uint8_t num_vars) {
            assert(num_vars <= kMaxPolyVars);
            MonomialKey k;
            for (uint8_t i = 0; i < num_vars; ++i) { k.exponents[i] = exps[i]; }
            return k;
        }

        void ToExponents(uint8_t *out, uint8_t num_vars) const {
            assert(num_vars <= kMaxPolyVars);
            for (uint8_t i = 0; i < num_vars; ++i) { out[i] = exponents[i]; }
        }

        uint8_t ExponentAt(uint8_t var_index) const {
            assert(var_index < kMaxPolyVars);
            return exponents[var_index];
        }

        MonomialKey WithExponent(uint8_t var_index, uint8_t new_val) const {
            assert(var_index < kMaxPolyVars);
            MonomialKey result          = *this;
            result.exponents[var_index] = new_val;
            return result;
        }

        uint32_t TotalDegree(uint8_t num_vars) const {
            assert(num_vars <= kMaxPolyVars);
            uint32_t sum = 0;
            for (uint8_t i = 0; i < num_vars; ++i) { sum += exponents[i]; }
            return sum;
        }

        uint8_t MaxDegree(uint8_t num_vars) const {
            assert(num_vars <= kMaxPolyVars);
            uint8_t mx = 0;
            for (uint8_t i = 0; i < num_vars; ++i) {
                if (exponents[i] > mx) { mx = exponents[i]; }
            }
            return mx;
        }

        uint32_t V2FactorialWeight(uint8_t num_vars) const {
            assert(num_vars <= kMaxPolyVars);
            uint32_t w = 0;
            for (uint8_t i = 0; i < num_vars; ++i) { w += TwosInFactorial(exponents[i]); }
            return w;
        }

        bool operator==(const MonomialKey &o) const { return exponents == o.exponents; }

        bool operator<(const MonomialKey &o) const { return exponents < o.exponents; }
    };

    struct MonomialKeyHash
    {
        size_t operator()(const MonomialKey &k) const {
            // FNV-1a over the 20-byte exponents array
            size_t hash = 14695981039346656037ULL;
            for (uint8_t b : k.exponents) {
                hash ^= static_cast< size_t >(b);
                hash *= 1099511628211ULL;
            }
            return hash;
        }
    };

} // namespace cobra
