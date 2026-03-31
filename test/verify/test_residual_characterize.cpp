// Dense characterization of 2-variable GhostResidual cases.
//
// For each 2-variable residual, compute:
//   1. Univariate slices: fix one var, analyze growth/degree
//   2. Difference operators: Δ_x, Δ_y, Δ_x Δ_y
//   3. Vanishing structure: where does r(x,y)=0?
//   4. Divisibility by candidate null factors
//   5. Parity / carry structure

#include "ExprParser.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/CoBExprBuilder.h"
#include "cobra/core/CoeffInterpolator.h"
#include "cobra/core/Expr.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <iomanip>
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

    // The 2-variable GhostResidual cases
    const std::vector< int > kTargetLines = {
        14, 85, 96, 102, 107, 115, 148, 152, 186, 246, 278
    };

} // namespace

TEST(ResidualCharacterize, TwoVarDense) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    std::vector< std::string > lines;
    std::string line;
    while (std::getline(file, line)) { lines.push_back(line); }

    int total = 0;

    std::cerr << "\n";
    std::cerr << "═══════════════════════════════════════════════════════════\n";
    std::cerr << "  2-VARIABLE RESIDUAL CHARACTERIZATION\n";
    std::cerr << "═══════════════════════════════════════════════════════════\n";

    for (int target_line : kTargetLines) {
        if (target_line < 1 || target_line > static_cast< int >(lines.size())) { continue; }
        const auto &raw = lines[target_line - 1];
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
        if (bool_real != 2) { continue; }

        auto folded_ptr     = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*folded));
        Evaluator orig_eval = [folded_ptr,
                               &vars](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, 64);
        };

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

        auto coeffs   = InterpolateCoefficients(bool_elim.reduced_sig, bool_real, 64);
        auto cob_expr = BuildCobExpr(coeffs, bool_real, 64);
        auto cob_str  = Render(*cob_expr, bool_elim.real_vars, 64);

        auto cob_shared = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*cob_expr));

        // r(x,y) = f(x,y) - cob(x,y)
        auto r = [reduced_eval, cob_shared](uint64_t x, uint64_t y) -> uint64_t {
            std::vector< uint64_t > v = { x, y };
            return reduced_eval(v) - EvalExpr(**cob_shared, v, 64);
        };

        ++total;

        std::cerr << "\n════════════════════════════════════════════════════\n";
        std::cerr << "L" << target_line << " vars={" << bool_elim.real_vars[0] << ","
                  << bool_elim.real_vars[1] << "}"
                  << " CoB: " << cob_str << "\n";
        std::cerr << "  GT: " << gt_str << "\n";

        // ── 1. Dense grid r(x,y) for small values ───────────

        std::cerr << "\n  r(x,y) grid [0..7]×[0..7]:\n";
        std::cerr << "       ";
        for (int y = 0; y <= 7; ++y) { std::cerr << std::setw(8) << y; }
        std::cerr << "\n";
        for (int x = 0; x <= 7; ++x) {
            std::cerr << "  x=" << x << " ";
            for (int y = 0; y <= 7; ++y) {
                int64_t val = static_cast< int64_t >(r(x, y));
                std::cerr << std::setw(8) << val;
            }
            std::cerr << "\n";
        }

        // ── 2. Univariate slices: fix y, look at r(x, y0) ──

        std::cerr << "\n  Univariate slices r(x, y0) for x=0..15:\n";
        for (uint64_t y0 : { 2ULL, 3ULL, 5ULL }) {
            std::cerr << "    y=" << y0 << ": ";
            for (uint64_t x = 0; x <= 15; ++x) {
                int64_t val = static_cast< int64_t >(r(x, y0));
                std::cerr << val << " ";
            }
            std::cerr << "\n";
        }

        // ── 3. First differences Δ_x r(x, y0) ──────────────

        std::cerr << "\n  First diff Δ_x r(x,y) at y=3:\n    ";
        for (uint64_t x = 0; x <= 14; ++x) {
            int64_t d = static_cast< int64_t >(r(x + 1, 3) - r(x, 3));
            std::cerr << d << " ";
        }
        std::cerr << "\n";

        std::cerr << "  Second diff Δ²_x r(x,y) at y=3:\n    ";
        for (uint64_t x = 0; x <= 13; ++x) {
            int64_t d2 = static_cast< int64_t >(r(x + 2, 3) - 2 * r(x + 1, 3) + r(x, 3));
            std::cerr << d2 << " ";
        }
        std::cerr << "\n";

        // ── 4. Mixed difference Δ_x Δ_y r(x,y) ─────────────

        std::cerr << "\n  Mixed diff Δ_x Δ_y r(x,y) grid [0..7]×[0..7]:\n";
        std::cerr << "       ";
        for (int y = 0; y <= 6; ++y) { std::cerr << std::setw(8) << y; }
        std::cerr << "\n";
        for (int x = 0; x <= 6; ++x) {
            std::cerr << "  x=" << x << " ";
            for (int y = 0; y <= 6; ++y) {
                int64_t dxy = static_cast< int64_t >(
                    r(x + 1, y + 1) - r(x + 1, y) - r(x, y + 1) + r(x, y)
                );
                std::cerr << std::setw(8) << dxy;
            }
            std::cerr << "\n";
        }

        // ── 5. Vanishing structure ───────────────────────────

        std::cerr << "\n  Vanishing sets (r=0) on [0..15]²:\n";
        int zero_count              = 0;
        bool zero_on_diag           = true;
        bool zero_when_equal_parity = true;
        bool zero_when_and_zero     = true;
        for (uint64_t x = 0; x <= 15; ++x) {
            for (uint64_t y = 0; y <= 15; ++y) {
                uint64_t val = r(x, y);
                if (val == 0) {
                    zero_count++;
                } else {
                    if (x == y) { zero_on_diag = false; }
                    if ((x & 1) == (y & 1)) { zero_when_equal_parity = false; }
                    if ((x & y) == 0) { zero_when_and_zero = false; }
                }
            }
        }
        std::cerr << "    zeros: " << zero_count << "/256\n";
        std::cerr << "    zero on diagonal (x=y): " << (zero_on_diag ? "YES" : "no") << "\n";
        std::cerr << "    zero when parity(x)=parity(y): "
                  << (zero_when_equal_parity ? "YES" : "no") << "\n";
        std::cerr << "    zero when x&y=0: " << (zero_when_and_zero ? "YES" : "no") << "\n";

        // ── 6. Divisibility by candidate null factors ────────

        std::cerr << "\n  Divisibility (sampled x,y in [2..31]):\n";

        // Test: is r(x,y) always divisible by x*(x-1)?
        bool div_xx1 = true;
        bool div_yy1 = true;
        bool div_xy  = true; // x*y - (x&y)
        bool div_xmy = true; // x - y (when x != y)
        for (uint64_t x = 2; x <= 31; ++x) {
            for (uint64_t y = 2; y <= 31; ++y) {
                uint64_t val = r(x, y);
                if (val == 0) { continue; }
                uint64_t xx1 = x * (x - 1);
                uint64_t yy1 = y * (y - 1);
                uint64_t msa = x * y - (x & y);
                if (xx1 != 0 && (val % xx1) != 0) { div_xx1 = false; }
                if (yy1 != 0 && (val % yy1) != 0) { div_yy1 = false; }
                if (msa != 0 && (val % msa) != 0) { div_xy = false; }
                if (x != y) {
                    uint64_t diff = x - y;
                    if (diff != 0 && (val % diff) != 0) { div_xmy = false; }
                }
            }
        }
        std::cerr << "    div by x(x-1): " << (div_xx1 ? "YES" : "no") << "\n";
        std::cerr << "    div by y(y-1): " << (div_yy1 ? "YES" : "no") << "\n";
        std::cerr << "    div by x*y-(x&y): " << (div_xy ? "YES" : "no") << "\n";
        std::cerr << "    div by (x-y): " << (div_xmy ? "YES" : "no") << "\n";
    }

    std::cerr << "\n═══════════════════════════════════════════════════════════\n";
    std::cerr << "  Total 2-var cases characterized: " << total << "\n";
    std::cerr << "═══════════════════════════════════════════════════════════\n";

    EXPECT_GE(total, 8); // expect at least 8 of the 11 target lines are 2-var
}
