#include "cobra/core/SignatureVector.h"
#include "cobra/core/BitWidth.h"
#include <cstdint>
#include <vector>

namespace cobra {

    SignatureVector::SignatureVector(uint32_t num_vars, uint32_t bitwidth)
        : num_vars_(num_vars), bitwidth_(bitwidth), mask_(Bitmask(bitwidth)) {}

    // NOLINTNEXTLINE(readability-identifier-naming)
    std::vector< uint64_t > SignatureVector::FromValues(std::vector< uint64_t > values) const {
        for (auto &v : values) { v &= mask_; }
        return values;
    }

} // namespace cobra
