#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Profile.h"
#include "cobra/core/Trace.h"
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Hardware PEXT path requires __attribute__((target("bmi2"))) +
// __builtin_cpu_supports, which only work on GCC/Clang (not clang-cl).
#if (defined(__x86_64__) || defined(__i386__)) && !defined(_MSC_VER)
    #define COBRA_X86 1 // NOLINT(cppcoreguidelines-macro-usage)
    #include <immintrin.h>
#else
    #define COBRA_X86 0 // NOLINT(cppcoreguidelines-macro-usage)
#endif

#if defined(__aarch64__) && defined(__ARM_FEATURE_SVE2_BITPERM)
    #define COBRA_SVE2 1 // NOLINT(cppcoreguidelines-macro-usage)
    #include <arm_sve.h>
#else
    #define COBRA_SVE2 0 // NOLINT(cppcoreguidelines-macro-usage)
#endif

namespace cobra {

    namespace {

        bool
        IsSpurious(const std::vector< uint64_t > &sig, uint32_t var_bit, uint32_t num_vars) {
            const size_t kLen      = 1ULL << num_vars;
            const uint32_t kStride = 1U << var_bit;

            for (size_t i = 0; i < kLen; ++i) {
                if ((i & kStride) != 0u) { continue; }
                const size_t kIWithBit = i | kStride;
                if (sig[i] != sig[kIWithBit]) { return false; }
            }
            return true;
        }

        uint64_t DetectLiveMask(const std::vector< uint64_t > &sig, uint32_t num_vars) {
            uint64_t live = 0;
            for (uint32_t v = 0; v < num_vars; ++v) {
                const bool kSpurious = IsSpurious(sig, v, num_vars);
                COBRA_TRACE(
                    "AuxVarEliminator", "IsSpurious: var_bit={} result={}", v,
                    kSpurious ? "spurious" : "live"
                );
                if (!kSpurious) { live |= (1ULL << v); }
            }
            COBRA_TRACE(
                "AuxVarEliminator", "DetectLiveMask: live_mask=0b{:b} ({}/{} live)", live,
                std::popcount(live), num_vars
            );
            return live;
        }

        uint64_t Splitmix64(uint64_t &state) {
            state      += 0x9E3779B97F4A7C15ULL;
            uint64_t z  = state;
            z           = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z           = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            return z ^ (z >> 31);
        }

        bool IsSpuriousFullWidth(
            const Evaluator &eval, uint32_t var_index, uint32_t num_vars, uint32_t bitwidth
        ) {
            constexpr uint32_t kNumSamples = 8;
            const uint64_t kMask = (bitwidth >= 64) ? UINT64_MAX : ((1ULL << bitwidth) - 1);
            uint64_t rng_state   = (static_cast< uint64_t >(var_index) * 2654435761ULL)
                + (static_cast< uint64_t >(num_vars) * 40503ULL) + 0xDEADBEEFULL;

            std::vector< uint64_t > inputs(num_vars);
            for (uint32_t s = 0; s < kNumSamples; ++s) {
                for (uint32_t v = 0; v < num_vars; ++v) {
                    inputs[v] = Splitmix64(rng_state) & kMask;
                }
                const uint64_t kVal1 = eval(inputs) & kMask;
                inputs[var_index]    = Splitmix64(rng_state) & kMask;
                const uint64_t kVal2 = eval(inputs) & kMask;
                if (kVal1 != kVal2) { return false; }
            }
            return true;
        }

        inline uint64_t PextSoft(uint64_t val, uint64_t mask) {
            uint64_t result  = 0;
            uint64_t dst_bit = 1;
            while (mask != 0u) {
                const uint64_t kLowest = mask & (0ULL - mask);
                if ((val & kLowest) != 0u) { result |= dst_bit; }
                mask     ^= kLowest;
                dst_bit <<= 1;
            }
            return result;
        }

        std::vector< uint64_t > CompactSignatureSoft(
            const std::vector< uint64_t > &sig, uint64_t live_mask, uint32_t num_vars
        ) {
            const auto kNewCount     = static_cast< uint32_t >(std::popcount(live_mask));
            const size_t kNewLen     = 1ULL << kNewCount;
            const size_t kOldLen     = 1ULL << num_vars;
            const uint64_t kDeadMask = ((1ULL << num_vars) - 1) & ~live_mask;

            std::vector< uint64_t > reduced(kNewLen);
            for (size_t i = 0; i < kOldLen; ++i) {
                if ((i & kDeadMask) != 0u) { continue; }
                reduced[PextSoft(i, live_mask)] = sig[i];
            }
            return reduced;
        }

#if COBRA_X86

        bool has_bmi2() {
            static const bool supported = __builtin_cpu_supports("bmi2");
            return supported;
        }

        __attribute__((target("bmi2"))) std::vector< uint64_t > CompactSignatureHw(
            const std::vector< uint64_t > &sig, uint64_t live_mask, uint32_t num_vars
        ) {
            uint32_t new_count = static_cast< uint32_t >(std::popcount(live_mask));
            size_t new_len     = 1ULL << new_count;
            size_t old_len     = 1ULL << num_vars;
            uint64_t dead_mask = ((1ULL << num_vars) - 1) & ~live_mask;

            std::vector< uint64_t > reduced(new_len);
            for (size_t i = 0; i < old_len; ++i) {
                if (i & dead_mask) { continue; }
                reduced[_pext_u64(i, live_mask)] = sig[i];
            }
            return reduced;
        }

#endif // COBRA_X86

#if COBRA_SVE2

