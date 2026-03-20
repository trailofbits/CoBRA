#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Trace.h"
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
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

    } // namespace

    EliminationResult EliminateAuxVars(
        const std::vector< uint64_t > &sig, const std::vector< std::string > &vars
    ) {
        const auto kNumVars = static_cast< uint32_t >(vars.size());
        COBRA_TRACE("AuxVarEliminator", "EliminateAuxVars: num_vars={}", kNumVars);
        COBRA_TRACE_SIG("AuxVarEliminator", "input sig", sig);

        if (kNumVars == 0) {
            return { .reduced_sig = sig, .real_vars = {}, .spurious_vars = {} };
        }

        const uint64_t kLiveMask = DetectLiveMask(sig, kNumVars);

        // Compact the signature vector in a single pass
        std::vector< uint64_t > reduced;
#if COBRA_X86
        if (has_bmi2()) {
            reduced = CompactSignatureHw(sig, kLiveMask, kNumVars);
        } else {
            reduced = CompactSignatureSoft(sig, kLiveMask, kNumVars);
        }
#elif COBRA_SVE2
        reduced = CompactSignatureSve2(sig, kLiveMask, kNumVars);
#else
        reduced = CompactSignatureSoft(sig, kLiveMask, kNumVars);
#endif

        EliminationResult result;
        result.reduced_sig = std::move(reduced);
        for (uint32_t v = 0; v < kNumVars; ++v) {
            if ((kLiveMask & (1ULL << v)) != 0u) {
                result.real_vars.push_back(vars[v]);
            } else {
                result.spurious_vars.push_back(vars[v]);
            }
        }
        for (const auto &sv : result.spurious_vars) {
            COBRA_TRACE("AuxVarEliminator", "  spurious: {}", sv);
        }
        for (const auto &rv : result.real_vars) {
            COBRA_TRACE("AuxVarEliminator", "  live: {}", rv);
        }
        COBRA_TRACE_SIG("AuxVarEliminator", "compacted sig", result.reduced_sig);
        return result;
    }

} // namespace cobra
