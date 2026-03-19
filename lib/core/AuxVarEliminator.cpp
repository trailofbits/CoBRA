#include "cobra/core/AuxVarEliminator.h"
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Hardware PEXT path requires __attribute__((target("bmi2"))) +
// __builtin_cpu_supports, which only work on GCC/Clang (not clang-cl).
#if (defined(__x86_64__) || defined(__i386__)) && !defined(_MSC_VER)
    #define COBRA_X86 1
    #include <immintrin.h>
#else
    #define COBRA_X86 0
#endif

namespace cobra {

    namespace {

        bool
        IsSpurious(const std::vector< uint64_t > &sig, uint32_t var_bit, uint32_t num_vars) {
            const size_t len      = 1ULL << num_vars;
            const uint32_t stride = 1U << var_bit;

            for (size_t i = 0; i < len; ++i) {
                if ((i & stride) != 0u) {
                    continue;
                }
                const size_t i_with_bit = i | stride;
                if (sig[i] != sig[i_with_bit]) {
                    return false;
                }
            }
            return true;
        }

        uint64_t DetectLiveMask(const std::vector< uint64_t > &sig, uint32_t num_vars) {
            uint64_t live = 0;
            for (uint32_t v = 0; v < num_vars; ++v) {
                if (!IsSpurious(sig, v, num_vars)) {
                    live |= (1ULL << v);
                }
            }
            return live;
        }

        inline uint64_t PextSoft(uint64_t val, uint64_t mask) {
            uint64_t result  = 0;
            uint64_t dst_bit = 1;
            while (mask != 0u) {
                const uint64_t lowest = mask & (0ULL - mask);
                if ((val & lowest) != 0u) {
                    result |= dst_bit;
                }
                mask     ^= lowest;
                dst_bit <<= 1;
            }
            return result;
        }

        std::vector< uint64_t > CompactSignatureSoft(
            const std::vector< uint64_t > &sig, uint64_t live_mask, uint32_t num_vars
        ) {
            const auto new_count     = static_cast< uint32_t >(std::popcount(live_mask));
            const size_t new_len     = 1ULL << new_count;
            const size_t old_len     = 1ULL << num_vars;
            const uint64_t dead_mask = ((1ULL << num_vars) - 1) & ~live_mask;

            std::vector< uint64_t > reduced(new_len);
            for (size_t i = 0; i < old_len; ++i) {
                if ((i & dead_mask) != 0u) {
                    continue;
                }
                reduced[PextSoft(i, live_mask)] = sig[i];
            }
            return reduced;
        }

#if COBRA_X86

        bool has_bmi2() {
            static const bool supported = __builtin_cpu_supports("bmi2");
            return supported;
        }

        __attribute__((target("bmi2"))) std::vector< uint64_t > compact_signature_hw(
            const std::vector< uint64_t > &sig, uint64_t live_mask, uint32_t num_vars
        ) {
            uint32_t new_count = static_cast< uint32_t >(std::popcount(live_mask));
            size_t new_len     = 1ULL << new_count;
            size_t old_len     = 1ULL << num_vars;
            uint64_t dead_mask = ((1ULL << num_vars) - 1) & ~live_mask;

            std::vector< uint64_t > reduced(new_len);
            for (size_t i = 0; i < old_len; ++i) {
                if (i & dead_mask) {
                    continue;
                }
                reduced[_pext_u64(i, live_mask)] = sig[i];
            }
            return reduced;
        }

#endif // COBRA_X86

    } // namespace

    EliminationResult EliminateAuxVars(
        const std::vector< uint64_t > &sig, const std::vector< std::string > &vars
    ) {
        const auto num_vars = static_cast< uint32_t >(vars.size());

        if (num_vars == 0) {
            return { .reduced_sig = sig, .real_vars = {}, .spurious_vars = {} };
        }

        const uint64_t live_mask = DetectLiveMask(sig, num_vars);

        // Compact the signature vector in a single pass
        std::vector< uint64_t > reduced;
#if COBRA_X86
        if (has_bmi2()) {
            reduced = compact_signature_hw(sig, live_mask, num_vars);
        } else {
            reduced = compact_signature_soft(sig, live_mask, num_vars);
        }
#else
        reduced = CompactSignatureSoft(sig, live_mask, num_vars);
#endif

        EliminationResult result;
        result.reduced_sig = std::move(reduced);
        for (uint32_t v = 0; v < num_vars; ++v) {
            if ((live_mask & (1ULL << v)) != 0u) {
                result.real_vars.push_back(vars[v]);
            } else {
                result.spurious_vars.push_back(vars[v]);
            }
        }
        return result;
    }

} // namespace cobra
