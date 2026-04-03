#pragma once

#include "MicrocodeDetector.h"

#include <cobra/core/Expr.h>

#include <memory>

namespace ida_cobra {

    std::unique_ptr< cobra::Expr >
    BuildExprFromMinsn(const minsn_t &insn, const MBACandidate &candidate);

    minsn_t *ReconstructMinsn(
        const cobra::Expr &expr, const MBACandidate &candidate,
        const std::vector< std::string > &real_vars
    );

} // namespace ida_cobra
