#pragma once

#include <cassert>
#include <cstdint>
#include <functional>

namespace cobra {

    inline constexpr uint8_t kMaxPolyVars = 20;

    inline constexpr uint32_t kPow3[] = {
        1,       3,        9,        27,        81,        243,        729,
        2187,    6561,     19683,    59049,     177147,    531441,     1594323,
        4782969, 14348907, 43046721, 129140163, 387420489, 1162261467, 3486784401U,
    };

    struct ExponentTuple
    {
        uint32_t packed; // big-endian base-3 encoding

        static ExponentTuple FromExponents(const uint8_t *exps, uint8_t num_vars) {
            assert(num_vars <= kMaxPolyVars);
            uint32_t p = 0;
            for (uint8_t i = 0; i < num_vars; ++i) {
                assert(exps[i] <= 2);
                p = (p * 3) + exps[i];
            }
            return { p };
        }

        void ToExponents(uint8_t *out, uint8_t num_vars) const {
            uint32_t p = packed;
            for (int8_t i = num_vars - 1; i >= 0; --i) {
                out[i]  = static_cast< uint8_t >(p % 3);
                p      /= 3;
            }
        }

        uint8_t ExponentAt(uint8_t var_index, uint8_t num_vars) const {
            assert(var_index < num_vars);
            assert(num_vars <= kMaxPolyVars);
            return static_cast< uint8_t >((packed / kPow3[num_vars - 1 - var_index]) % 3);
        }

        ExponentTuple WithExponent(uint8_t var_index, uint8_t new_val, uint8_t num_vars) const {
            assert(var_index < num_vars && new_val <= 2);
            const uint32_t pos = kPow3[num_vars - 1 - var_index];
            const auto old_val = static_cast< uint8_t >((packed / pos) % 3);
            return { packed - (old_val * pos) + (new_val * pos) };
        }

        uint8_t TotalDegree(uint8_t num_vars) const {
            uint8_t sum = 0;
            uint32_t p  = packed;
            for (uint8_t i = 0; i < num_vars; ++i) {
                sum += static_cast< uint8_t >(p % 3);
                p   /= 3;
            }
            return sum;
        }

        bool operator==(const ExponentTuple &o) const { return packed == o.packed; }

        bool operator<(const ExponentTuple &o) const { return packed < o.packed; }
    };

    struct ExponentTupleHash
    {
        size_t operator()(const ExponentTuple &t) const {
            return std::hash< uint32_t >{}(t.packed);
        }
    };

} // namespace cobra
