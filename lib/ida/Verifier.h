#pragma once

#include "MicrocodeDetector.h"

#include <cobra/core/Expr.h>

namespace ida_cobra {

    bool ProbablyEquivalent(
        const minsn_t &original, const cobra::Expr &simplified, const MBACandidate &candidate
    );

    int CountNodes(const minsn_t &insn);

} // namespace ida_cobra
