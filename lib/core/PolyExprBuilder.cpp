#include "cobra/core/PolyExprBuilder.h"
#include "cobra/core/BasisTransform.h"
#include "cobra/core/ExponentTuple.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/PolyIR.h"
#include "cobra/core/Result.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cobra {

    Result< std::unique_ptr< Expr > > BuildPolyExpr(const NormalizedPoly &poly) {
        const uint8_t n = poly.num_vars;
        if (n > kMaxPolyVars) {
            return Err< std::unique_ptr< Expr > >(
                CobraError::kTooManyVariables,
                "build_poly_expr: num_vars (" + std::to_string(n) + ") exceeds kMaxPolyVars ("
                    + std::to_string(kMaxPolyVars) + ")"
            );
        }
        const uint32_t w = poly.bitwidth;

        if (poly.coeffs.empty()) { return Ok(Expr::Constant(0)); }

        // Step 1: Apply C_3 per variable (factorial -> monomial)
        CoeffMap current = ToMonomialBasis(poly.coeffs, n, w);

        // Step 2: Sort terms for deterministic output
        std::vector< std::pair< ExponentTuple, Coeff > > sorted(current.begin(), current.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
            return a.first < b.first;
        });

        // Step 3: Build Expr for each monomial term
        std::vector< std::unique_ptr< Expr > > term_exprs;
        for (const auto &[tuple, coeff] : sorted) {
            uint8_t exps[kMaxPolyVars];
            tuple.ToExponents(exps, n);

            std::unique_ptr< Expr > product;
            for (uint8_t i = 0; i < n; ++i) {
                if (exps[i] == 0) { continue; }
                auto var = Expr::Variable(i);
                if (exps[i] == 2) { var = Expr::Mul(Expr::Variable(i), std::move(var)); }
                if (product) {
                    product = Expr::Mul(std::move(product), std::move(var));
                } else {
                    product = std::move(var);
                }
            }

            if (!product) {
                // Constant term (all exponents zero)
                term_exprs.push_back(Expr::Constant(coeff));
            } else {
                term_exprs.push_back(ApplyCoefficient(std::move(product), coeff, w));
            }
        }

        // Step 4: Build balanced add tree via pairwise reduction
        while (term_exprs.size() > 1) {
            std::vector< std::unique_ptr< Expr > > next;
            for (size_t i = 0; i < term_exprs.size(); i += 2) {
                if (i + 1 < term_exprs.size()) {
                    next.push_back(
                        Expr::Add(std::move(term_exprs[i]), std::move(term_exprs[i + 1]))
                    );
                } else {
                    next.push_back(std::move(term_exprs[i]));
                }
            }
            term_exprs = std::move(next);
        }

        return Ok(std::move(term_exprs[0]));
    }

} // namespace cobra
