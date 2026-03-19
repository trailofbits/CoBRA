#pragma once

#include "cobra/core/Expr.h"
#include "cobra/core/PackedAnf.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace cobra {

    // Compute ANF (Algebraic Normal Form) coefficients from a Boolean
    // signature vector via the Möbius transform over GF(2).
    //
    // Input: truth table of length 2^n, values interpreted mod 2.
    // Output: packed ANF coefficient bitvector of length 2^n.
    //   coefficient[m] == 1 means the monomial ∏_{i ∈ bits(m)} x_i
    //   is present in the ANF. coefficient[0] is the constant term.
    PackedAnf ComputeAnf(const std::vector< uint64_t > &sig, uint32_t num_vars);

    // Build an Expr from ANF coefficients using only XOR and AND.
    //
    // Each monomial with multiple variables becomes a chain of AND nodes.
    // The sum of monomials becomes a balanced tree of XOR nodes.
    // Ordering: increasing degree, then lexicographic mask within
    // each degree.
    std::unique_ptr< Expr > BuildAnfExpr(const PackedAnf &anf, uint32_t num_vars);

} // namespace cobra
