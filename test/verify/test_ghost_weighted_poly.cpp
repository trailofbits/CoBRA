// Higher-degree weighted polynomial ghost probe.
//
// Tests whether the 15 GhostResidual cases factor as:
//   r(x) = q(x) * g(x)
// where g is a ghost weight and q is a polynomial of degree 1..3.
//
// Uses:
//   - SolveFactoredGhostResidual (existing basis) at d=1,2,3
//   - Direct RecoverWeightedPoly with x^2-x weight at d=0,1,2,3
//   - Exact full-width verification for all matches

#include "ExprParser.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/CoBExprBuilder.h"
#include "cobra/core/CoeffInterpolator.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/GhostResidualSolver.h"
#include "cobra/core/PolyExprBuilder.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/WeightedPolyFit.h"
#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace cobra;

namespace {

    size_t find_separator(const std::string &line) {
        int depth         = 0;
        size_t last_comma = std::string::npos;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '(') {
                ++depth;
            } else if (line[i] == ')') {
                --depth;
            } else if (line[i] == '\t' && depth == 0) {
                return i;
            } else if (line[i] == ',' && depth == 0) {
                last_comma = i;
            }
        }
        return last_comma;
    }

    std::string trim(const std::string &s) {
        auto start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) { return ""; }
        auto end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    const std::vector< int > kTargetLines = { 14,  85,  95,  96,  102, 107, 115, 131,
                                              148, 152, 155, 186, 218, 246, 278 };

} // namespace

