#include "cobra/core/SignatureVector.h"
#include "cobra/core/BitWidth.h"
#include <cassert>
#include <cstdint>
#include <vector>

namespace cobra {

    SignatureVector::SignatureVector(uint32_t num_vars, uint32_t bitwidth)
        : num_vars_(num_vars), bitwidth_(bitwidth), mask_(Bitmask(bitwidth)) {
        // Length() computes 1ULL << num_vars_; that shift is UB for >= 64.
        // The orchestrator's max_vars policy (default 16) keeps callers in
        // range; the assert catches misuse from tests or new callers.
        assert(num_vars < 64 && "SignatureVector::Length would be UB for num_vars >= 64");
    }

    // NOLINTNEXTLINE(readability-identifier-naming)
    std::vector< uint64_t > SignatureVector::FromValues(std::vector< uint64_t > values) const {
        for (auto &v : values) { v &= mask_; }
        return values;
    }

} // namespace cobra
