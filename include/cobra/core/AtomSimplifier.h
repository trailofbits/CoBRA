#pragma once

#include "cobra/core/Expr.h"
#include "cobra/core/SemilinearIR.h"
#include <memory>

namespace cobra {

    // NOLINTNEXTLINE(readability-identifier-naming)
    std::unique_ptr< Expr > SimplifyAtom(std::unique_ptr< Expr > atom, uint32_t bitwidth = 64);
    void SimplifyStructure(SemilinearIR &ir);

} // namespace cobra