TEST(GhostWeightedPoly, HigherDegreeProbe) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    std::vector< std::string > lines;
    std::string line;
    while (std::getline(file, line)) { lines.push_back(line); }

    int total             = 0;
    int existing_basis_ok = 0;
    int sq_sub_weight_ok  = 0;

    std::cerr << "\n";
    std::cerr << "═══════════════════════════════════════════════════════════\n";
    std::cerr << "  WEIGHTED POLY GHOST PROBE (higher degree quotients)\n";
    std::cerr << "═══════════════════════════════════════════════════════════\n";

    for (int target_line : kTargetLines) {
        if (target_line < 1 || target_line > static_cast< int >(lines.size())) { continue; }
        const auto &raw = lines[static_cast< size_t >(target_line) - 1];
        if (raw.empty() || raw[0] == '#') { continue; }

        size_t sep = find_separator(raw);
        if (sep == std::string::npos) { continue; }

        std::string obfuscated = trim(raw.substr(0, sep));
        std::string gt_str     = trim(raw.substr(sep + 1));
        if (obfuscated.empty()) { continue; }

        auto parse = ParseAndEvaluate(obfuscated, 64);
        if (!parse.has_value()) { continue; }
        auto ast = ParseToAst(obfuscated, 64);
        if (!ast.has_value()) { continue; }

        auto folded      = FoldConstantBitwise(std::move(ast.value().expr), 64);
        const auto &sig  = parse.value().sig;
        const auto &vars = parse.value().vars;
        auto nv          = static_cast< uint32_t >(vars.size());

        auto bool_elim = EliminateAuxVars(sig, vars);
        auto bool_real = static_cast< uint32_t >(bool_elim.real_vars.size());

        auto folded_ptr     = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*folded));
        Evaluator orig_eval = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, 64);
        };

        // Build reduced-space evaluator
        std::vector< uint32_t > var_map;
        for (const auto &rv : bool_elim.real_vars) {
            for (uint32_t j = 0; j < nv; ++j) {
                if (vars[j] == rv) {
                    var_map.push_back(j);
                    break;
                }
            }
        }
        Evaluator reduced_eval = [&orig_eval, &var_map,
                                  nv](const std::vector< uint64_t > &v) -> uint64_t {
            std::vector< uint64_t > full(nv, 0);
            for (size_t i = 0; i < var_map.size(); ++i) { full[var_map[i]] = v[i]; }
            return orig_eval(full);
        };

        // Build CoB and residual evaluator
        auto coeffs   = InterpolateCoefficients(bool_elim.reduced_sig, bool_real, 64);
        auto cob_expr = BuildCobExpr(coeffs, bool_real, 64);

        auto cob_shared = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*cob_expr));
        Evaluator residual_eval = [reduced_eval,
                                   cob_shared](const std::vector< uint64_t > &v) -> uint64_t {
            return reduced_eval(v) - EvalExpr(**cob_shared, v, 64);
        };

        // Support in reduced space (0..bool_real-1)
        std::vector< uint32_t > support;
        for (uint32_t i = 0; i < bool_real; ++i) { support.push_back(i); }

        ++total;

        auto cob_str = Render(*cob_expr, bool_elim.real_vars, 64);
        std::cerr << "\n────────────────────────────────────────────────────\n";
        std::cerr << "L" << target_line << " vars=" << bool_real
                  << " CoB: " << cob_str.substr(0, 60) << "\n";
        std::cerr << "  GT: " << gt_str << "\n";

        bool solved = false;

        // ══════════════════════════════════════════════════════
        //  Part A: Existing ghost basis at degree 1, 2, 3
        //  (SolveFactoredGhostResidual with escalated degree)
        // ══════════════════════════════════════════════════════

        for (uint8_t deg = 1; deg <= 3 && !solved; ++deg) {
            uint8_t grid = deg + 1;
            // Cap grid for 3-var cases to avoid huge grids
            if (bool_real >= 3 && grid > 3) { grid = 3; }

            auto result =
                SolveFactoredGhostResidual(residual_eval, support, bool_real, 64, deg, grid);
            if (result.Succeeded()) {
                auto res      = result.TakePayload();
                auto expr_str = Render(*res.expr, bool_elim.real_vars, 64);
                // Full-width verify the recombination
                auto combined = Expr::Add(CloneExpr(*cob_expr), CloneExpr(*res.expr));
                auto check    = FullWidthCheckEval(reduced_eval, bool_real, *combined, 64);
                std::cerr << "  EXISTING BASIS d=" << static_cast< int >(deg) << " → "
                          << expr_str.substr(0, 80)
                          << " fw=" << (check.passed ? "PASS" : "FAIL") << "\n";
                if (check.passed) {
                    solved = true;
                    existing_basis_ok++;
                    std::cerr << "  ** SOLVED via existing ghost basis"
                              << " at quotient degree " << static_cast< int >(deg) << " **\n";
                }
            }
        }

        if (solved) { continue; }

        // ══════════════════════════════════════════════════════
        //  Part B: x_i^2 - x_i weight at degree 0, 1, 2, 3
        //  (Direct RecoverWeightedPoly call)
        // ══════════════════════════════════════════════════════

        for (uint32_t wi = 0; wi < bool_real && !solved; ++wi) {
            for (uint8_t deg = 0; deg <= 3 && !solved; ++deg) {
                uint8_t grid =
                    std::max(static_cast< uint8_t >(2), static_cast< uint8_t >(deg + 1));
                if (bool_real >= 3 && grid > 3) { grid = 3; }

                WeightFn weight =
                    [wi](std::span< const uint64_t > args, uint32_t /*bw*/) -> uint64_t {
                    uint64_t x = args[wi];
                    return (x * x) - x;
                };

                auto fit = RecoverWeightedPoly(
                    residual_eval, weight, support, bool_real, 64, deg, grid
                );
                if (!fit.Succeeded()) { continue; }

                auto q_expr = BuildPolyExpr(fit.Payload().poly);
                if (!q_expr.has_value()) { continue; }

                // Build: q(x) * (x_wi^2 - x_wi)
                auto sq_ghost = Expr::Add(
                    Expr::Mul(Expr::Variable(wi), Expr::Variable(wi)),
                    Expr::Negate(Expr::Variable(wi))
                );
                auto ghost_result = Expr::Mul(std::move(*q_expr), std::move(sq_ghost));

                // Full-width verify: cob + ghost_result = original
                auto combined = Expr::Add(CloneExpr(*cob_expr), CloneExpr(*ghost_result));
                auto check    = FullWidthCheckEval(reduced_eval, bool_real, *combined, 64);

                if (check.passed) {
                    auto expr_str = Render(*ghost_result, bool_elim.real_vars, 64);
                    std::cerr << "  SQ_SUB weight on " << bool_elim.real_vars[wi]
                              << " d=" << static_cast< int >(deg) << " → "
                              << expr_str.substr(0, 80) << "\n";
                    std::cerr << "  ** SOLVED via x^2-x weight **\n";
                    solved = true;
                    sq_sub_weight_ok++;
                }
            }
        }

        if (!solved) { std::cerr << "  UNSOLVED by all weighted poly probes\n"; }
    }

    // ── Summary ─────────────────────────────────────────────

    std::cerr << "\n═══════════════════════════════════════════════════════════\n";
    std::cerr << "  RESULTS\n";
    std::cerr << "  Total: " << total << "\n";
    std::cerr << "  Existing basis (d=1..3): " << existing_basis_ok << "/" << total << "\n";
    std::cerr << "  x^2-x weight (d=0..3): " << sq_sub_weight_ok << "/" << total << "\n";
    std::cerr << "  Unsolved: " << (total - existing_basis_ok - sq_sub_weight_ok) << "/"
              << total << "\n";
    std::cerr << "═══════════════════════════════════════════════════════════\n";

    EXPECT_EQ(total, 15);
}
