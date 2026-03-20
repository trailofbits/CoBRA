#include "cobra/core/CoeffInterpolator.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Trace.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cobra {

    std::vector< uint64_t > InterpolateCoefficients(
        std::vector< uint64_t > sig, uint32_t num_vars, uint32_t bitwidth
    ) { // NOLINT(readability-identifier-naming)
        const uint64_t mask = Bitmask(bitwidth);
        const size_t len    = sig.size();
        COBRA_TRACE(
            "CoeffInterp", "InterpolateCoefficients: vars={} bitwidth={} sig_len={}", num_vars,
            bitwidth, len
        );

        // In-place butterfly interpolation: for each variable, subtract the
        // "without this variable" entry from the "with" entry to isolate its
        // contribution. Equivalent to evaluating directly in the (1, x, y, x&y)
        // basis rather than computing a change-of-basis matrix.
        for (uint32_t var = 0; var < num_vars; ++var) {
            const uint32_t stride = 1U << var;
            for (size_t i = 0; i < len; ++i) {
                if ((i & stride) != 0u) {
                    const size_t i0 = i & ~static_cast< size_t >(stride);
                    sig[i]          = (sig[i] - sig[i0]) & mask;
                }
            }
        }
        COBRA_TRACE_SIG("CoeffInterp", "output coeffs", sig);
        return sig;
    }

} // namespace cobra
