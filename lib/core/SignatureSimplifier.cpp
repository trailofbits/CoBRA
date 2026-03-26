#include "cobra/core/SignatureSimplifier.h"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace cobra {

    bool IsBooleanValued(const std::vector< uint64_t > &sig) {
        return std::all_of(sig.begin(), sig.end(), [](uint64_t v) { return v <= 1; });
    }

} // namespace cobra
