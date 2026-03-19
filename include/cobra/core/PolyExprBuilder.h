#pragma once

#include "cobra/core/Expr.h"
#include "cobra/core/PolyIR.h"
#include "cobra/core/Result.h"
#include <memory>

namespace cobra {

    Result< std::unique_ptr< Expr > > BuildPolyExpr(const NormalizedPoly &poly);

} // namespace cobra
