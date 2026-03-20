#include "cobra/core/NullPolyGenerator.h"
#include "cobra/core/BasisTransform.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/MonomialKey.h"
#include "cobra/core/PolyIR.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <random>
#include <vector>

namespace cobra {

    namespace {

        // SplitMix64 finalizer for seed derivation.
        // Avoids adjacent-seed correlation in mt19937_64.
        uint64_t Splitmix64(uint64_t x) {
            x += 0x9e3779b97f4a7c15ULL;
            x  = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
            x  = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
            return x ^ (x >> 31);
        }

        // Sample a random exponent tuple with positive V2FactorialWeight (at least one exponent
        // >= 2). Tuples with zero V2FactorialWeight are always zero in the null space and must
        // be excluded.
        MonomialKey SampleTupleWithPositiveNullWeight(
            std::mt19937_64 &rng, uint8_t num_vars, uint8_t max_degree
        ) {
            // Use unsigned, not uint8_t — uniform_int_distribution<uint8_t>
            // is undefined behavior per C++17 [rand.req.genl].
            std::uniform_int_distribution< unsigned > digit_dist(0, max_degree);
            std::array< uint8_t, kMaxPolyVars > exps{};
            for (;;) {
                for (uint8_t i = 0; i < num_vars; ++i) {
                    exps[i] = static_cast< uint8_t >(digit_dist(rng));
                }
                auto candidate = MonomialKey::FromExponents(exps.data(), num_vars);
                if (candidate.V2FactorialWeight(num_vars) > 0) { return candidate; }
            }
        }

    } // anonymous namespace

    PolyIR AddNullPolynomial(const PolyIR &seed, const NullPolyConfig &config) {
        assert(seed.bitwidth >= 2 && seed.bitwidth <= 64);
        assert(seed.num_vars > 0 && seed.num_vars <= kMaxPolyVars);
        if (config.max_degree < 2) { return seed; }

        const uint8_t kN     = seed.num_vars;
        const uint32_t kW    = seed.bitwidth;
        const uint64_t kMask = Bitmask(kW);

        std::mt19937_64 rng(config.rng_seed);

        // Step 1: Generate null coefficients in factorial basis.
        // A factorial-basis coefficient is null iff it is a multiple of
        // 2^{w-q} where q = V2FactorialWeight.
        // When q >= w, every coefficient is valid (bound is 1).
        CoeffMap null_factorial;

        for (uint32_t t = 0; t < config.num_terms; ++t) {
            const MonomialKey kTuple =
                SampleTupleWithPositiveNullWeight(rng, kN, config.max_degree);

            const uint32_t q = kTuple.V2FactorialWeight(kN);

            Coeff coeff = 0;
            if (q >= kW) {
                // Unconstrained: any nonzero value
                std::uniform_int_distribution< uint64_t > dist(
                    1, (kW < 64) ? ((1ULL << kW) - 1) : UINT64_MAX
                );
                coeff = dist(rng);
            } else {
                // Constrained: multiple of 2^{kW-q}
                const uint64_t kBound   = 1ULL << (kW - q);
                const uint64_t kMaxMult = (1ULL << q) - 1; // q < kW <= 64
                std::uniform_int_distribution< uint64_t > dist(1, kMaxMult);
                coeff = (dist(rng) * kBound) & kMask;
            }

            null_factorial[kTuple] = (null_factorial[kTuple] + coeff) & kMask;
        }

        // Strip zeros from factorial map
        for (auto it = null_factorial.begin(); it != null_factorial.end();) {
            if (it->second == 0) {
                it = null_factorial.erase(it);
            } else {
                ++it;
            }
        }

        if (null_factorial.empty()) { return seed; }

        // Step 2: Convert null polynomial to monomial basis
        const CoeffMap kNullMonomial = ToMonomialBasis(null_factorial, kN, kW);

        // Step 3: Add to seed
        PolyIR result;
        result.num_vars = kN;
        result.bitwidth = kW;
        result.terms    = seed.terms;

        for (const auto &[tuple, c] : kNullMonomial) {
            result.terms[tuple] = (result.terms[tuple] + c) & kMask;
        }

        // Strip zeros
        for (auto it = result.terms.begin(); it != result.terms.end();) {
            if (it->second == 0) {
                it = result.terms.erase(it);
            } else {
                ++it;
            }
        }

        return result;
    }

    std::vector< PolyIR > GenerateEquivalentVariants(
        const PolyIR &seed, uint32_t count, const NullPolyConfig &config
    ) {
        std::vector< PolyIR > variants;
        variants.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            NullPolyConfig variant_config = config; // NOLINT(misc-const-correctness)
            variant_config.rng_seed       = Splitmix64(config.rng_seed ^ i);
            variants.push_back(AddNullPolynomial(seed, variant_config));
        }
        return variants;
    }

} // namespace cobra