        std::vector< uint64_t > CompactSignatureSve2(
            const std::vector< uint64_t > &sig, uint64_t live_mask, uint32_t num_vars
        ) {
            uint32_t new_count = static_cast< uint32_t >(std::popcount(live_mask));
            size_t new_len     = 1ULL << new_count;
            size_t old_len     = 1ULL << num_vars;
            uint64_t dead_mask = ((1ULL << num_vars) - 1) & ~live_mask;

            svbool_t lane0     = svptrue_pat_b64(SV_VL1);
            svuint64_t mask_sv = svdup_u64(live_mask);

            std::vector< uint64_t > reduced(new_len);
            for (size_t i = 0; i < old_len; ++i) {
                if (i & dead_mask) { continue; }
                svuint64_t extracted                   = svbext_u64(svdup_u64(i), mask_sv);
                reduced[svlastb_u64(lane0, extracted)] = sig[i];
            }
            return reduced;
        }

#endif // COBRA_SVE2

        std::vector< uint64_t > CompactSignature(
            const std::vector< uint64_t > &sig, uint64_t live_mask, uint32_t num_vars
        ) {
#if COBRA_X86
            if (has_bmi2()) { return CompactSignatureHw(sig, live_mask, num_vars); }
            return CompactSignatureSoft(sig, live_mask, num_vars);
#elif COBRA_SVE2
            return CompactSignatureSve2(sig, live_mask, num_vars);
#else
            return CompactSignatureSoft(sig, live_mask, num_vars);
#endif
        }

    } // namespace

    EliminationResult EliminateAuxVars(
        const std::vector< uint64_t > &sig, const std::vector< std::string > &vars
    ) {
        COBRA_ZONE_N("EliminateAuxVars");
        const auto kNumVars = static_cast< uint32_t >(vars.size());
        COBRA_TRACE("AuxVarEliminator", "EliminateAuxVars: num_vars={}", kNumVars);
        COBRA_TRACE_SIG("AuxVarEliminator", "input sig", sig);

        if (kNumVars == 0) {
            return { .reduced_sig = sig, .real_vars = {}, .spurious_vars = {} };
        }

        const uint64_t kLiveMask = DetectLiveMask(sig, kNumVars);

        EliminationResult result;
        result.reduced_sig = CompactSignature(sig, kLiveMask, kNumVars);
        for (uint32_t v = 0; v < kNumVars; ++v) {
            if ((kLiveMask & (1ULL << v)) != 0u) {
                result.real_vars.push_back(vars[v]);
            } else {
                result.spurious_vars.push_back(vars[v]);
            }
        }
#ifdef COBRA_ENABLE_TRACE
        for (const auto &sv : result.spurious_vars) {
            COBRA_TRACE("AuxVarEliminator", "  spurious: {}", sv);
        }
        for (const auto &rv : result.real_vars) {
            COBRA_TRACE("AuxVarEliminator", "  live: {}", rv);
        }
#endif
        COBRA_TRACE_SIG("AuxVarEliminator", "compacted sig", result.reduced_sig);
        return result;
    }

    EliminationResult EliminateAuxVars(
        const std::vector< uint64_t > &sig, const std::vector< std::string > &vars,
        const Evaluator &eval, uint32_t bitwidth
    ) {
        // Start with the boolean-signature-based elimination
        auto result = EliminateAuxVars(sig, vars);

        // Re-check each spurious variable at full width
        const auto kNumVars = static_cast< uint32_t >(vars.size());
        std::unordered_map< std::string, uint32_t > var_idx;
        for (uint32_t j = 0; j < kNumVars; ++j) { var_idx[vars[j]] = j; }

        std::vector< std::string > still_spurious;
        for (const auto &sv : result.spurious_vars) {
            if (IsSpuriousFullWidth(eval, var_idx.at(sv), kNumVars, bitwidth)) {
                still_spurious.push_back(sv);
                COBRA_TRACE("AuxVarEliminator", "FullWidth: {} is spurious (confirmed)", sv);
            } else {
                result.real_vars.push_back(sv);
                COBRA_TRACE("AuxVarEliminator", "FullWidth: {} is LIVE at full width", sv);
            }
        }
        result.spurious_vars = std::move(still_spurious);

        // Recompute live mask and reduced sig from the updated real_vars
        uint64_t live_mask = 0;
        for (const auto &rv : result.real_vars) { live_mask |= (1ULL << var_idx.at(rv)); }

        // Sort real_vars and spurious_vars by original index order
        auto by_original_index = [&var_idx](const std::string &a, const std::string &b) {
            return var_idx.at(a) < var_idx.at(b);
        };
        std::sort(result.real_vars.begin(), result.real_vars.end(), by_original_index);
        std::sort(result.spurious_vars.begin(), result.spurious_vars.end(), by_original_index);

        result.reduced_sig = CompactSignature(sig, live_mask, kNumVars);

        return result;
    }

} // namespace cobra
