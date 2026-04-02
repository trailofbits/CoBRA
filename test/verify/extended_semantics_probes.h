// test/verify/extended_semantics_probes.h
//
// Extended-semantics probe suite: data structures, probe functions,
// clustering, and report rendering.  Test-scope only — not part of
// lib/core/.

#pragma once

#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classification.h"
#include "cobra/core/CoBExprBuilder.h"
#include "cobra/core/CoeffInterpolator.h"
#include "cobra/core/Evaluator.h"
#include "cobra/core/Expr.h"
#include "cobra/core/GhostResidualSolver.h"
#include "cobra/core/PassContract.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include <algorithm>
#include <cstdint>
#include <map>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace cobra::probe {

    // ═══════════════════════════════════════════════════════════
    //  Configuration and context
    // ═══════════════════════════════════════════════════════════

    struct ProbeConfig
    {
        uint32_t bitwidth                 = 64;
        uint32_t sweep_count              = 64;
        uint32_t diagonal_sample_count    = 128;
        uint32_t overlap_samples_per_pair = 32; // total budget per variable pair
        uint32_t max_dependency_bit       = 16;
    };

    struct ProbeContext
    {
        const Evaluator &residual;
        uint32_t real_vars; // FW-live count
        ProbeConfig cfg;
    };

    // ═══════════════════════════════════════════════════════════
    //  Raw observation structs
    // ═══════════════════════════════════════════════════════════

    // "Boolean-null" means: zero on all {0,1}^k inputs AND nonzero
    // at some full-width point.  Records both halves separately.
    struct BooleanNullProbeResult
    {
        bool tested                           = false;
        uint32_t boolean_sample_count         = 0;
        uint32_t boolean_zero_count           = 0;
        bool zero_on_boolean_cube             = false;
        uint32_t full_width_probe_count       = 0; // deterministic arity-safe witness bank
        bool found_nonzero_full_width_witness = false;
    };

    struct PeriodicityProbeResult
    {
        bool tested                   = false;
        uint32_t dominant_period      = 0; // 0 = none detected
        bool consistent_across_slices = false;
        double support_fraction       = 0.0;
    };

    struct PairOverlapObservation
    {
        uint32_t var_i                       = 0;
        uint32_t var_j                       = 0;
        uint32_t disjoint_sample_count       = 0;
        uint32_t overlap_sample_count        = 0;
        uint32_t residual_zero_when_disjoint = 0;
        uint32_t residual_zero_when_overlap  = 0;
    };

    struct OverlapProbeResult
    {
        bool tested = false;
        std::vector< PairOverlapObservation > pairs;
        bool suggests_overlap_sensitivity = false;
        double disjoint_zero_fraction     = 0.0;
        double overlap_zero_fraction      = 0.0;
    };

    struct PairDiagonalObservation
    {
        uint32_t var_i        = 0;
        uint32_t var_j        = 0;
        uint32_t sample_count = 0;
        uint32_t zero_count   = 0;
        bool all_zero         = false;
    };

    struct DiagonalProbeResult
    {
        bool tested                = false;
        uint32_t full_sample_count = 0;
        uint32_t full_zero_count   = 0;
        bool full_all_zero         = false;
        std::vector< PairDiagonalObservation > pairs;
    };

    struct BitDependencyProbeResult
    {
        bool tested                         = false;
        uint32_t max_bit_tested             = 0;
        uint32_t estimated_influence_radius = 0;
        bool shows_prefix_sensitivity       = false;
        bool shows_nonlocal_effects         = false;
    };

    // r(x,x)=0 for small x even if the full diagonal doesn't vanish.
    // Captures cases where carry effects only appear at larger magnitudes.
    struct SmallDiagonalProbeResult
    {
        bool tested              = false;
        uint32_t sample_count    = 0; // how many x tested
        uint32_t zero_count      = 0;
        bool all_zero            = false;
        uint64_t first_nonzero_x = 0; // smallest x where r(x,x)!=0, 0 if all zero
    };

    // Parity structure: does r behave differently based on
    // even/odd parity of inputs?
    struct ParityProbeResult
    {
        bool tested                     = false;
        uint32_t sample_count           = 0;
        uint32_t zero_when_all_even     = 0;
        uint32_t zero_when_all_odd      = 0;
        uint32_t zero_when_mixed        = 0;
        uint32_t total_all_even         = 0;
        uint32_t total_all_odd          = 0;
        uint32_t total_mixed            = 0;
        bool suggests_parity_dependence = false;
    };

    // ═══════════════════════════════════════════════════════════
    //  Univariate slice
    // ═══════════════════════════════════════════════════════════

    enum class SampleKind {
        kSmallRange,
        kOddOnly,
        kPowersOfTwo,
        kCustom,
    };

    struct UnivariateSlice
    {
        uint32_t free_var = 0;
        std::vector< uint64_t > pinned_values;
        std::vector< uint64_t > sample_points;
        std::vector< uint64_t > values;
        uint32_t detected_period = 0;
        SampleKind sample_kind   = SampleKind::kSmallRange;
    };

    // ═══════════════════════════════════════════════════════════
    //  Per-expression record
    // ═══════════════════════════════════════════════════════════

    struct ReasonInfo
    {
        ReasonCategory category = ReasonCategory::kNone;
        ReasonDomain domain     = ReasonDomain::kOrchestrator;
        uint16_t subcode        = 0;
    };

    struct ExtendedSemanticsRecord
    {
        int line_num            = 0;
        uint32_t bool_real_vars = 0;
        uint32_t fw_real_vars   = 0;
        SemanticClass semantic  = SemanticClass::kLinear;
        StructuralFlag flags    = kSfNone;

        ReasonInfo top_reason;
        std::string top_reason_message;

        ReasonInfo terminal_reason;
        std::string terminal_message;

        BooleanNullProbeResult boolean_null;
        PeriodicityProbeResult periodicity;
        OverlapProbeResult overlap;
        DiagonalProbeResult diagonal;
        SmallDiagonalProbeResult small_diagonal;
        ParityProbeResult parity;
        BitDependencyProbeResult bit_dependency;

        std::vector< UnivariateSlice > slices;
    };

    // ═══════════════════════════════════════════════════════════
    //  Cluster assignment (separate from records)
    // ═══════════════════════════════════════════════════════════

    struct ClusterAssignment
    {
        int line_num = 0;
        std::vector< std::string > tags;
        std::vector< std::string > defining_features;
    };

    struct ClusterSummary
    {
        std::string tag;
        std::vector< int > line_nums;
        uint32_t min_vars = 0;
        uint32_t max_vars = 0;
        std::vector< std::string > defining_features;
        std::vector< int > representative_lines;
    };

    // ═══════════════════════════════════════════════════════════
    //  Probe implementations
    // ═══════════════════════════════════════════════════════════

    inline BooleanNullProbeResult ProbeBooleanNull(const ProbeContext &ctx) {
        BooleanNullProbeResult result;
        result.tested = true;

        // Part 1: check all {0,1}^k boolean inputs
        uint32_t n                  = ctx.real_vars;
        uint32_t total              = 1u << n;
        result.boolean_sample_count = total;
        result.boolean_zero_count   = 0;

        for (uint32_t mask = 0; mask < total; ++mask) {
            std::vector< uint64_t > v(n, 0);
            for (uint32_t bit = 0; bit < n; ++bit) {
                if (mask & (1u << bit)) { v[bit] = 1; }
            }
            uint64_t val = ctx.residual(v);
            if (val == 0) { ++result.boolean_zero_count; }
        }
        result.zero_on_boolean_cube =
            (result.boolean_zero_count == result.boolean_sample_count);

        // Part 2: deterministic full-width witness bank.
        // Uses the same mixed-parity spirit as GhostResidualSolver,
        // but extends it to arbitrary FW-local arity so variables
        // beyond index 5 are still exercised at non-boolean values.
        constexpr uint64_t kSeeds[48] = {
            3, 4,  7,  10, 13, 18, 23, 28, 37, 42, 51, 60, 71, 80, 97, 106,
            5, 6,  11, 14, 19, 22, 29, 34, 41, 50, 59, 66, 73, 82, 91, 100,
            9, 12, 17, 20, 25, 30, 35, 40, 47, 56, 63, 70, 79, 86, 95, 102,
        };
        constexpr int kNumProbes = 8;
        const uint64_t kMask =
            (ctx.cfg.bitwidth >= 64) ? UINT64_MAX : ((1ULL << ctx.cfg.bitwidth) - 1);

        auto splitmix64 = [](uint64_t &state) -> uint64_t {
            state      += 0x9E3779B97F4A7C15ULL;
            uint64_t z  = state;
            z           = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z           = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            return z ^ (z >> 31);
        };

        result.full_width_probe_count           = 0;
        result.found_nonzero_full_width_witness = false;

        for (int p = 0; p < kNumProbes; ++p) {
            std::vector< uint64_t > v(n, 0);
            uint64_t extra_state = static_cast< uint64_t >(0xD1B54A32D192ED03ULL)
                ^ (static_cast< uint64_t >(p) * static_cast< uint64_t >(0x9E3779B97F4A7C15ULL));

            for (uint32_t i = 0; i < n; ++i) {
                uint64_t raw = 0;
                if (i < 6) {
                    raw = kSeeds[static_cast< uint32_t >(p) * 6 + i];
                } else {
                    raw = splitmix64(extra_state);
                }

                uint64_t val = raw & kMask;
                if (kMask > 1 && (val == 0 || val == 1)) {
                    val = (uint64_t{ 3 } + static_cast< uint64_t >(i)
                           + static_cast< uint64_t >(p))
                        & kMask;
                    if (val == 0 || val == 1) { val = 3ULL & kMask; }
                }
                v[i] = val;
            }
            ++result.full_width_probe_count;
            if (ctx.residual(v) != 0) { result.found_nonzero_full_width_witness = true; }
        }

        return result;
    }

    // ── Default pin families for Tier 1 ─────────────────────────
    inline std::vector< std::vector< uint64_t > > DefaultPinFamilies(uint32_t real_vars) {
        std::vector< std::vector< uint64_t > > families;
        // Small constants: pin all non-free vars to each value
        for (uint64_t c : { 0ULL, 1ULL, 2ULL, 3ULL }) {
            families.push_back(std::vector< uint64_t >(real_vars, c));
        }
        // Powers of two
        families.push_back(std::vector< uint64_t >(real_vars, 4));
        families.push_back(std::vector< uint64_t >(real_vars, 8));
        // Mixed parity
        families.push_back(std::vector< uint64_t >(real_vars, 0xFF));
        families.push_back(std::vector< uint64_t >(real_vars, 0x5555));
        return families;
    }

    // Detect period in a value sequence via brute-force check.
    // Returns 0 if no period <= max_period found.
    inline uint32_t
    DetectPeriod(const std::vector< uint64_t > &values, uint32_t max_period = 16) {
        if (values.size() < 4) { return 0; }
        for (uint32_t p = 1; p <= max_period; ++p) {
            bool is_periodic = true;
            for (size_t i = p; i < values.size(); ++i) {
                if (values[i] != values[i - p]) {
                    is_periodic = false;
                    break;
                }
            }
            if (is_periodic) { return p; }
        }
        return 0;
    }

    inline std::vector< UnivariateSlice > ProbeUnivariateSlices(
        const ProbeContext &ctx, const std::vector< std::vector< uint64_t > > &pin_families
    ) {
        std::vector< UnivariateSlice > slices;
        uint32_t n     = ctx.real_vars;
        uint32_t sweep = ctx.cfg.sweep_count;

        for (uint32_t free_var = 0; free_var < n; ++free_var) {
            for (const auto &pins : pin_families) {
                UnivariateSlice slice;
                slice.free_var      = free_var;
                slice.pinned_values = pins;
                slice.sample_kind   = SampleKind::kSmallRange;

                for (uint64_t x = 0; x < sweep; ++x) {
                    std::vector< uint64_t > v = pins;
                    v[free_var]               = x;
                    slice.sample_points.push_back(x);
                    slice.values.push_back(ctx.residual(v));
                }

                slice.detected_period = DetectPeriod(slice.values);
                slices.push_back(std::move(slice));
            }
        }

        return slices;
    }

    inline std::vector< UnivariateSlice > ProbeUnivariateSlices(const ProbeContext &ctx) {
        return ProbeUnivariateSlices(ctx, DefaultPinFamilies(ctx.real_vars));
    }

    inline PeriodicityProbeResult
    ProbePeriodicity(const std::vector< UnivariateSlice > &slices) {
        PeriodicityProbeResult result;
        result.tested = true;

        if (slices.empty()) {
            result.dominant_period = 0;
            return result;
        }

        // Count periods across all slices
        std::map< uint32_t, uint32_t > period_counts;
        uint32_t total_with_period = 0;

        for (const auto &s : slices) {
            if (s.detected_period > 0) {
                period_counts[s.detected_period]++;
                ++total_with_period;
            }
        }

        if (total_with_period == 0) {
            result.dominant_period          = 0;
            result.consistent_across_slices = false;
            result.support_fraction         = 0.0;
            return result;
        }

        // Find dominant period (most common non-zero)
        uint32_t best_period = 0;
        uint32_t best_count  = 0;
        for (const auto &[period, count] : period_counts) {
            if (count > best_count) {
                best_count  = count;
                best_period = period;
            }
        }

        result.dominant_period = best_period;
        result.support_fraction =
            static_cast< double >(best_count) / static_cast< double >(slices.size());
        result.consistent_across_slices = (best_count == total_with_period);

        return result;
    }

    inline OverlapProbeResult ProbeOverlapSensitivity(const ProbeContext &ctx) {
        OverlapProbeResult result;
        uint32_t n = ctx.real_vars;

        if (n < 2) {
            result.tested = false;
            return result;
        }
        result.tested = true;

        // `overlap_samples_per_pair` is the TOTAL sample budget for
        // each variable pair. Keep the budget roughly balanced across
        // disjoint and overlapping samples so changing the config does
        // not accidentally change the class mix.
        uint32_t spp = std::max(2u, ctx.cfg.overlap_samples_per_pair);

        struct ABPair
        {
            uint64_t a;
            uint64_t b;
        };

        std::vector< ABPair > disjoint_bank = {
            { 0x0F, 0xF0 },
            { 0x55, 0xAA },
            { 0x33, 0xCC },
            {    1,    2 },
            {    4,    3 },
            {    8,    7 },
            { 0x00, 0xFF },
            {   16,   15 },
            {    1,    0 },
            {    2,    0 },
            {    0,    3 },
            {    4,    0 },
            {    0,    5 },
            {    0,    0 },
            { 0x0F, 0x30 },
            { 0x03, 0x0C },
        };
        std::vector< ABPair > overlap_bank = {
            {    3,    3 },
            {    5,    7 },
            { 0xFF, 0xFF },
            { 0xAA, 0xAB },
            {    7,    5 },
            {   15,    9 },
            {    3,    1 },
            {    6,    2 },
            { 0x55, 0x55 },
            { 0xF0, 0xF1 },
            {   10,    2 },
            {   12,    4 },
            {    1,    1 },
            {    2,    3 },
            {    7,    3 },
            { 0x0F, 0x0F },
        };
        uint32_t want_disjoint = (spp + 1) / 2;
        uint32_t want_overlap  = spp / 2;

        uint32_t total_disjoint       = 0;
        uint32_t total_overlap        = 0;
        uint32_t total_rzero_disjoint = 0;
        uint32_t total_rzero_overlap  = 0;

        for (uint32_t i = 0; i < n; ++i) {
            for (uint32_t j = i + 1; j < n; ++j) {
                PairOverlapObservation obs;
                obs.var_i = i;
                obs.var_j = j;

                for (uint32_t t = 0; t < want_disjoint; ++t) {
                    const auto &[a, b] = disjoint_bank[t % disjoint_bank.size()];
                    std::vector< uint64_t > v(n, 3);
                    v[i]       = a;
                    v[j]       = b;
                    uint64_t r = ctx.residual(v);

                    ++obs.disjoint_sample_count;
                    if (r == 0) { ++obs.residual_zero_when_disjoint; }
                }

                for (uint32_t t = 0; t < want_overlap; ++t) {
                    const auto &[a, b] = overlap_bank[t % overlap_bank.size()];
                    std::vector< uint64_t > v(n, 3);
                    v[i]       = a;
                    v[j]       = b;
                    uint64_t r = ctx.residual(v);

                    ++obs.overlap_sample_count;
                    if (r == 0) { ++obs.residual_zero_when_overlap; }
                }

                total_disjoint       += obs.disjoint_sample_count;
                total_overlap        += obs.overlap_sample_count;
                total_rzero_disjoint += obs.residual_zero_when_disjoint;
                total_rzero_overlap  += obs.residual_zero_when_overlap;

                result.pairs.push_back(std::move(obs));
            }
        }

        result.disjoint_zero_fraction = (total_disjoint > 0)
            ? static_cast< double >(total_rzero_disjoint)
                / static_cast< double >(total_disjoint)
            : 0.0;
        result.overlap_zero_fraction  = (total_overlap > 0)
            ? static_cast< double >(total_rzero_overlap) / static_cast< double >(total_overlap)
            : 0.0;

        // Overlap-sensitive if disjoint zeros are significantly
        // more common than overlap zeros
        result.suggests_overlap_sensitivity =
            (result.disjoint_zero_fraction > result.overlap_zero_fraction + 0.3);

        return result;
    }

    inline DiagonalProbeResult ProbeDiagonalStructure(const ProbeContext &ctx) {
        DiagonalProbeResult result;
        uint32_t n       = ctx.real_vars;
        uint32_t samples = ctx.cfg.diagonal_sample_count;

        result.tested = true;

        // Full diagonal: r(x, x, ..., x)
        result.full_sample_count = samples;
        result.full_zero_count   = 0;

        for (uint64_t x = 0; x < samples; ++x) {
            std::vector< uint64_t > v(n, x);
            if (ctx.residual(v) == 0) { ++result.full_zero_count; }
        }
        result.full_all_zero = (result.full_zero_count == result.full_sample_count);

        // Pairwise diagonals: pin all but (i,j), set v_i = v_j
        if (n >= 2) {
            for (uint32_t i = 0; i < n; ++i) {
                for (uint32_t j = i + 1; j < n; ++j) {
                    PairDiagonalObservation obs;
                    obs.var_i        = i;
                    obs.var_j        = j;
                    obs.sample_count = samples;
                    obs.zero_count   = 0;

                    for (uint64_t x = 0; x < samples; ++x) {
                        std::vector< uint64_t > v(n, 3);
                        v[i] = x;
                        v[j] = x;
                        if (ctx.residual(v) == 0) { ++obs.zero_count; }
                    }
                    obs.all_zero = (obs.zero_count == obs.sample_count);

                    result.pairs.push_back(std::move(obs));
                }
            }
        }

        return result;
    }

    inline BitDependencyProbeResult ProbeBitDependency(const ProbeContext &ctx) {
        BitDependencyProbeResult result;
        result.tested = true;

        uint32_t n                        = ctx.real_vars;
        uint32_t max_bit                  = ctx.cfg.max_dependency_bit;
        result.max_bit_tested             = max_bit;
        result.estimated_influence_radius = 0;
        result.shows_prefix_sensitivity   = false;
        result.shows_nonlocal_effects     = false;

        // For each output bit k, test whether flipping a single
        // input bit above k in ONE variable at a time changes
        // output bit k.  One-variable-at-a-time perturbation
        // avoids overstating influence from correlated multi-
        // variable interactions.

        const std::vector< uint64_t > bases = {
            3, 5, 7, 0xAA, 0x55, 0xFF, 0x1234, 0xDEAD,
        };

        uint32_t max_influence = 0;

        for (uint32_t k = 0; k < max_bit; ++k) {
            uint64_t out_mask = 1ULL << k;

            for (uint64_t base : bases) {
                std::vector< uint64_t > v0(n, base);
                uint64_t r0   = ctx.residual(v0);
                uint64_t bit0 = r0 & out_mask;

                // Flip one bit in one variable at a time
                for (uint32_t var = 0; var < n; ++var) {
                    for (uint32_t delta = 1; delta <= 8; ++delta) {
                        uint32_t flip_bit = k + delta;
                        if (flip_bit >= 64) { break; }

                        uint64_t flip_mask         = 1ULL << flip_bit;
                        std::vector< uint64_t > v1 = v0;
                        v1[var]                    = base ^ flip_mask;

                        uint64_t r1   = ctx.residual(v1);
                        uint64_t bit1 = r1 & out_mask;

                        if (bit0 != bit1) {
                            result.shows_prefix_sensitivity = true;
                            if (delta > max_influence) { max_influence = delta; }
                            if (delta > 4) { result.shows_nonlocal_effects = true; }
                        }
                    }
                }
            }
        }

        result.estimated_influence_radius = max_influence;
        return result;
    }

    inline SmallDiagonalProbeResult ProbeSmallDiagonal(const ProbeContext &ctx) {
        SmallDiagonalProbeResult result;
        result.tested = true;

        uint32_t n             = ctx.real_vars;
        uint32_t limit         = 64;
        result.sample_count    = limit;
        result.zero_count      = 0;
        result.first_nonzero_x = 0;

        for (uint64_t x = 0; x < limit; ++x) {
            std::vector< uint64_t > v(n, x);
            if (ctx.residual(v) == 0) {
                ++result.zero_count;
            } else if (result.first_nonzero_x == 0 && x > 0) {
                result.first_nonzero_x = x;
            }
        }
        result.all_zero = (result.zero_count == result.sample_count);
        return result;
    }

    inline ParityProbeResult ProbeParity(const ProbeContext &ctx) {
        ParityProbeResult result;
        result.tested = true;

        uint32_t n = ctx.real_vars;

        // Sample pairs of even and odd values
        const std::vector< uint64_t > evens = { 2, 4, 6, 8, 10, 12, 14, 16 };
        const std::vector< uint64_t > odds  = { 1, 3, 5, 7, 9, 11, 13, 15 };

        for (uint64_t a : evens) {
            for (uint64_t b : evens) {
                std::vector< uint64_t > v(n, a);
                if (n >= 2) { v[1] = b; }
                ++result.total_all_even;
                if (ctx.residual(v) == 0) { ++result.zero_when_all_even; }
            }
        }

        for (uint64_t a : odds) {
            for (uint64_t b : odds) {
                std::vector< uint64_t > v(n, a);
                if (n >= 2) { v[1] = b; }
                ++result.total_all_odd;
                if (ctx.residual(v) == 0) { ++result.zero_when_all_odd; }
            }
        }

        for (uint64_t a : evens) {
            for (uint64_t b : odds) {
                std::vector< uint64_t > v(n, a);
                if (n >= 2) { v[1] = b; }
                ++result.total_mixed;
                if (ctx.residual(v) == 0) { ++result.zero_when_mixed; }
            }
        }

        result.sample_count = result.total_all_even + result.total_all_odd + result.total_mixed;

        // Parity-dependent if zero rates differ significantly
        double even_rate  = (result.total_all_even > 0)
            ? static_cast< double >(result.zero_when_all_even)
                / static_cast< double >(result.total_all_even)
            : 0.0;
        double odd_rate   = (result.total_all_odd > 0)
            ? static_cast< double >(result.zero_when_all_odd)
                / static_cast< double >(result.total_all_odd)
            : 0.0;
        double mixed_rate = (result.total_mixed > 0)
            ? static_cast< double >(result.zero_when_mixed)
                / static_cast< double >(result.total_mixed)
            : 0.0;

        double max_rate                   = std::max({ even_rate, odd_rate, mixed_rate });
        double min_rate                   = std::min({ even_rate, odd_rate, mixed_rate });
        result.suggests_parity_dependence = (max_rate - min_rate > 0.3);

        return result;
    }

    // ═══════════════════════════════════════════════════════════
    //  Clustering
    // ═══════════════════════════════════════════════════════════

    // Derives boolean-null from raw probe observations.
    inline bool IsDerivedBooleanNull(const BooleanNullProbeResult &bn) {
        return bn.zero_on_boolean_cube && bn.found_nonzero_full_width_witness;
    }

    inline std::vector< ClusterAssignment >
    AssignClusters(const std::vector< ExtendedSemanticsRecord > &records) {
        std::vector< ClusterAssignment > assignments;
        assignments.reserve(records.size());

        for (const auto &rec : records) {
            ClusterAssignment ca;
            ca.line_num = rec.line_num;

            // Periodicity tags
            // period=1 means constant on sampled slices ("slice-constant");
            // higher periods keep "periodic-N" naming.
            if (rec.periodicity.dominant_period > 0 && rec.periodicity.support_fraction >= 0.3)
            {
                std::string tag = (rec.periodicity.dominant_period == 1)
                    ? "slice-constant"
                    : "periodic-" + std::to_string(rec.periodicity.dominant_period);
                ca.tags.push_back(tag);
                ca.defining_features.push_back(
                    "period=" + std::to_string(rec.periodicity.dominant_period) + " support="
                    + std::to_string(static_cast< int >(rec.periodicity.support_fraction * 100))
                    + "%"
                );
            }

            // Diagonal vanish
            if (rec.diagonal.tested && rec.diagonal.full_all_zero) {
                ca.tags.push_back("diag-vanish");
                ca.defining_features.push_back(
                    "r(x,...,x)=0 for all " + std::to_string(rec.diagonal.full_sample_count)
                    + " samples"
                );
            }

            // Overlap sensitivity
            if (rec.overlap.tested && rec.overlap.suggests_overlap_sensitivity) {
                ca.tags.push_back("overlap-sensitive");
                ca.defining_features.push_back(
                    "disjoint_zero="
                    + std::to_string(
                        static_cast< int >(rec.overlap.disjoint_zero_fraction * 100)
                    )
                    + "% overlap_zero="
                    + std::to_string(
                        static_cast< int >(rec.overlap.overlap_zero_fraction * 100)
                    )
                    + "%"
                );
            }

            // Nonlocal carry
            if (rec.bit_dependency.tested && rec.bit_dependency.shows_nonlocal_effects) {
                ca.tags.push_back("nonlocal-carry");
                ca.defining_features.push_back(
                    "influence_radius="
                    + std::to_string(rec.bit_dependency.estimated_influence_radius)
                );
            }

            // Small-diagonal zero (carries only at larger magnitudes)
            if (rec.small_diagonal.tested && rec.small_diagonal.all_zero
                && !(rec.diagonal.tested && rec.diagonal.full_all_zero))
            {
                ca.tags.push_back("small-diag-zero");
                ca.defining_features.push_back(
                    "r(x,...,x)=0 for x<" + std::to_string(rec.small_diagonal.sample_count)
                    + " but not full range"
                );
            }

            // Parity dependence
            if (rec.parity.tested && rec.parity.suggests_parity_dependence) {
                ca.tags.push_back("parity-dependent");
                ca.defining_features.push_back(
                    "even_zero=" + std::to_string(rec.parity.zero_when_all_even) + "/"
                    + std::to_string(rec.parity.total_all_even)
                    + " odd_zero=" + std::to_string(rec.parity.zero_when_all_odd) + "/"
                    + std::to_string(rec.parity.total_all_odd)
                );
            }

            // Fallback: untagged
            if (ca.tags.empty()) {
                ca.tags.push_back("untagged");
                ca.defining_features.push_back("no probe triggered a classification");
            }

            assignments.push_back(std::move(ca));
        }

        return assignments;
    }

    inline std::vector< ClusterSummary > BuildClusterSummaries(
        const std::vector< ClusterAssignment > &assignments,
        const std::vector< ExtendedSemanticsRecord > &records
    ) {
        // Build record lookup by line_num
        std::map< int, const ExtendedSemanticsRecord * > rec_map;
        for (const auto &r : records) { rec_map[r.line_num] = &r; }

        // Collect per-tag
        std::map< std::string, ClusterSummary > by_tag;

        for (const auto &ca : assignments) {
            for (const auto &tag : ca.tags) {
                auto &cs = by_tag[tag];
                cs.tag   = tag;
                cs.line_nums.push_back(ca.line_num);

                auto it = rec_map.find(ca.line_num);
                if (it != rec_map.end()) {
                    uint32_t fv = it->second->fw_real_vars;
                    if (cs.line_nums.size() == 1) {
                        cs.min_vars = fv;
                        cs.max_vars = fv;
                    } else {
                        cs.min_vars = std::min(cs.min_vars, fv);
                        cs.max_vars = std::max(cs.max_vars, fv);
                    }
                }
            }
        }

        // Collect defining features from first member
        for (const auto &ca : assignments) {
            for (size_t i = 0; i < ca.tags.size(); ++i) {
                auto &cs = by_tag[ca.tags[i]];
                if (cs.defining_features.empty() && i < ca.defining_features.size()) {
                    cs.defining_features.push_back(ca.defining_features[i]);
                }
            }
        }

        // Pick representatives: lowest fw_real_vars members
        for (auto &[tag, cs] : by_tag) {
            auto sorted = cs.line_nums;
            std::sort(sorted.begin(), sorted.end(), [&](int a, int b) {
                auto ia     = rec_map.find(a);
                auto ib     = rec_map.find(b);
                uint32_t va = (ia != rec_map.end()) ? ia->second->fw_real_vars : 99;
                uint32_t vb = (ib != rec_map.end()) ? ib->second->fw_real_vars : 99;
                return va < vb;
            });
            for (size_t i = 0; i < std::min(sorted.size(), size_t{ 3 }); ++i) {
                cs.representative_lines.push_back(sorted[i]);
            }
        }

        std::vector< ClusterSummary > result;
        for (auto &[tag, cs] : by_tag) { result.push_back(std::move(cs)); }
        return result;
    }

    // ═══════════════════════════════════════════════════════════
    //  String conversion helpers
    // ═══════════════════════════════════════════════════════════

    inline const char *SemanticStr(SemanticClass s) {
        switch (s) {
            case SemanticClass::kLinear:
                return "Linear";
            case SemanticClass::kSemilinear:
                return "Semilinear";
            case SemanticClass::kPolynomial:
                return "Polynomial";
            case SemanticClass::kNonPolynomial:
                return "NonPoly";
        }
        return "?";
    }

    inline const char *CategoryStr(ReasonCategory c) {
        switch (c) {
            case ReasonCategory::kSearchExhausted:
                return "search-exhausted";
            case ReasonCategory::kVerifyFailed:
                return "verify-failed";
            case ReasonCategory::kRepresentationGap:
                return "rep-gap";
            case ReasonCategory::kGuardFailed:
                return "guard-failed";
            case ReasonCategory::kNoSolution:
                return "no-solution";
            case ReasonCategory::kResourceLimit:
                return "resource-limit";
            case ReasonCategory::kInapplicable:
                return "inapplicable";
            case ReasonCategory::kCostRejected:
                return "cost-rejected";
            case ReasonCategory::kInternalInvariant:
                return "internal";
            case ReasonCategory::kNone:
                return "none";
        }
        return "?";
    }

    inline const char *DomainStr(ReasonDomain d) {
        switch (d) {
            case ReasonDomain::kOrchestrator:
                return "Orchestrator";
            case ReasonDomain::kSemilinear:
                return "Semilinear";
            case ReasonDomain::kSignature:
                return "Signature";
            case ReasonDomain::kStructuralTransform:
                return "StructXform";
            case ReasonDomain::kDecomposition:
                return "Decomposition";
            case ReasonDomain::kTemplateDecomposer:
                return "TemplateDec";
            case ReasonDomain::kWeightedPolyFit:
                return "WeightedPoly";
            case ReasonDomain::kMultivarPoly:
                return "MultivarPoly";
            case ReasonDomain::kPolynomialRecovery:
                return "PolyRecovery";
            case ReasonDomain::kBitwiseDecomposer:
                return "BitwiseDec";
            case ReasonDomain::kHybridDecomposer:
                return "HybridDec";
            case ReasonDomain::kGhostResidual:
                return "GhostResidual";
            case ReasonDomain::kOperandSimplifier:
                return "OperandSimp";
            case ReasonDomain::kLifting:
                return "Lifting";
            case ReasonDomain::kVerifier:
                return "Verifier";
        }
        return "?";
    }

    inline std::string FlagStr(StructuralFlag f) {
        std::string s;
        if (HasFlag(f, kSfHasBitwise)) { s += "Bw|"; }
        if (HasFlag(f, kSfHasArithmetic)) { s += "Ar|"; }
        if (HasFlag(f, kSfHasMul)) { s += "Mul|"; }
        if (HasFlag(f, kSfHasMultilinearProduct)) { s += "Mlp|"; }
        if (HasFlag(f, kSfHasSingletonPower)) { s += "Pow|"; }
        if (HasFlag(f, kSfHasMixedProduct)) { s += "MxP|"; }
        if (HasFlag(f, kSfHasBitwiseOverArith)) { s += "BoA|"; }
        if (HasFlag(f, kSfHasArithOverBitwise)) { s += "AoB|"; }
        if (HasFlag(f, kSfHasMultivarHighPower)) { s += "HiP|"; }
        if (!s.empty()) { s.pop_back(); }
        return s;
    }

    inline std::string ReasonStr(const ReasonInfo &r) {
        std::string s;
        s += CategoryStr(r.category);
        s += "/";
        s += DomainStr(r.domain);
        s += "/";
        s += std::to_string(r.subcode);
        return s;
    }

} // namespace cobra::probe
