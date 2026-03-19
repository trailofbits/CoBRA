#include "cobra/core/AnfTransform.h"
#include "cobra/core/AnfCleanup.h"
#include "cobra/core/Expr.h"
#include "cobra/core/PackedAnf.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace cobra {

    PackedAnf ComputeAnf(const std::vector< uint64_t > &sig, uint32_t num_vars) {
        const size_t kN = 1ULL << num_vars;
        PackedAnf anf(kN);

        // Pack low bits of signature into the bitvector
        for (size_t i = 0; i < kN; ++i) {
            if ((sig[i] & 1) != 0u) { anf.Set(i); }
        }

        // Packed Möbius transform (butterfly) over GF(2).
        // Intra-word stages (step < 64): each word is independent.
        // XOR the "low" bits (where index bit i is 0) into the
        // "high" positions (where index bit i is 1).
        constexpr uint64_t kMasks[] = {
            0x5555555555555555ULL, // step 1
            0x3333333333333333ULL, // step 2
            0x0F0F0F0F0F0F0F0FULL, // step 4
            0x00FF00FF00FF00FFULL, // step 8
            0x0000FFFF0000FFFFULL, // step 16
            0x00000000FFFFFFFFULL, // step 32
        };

        const uint32_t kIntra = std::min(num_vars, 6U);
        for (uint32_t i = 0; i < kIntra; ++i) {
            const size_t kShift = 1ULL << i;
            for (size_t w = 0; w < anf.WordCount(); ++w) {
                anf.Word(w) ^= (anf.Word(w) & kMasks[i]) << kShift;
            }
        }

        // Inter-word stages (step >= 64): XOR whole words
        for (uint32_t i = 6; i < num_vars; ++i) {
            const size_t kWordStep = 1ULL << (i - 6);
            for (size_t w = kWordStep; w < anf.WordCount(); w += 2 * kWordStep) {
                for (size_t k = 0; k < kWordStep; ++k) {
                    anf.Word(w + k) ^= anf.Word(w - kWordStep + k);
                }
            }
        }

        return anf;
    }

    std::unique_ptr< Expr > BuildAnfExpr(const PackedAnf &anf, uint32_t num_vars) {
        auto form = AnfForm::FromAnfCoeffs(anf, num_vars);
        return CleanupAnf(form);
    }

} // namespace cobra
