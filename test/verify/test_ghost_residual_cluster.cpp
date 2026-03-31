// Clustering forensic for the 15 GhostResidual search-exhausted cases.
//
// For each: run Simplify to confirm unsupported, then independently
// probe the residual after CoB extraction to understand what form
// the ghost residual takes and why existing primitives miss it.
//
// Captures:
//   1. CoB candidate (boolean shadow)
//   2. Residual = original - CoB: is it boolean-null?
//   3. Residual evaluation at diagnostic points
//   4. Whether residual factors as c*(x*y - x&y) for any variable pair
//   5. Whether residual has higher-order ghost structure
//   6. GT analysis for motif clustering

#include "ExprParser.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/CoBExprBuilder.h"
#include "cobra/core/CoeffInterpolator.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/GhostResidualSolver.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/Simplifier.h"
#include <bit>
#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <iomanip>
#include <map>
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

    uint32_t count_nodes(const Expr &e) {
        uint32_t n = 1;
        for (const auto &c : e.children) { n += count_nodes(*c); }
        return n;
    }

    std::string op_kind(Expr::Kind k) {
        switch (k) {
            case Expr::Kind::kAdd:
                return "+";
            case Expr::Kind::kMul:
                return "*";
            case Expr::Kind::kNeg:
                return "neg";
            case Expr::Kind::kAnd:
                return "&";
            case Expr::Kind::kOr:
                return "|";
            case Expr::Kind::kXor:
                return "^";
            case Expr::Kind::kNot:
                return "~";
            case Expr::Kind::kConstant:
                return "C";
            case Expr::Kind::kVariable:
                return "V";
            default:
                return "?";
        }
    }

    // Collect top-level operator signature of GT expression
    std::string gt_skeleton(const Expr &e, int depth = 0) {
        if (depth > 3) { return "..."; }
        if (e.kind == Expr::Kind::kConstant) { return "C"; }
        if (e.kind == Expr::Kind::kVariable) { return "v" + std::to_string(e.var_index); }
        std::string s = op_kind(e.kind) + "(";
        for (size_t i = 0; i < e.children.size(); ++i) {
            if (i > 0) { s += ","; }
            s += gt_skeleton(*e.children[i], depth + 1);
        }
        return s + ")";
    }

    // Check if expression contains multiplication
    bool has_mul(const Expr &e) {
        if (e.kind == Expr::Kind::kMul) { return true; }
        for (const auto &c : e.children) {
            if (has_mul(*c)) { return true; }
        }
        return false;
    }

    // Check if expression has bitwise wrapping arithmetic
    bool has_bw_over_arith(const Expr &e, bool under_bw = false) {
        bool is_bw =
            (e.kind == Expr::Kind::kAnd || e.kind == Expr::Kind::kOr
             || e.kind == Expr::Kind::kXor || e.kind == Expr::Kind::kNot);
        bool is_arith =
            (e.kind == Expr::Kind::kAdd || e.kind == Expr::Kind::kMul
             || e.kind == Expr::Kind::kNeg);
        if (under_bw && is_arith) { return true; }
        for (const auto &c : e.children) {
            if (has_bw_over_arith(*c, is_bw)) { return true; }
        }
        return false;
    }

    // The 15 GhostResidual search-exhausted line numbers
    const std::vector< int > kTargetLines = { 14,  85,  95,  96,  102, 107, 115, 131,
                                              148, 152, 155, 186, 218, 246, 278 };

} // namespace

TEST(GhostResidualCluster, ResidualAnalysis) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    std::vector< std::string > lines;
    std::string line;
    while (std::getline(file, line)) { lines.push_back(line); }

    int total                 = 0;
    int residual_is_bn        = 0;
    int single_ghost_solved   = 0;
    int factored_ghost_solved = 0;
    std::map< std::string, int > gt_motifs;

    std::cerr << "\n";
    std::cerr << "═══════════════════════════════════════════════════════════\n";
    std::cerr << "  GHOST RESIDUAL CLUSTER: 15 cases\n";
    std::cerr << "  All terminal: GhostResidual/search-exhausted sub=2\n";
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

        auto folded = FoldConstantBitwise(std::move(ast.value().expr), 64);

        const auto &sig  = parse.value().sig;
        const auto &vars = parse.value().vars;
        auto nv          = static_cast< uint32_t >(vars.size());

        auto bool_elim = EliminateAuxVars(sig, vars);
        auto bool_real = static_cast< uint32_t >(bool_elim.real_vars.size());

        auto folded_ptr     = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*folded));
        Evaluator orig_eval = [folded_ptr,
                               &vars](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, 64);
        };

        auto fw_elim = EliminateAuxVars(sig, vars, orig_eval, 64);

        ++total;

        // ── CoB reconstruction ───────────────────────────────

        auto coeffs   = InterpolateCoefficients(bool_elim.reduced_sig, bool_real, 64);
        auto cob_expr = BuildCobExpr(coeffs, bool_real, 64);
        auto cob_str  = Render(*cob_expr, bool_elim.real_vars, 64);

        // ── Build residual evaluator: r(x) = f(x) - cob(x) ─

        std::vector< uint32_t > var_map;
        for (const auto &rv : bool_elim.real_vars) {
            for (uint32_t j = 0; j < nv; ++j) {
                if (vars[j] == rv) {
                    var_map.push_back(j);
                    break;
                }
            }
        }

        auto cob_clone         = CloneExpr(*cob_expr);
        Evaluator reduced_eval = [&orig_eval, &var_map,
                                  nv](const std::vector< uint64_t > &v) -> uint64_t {
            std::vector< uint64_t > full(nv, 0);
            for (size_t i = 0; i < var_map.size(); ++i) { full[var_map[i]] = v[i]; }
            return orig_eval(full);
        };

        auto cob_shared = std::make_shared< std::unique_ptr< Expr > >(std::move(cob_clone));
        Evaluator residual_eval = [reduced_eval, cob_shared,
                                   bool_real](const std::vector< uint64_t > &v) -> uint64_t {
            uint64_t f = reduced_eval(v);
            uint64_t c = EvalExpr(**cob_shared, v, 64);
            return f - c;
        };

        // ── Check boolean-null ───────────────────────────────

        auto residual_sig = EvaluateBooleanSignature(residual_eval, bool_real, 64);

        std::vector< uint32_t > support;
        for (uint32_t i = 0; i < bool_real; ++i) { support.push_back(i); }

        bool is_bn = IsBooleanNullResidual(residual_eval, support, bool_real, 64, residual_sig);
        if (is_bn) { residual_is_bn++; }

        // ── Probe residual at diagnostic points ──────────────

        struct ProbeResult
        {
            std::vector< uint64_t > input;
            uint64_t value;
        };

        std::vector< ProbeResult > probes;

        // Adversarial points
        std::vector< std::vector< uint64_t > > test_pts;
        // All-ones
        test_pts.push_back(std::vector< uint64_t >(bool_real, ~0ULL));
        // Powers of two
        for (uint32_t v = 0; v < bool_real; ++v) {
            std::vector< uint64_t > pt(bool_real, 0);
            pt[v] = 2;
            test_pts.push_back(pt);
        }
        // Small mixed
        for (uint64_t val : { 3ULL, 5ULL, 7ULL, 0xFFULL }) {
            test_pts.push_back(std::vector< uint64_t >(bool_real, val));
        }

        for (const auto &pt : test_pts) {
            uint64_t val = residual_eval(pt);
            if (val != 0) { probes.push_back({ pt, val }); }
        }

        // ── Try ghost solvers directly ───────────────────────

        bool single_ok   = false;
        bool factored_ok = false;
        if (is_bn && bool_real <= 6) {
            auto sg = SolveGhostResidual(residual_eval, support, bool_real, 64);
            if (sg.Succeeded()) {
                single_ok = true;
                single_ghost_solved++;
            }

            auto fg = SolveFactoredGhostResidual(residual_eval, support, bool_real, 64, 2, 3);
            if (fg.Succeeded()) {
                factored_ok = true;
                factored_ghost_solved++;
            }
        }

        // ── GT analysis ──────────────────────────────────────

        auto gt_ast          = ParseToAst(gt_str, 64);
        std::string skeleton = "unparsed";
        bool gt_has_mul      = false;
        bool gt_has_boa      = false;
        if (gt_ast.has_value()) {
            auto gt_folded = FoldConstantBitwise(std::move(gt_ast.value().expr), 64);
            skeleton       = gt_skeleton(*gt_folded);
            gt_has_mul     = has_mul(*gt_folded);
            gt_has_boa     = has_bw_over_arith(*gt_folded);
        }

        // Classify the GT motif
        std::string motif;
        if (gt_has_mul && gt_has_boa) {
            motif = "mul+BoA";
        } else if (gt_has_mul) {
            motif = "mul-only";
        } else if (gt_has_boa) {
            motif = "BoA-only";
        } else {
            motif = "pure-arith";
        }
        gt_motifs[motif]++;

        // ── Report ───────────────────────────────────────────

        std::cerr << "\n────────────────────────────────────────────────────\n";
        std::cerr << "L" << target_line << " real_vars=" << bool_real
                  << " fw_real=" << fw_elim.real_vars.size()
                  << " nodes=" << count_nodes(*folded) << " bn=" << (is_bn ? "YES" : "no")
                  << " ghost="
                  << (single_ok         ? "single"
                          : factored_ok ? "factored"
                                        : "NONE")
                  << "\n";
        std::cerr << "  GT: " << gt_str << "\n";
        std::cerr << "  GT motif: " << motif << "\n";
        std::cerr << "  CoB: " << cob_str.substr(0, 80) << "\n";

        std::cerr << "  Residual bn=" << (is_bn ? "Y" : "N")
                  << " nonzero_probes=" << probes.size() << "\n";

        for (size_t i = 0; i < probes.size() && i < 3; ++i) {
            std::cerr << "    r([";
            for (size_t j = 0; j < probes[i].input.size(); ++j) {
                if (j > 0) { std::cerr << ","; }
                std::cerr << probes[i].input[j];
            }
            std::cerr << "]) = " << probes[i].value << "\n";
        }

        // Analyze residual 2-adic structure at all-2 point
        std::vector< uint64_t > all_two(bool_real, 2);
        uint64_t r_at_2 = residual_eval(all_two);
        if (r_at_2 != 0) {
            auto tz = std::countr_zero(r_at_2);
            std::cerr << "  r(all=2)=" << r_at_2 << " trailing_zeros=" << tz << "\n";
        }

        // Check each ghost primitive manually for diagnostics
        for (uint32_t i = 0; i < bool_real; ++i) {
            for (uint32_t j = i + 1; j < bool_real; ++j) {
                // mul_sub_and(v_i, v_j) = v_i*v_j - (v_i & v_j)
                std::vector< uint64_t > pt(bool_real, 3);
                uint64_t ghost_val = (pt[i] * pt[j]) - (pt[i] & pt[j]);
                uint64_t r_val     = residual_eval(pt);
                if (ghost_val != 0 && r_val != 0) {
                    // Check if r_val is a multiple of ghost_val
                    uint64_t ratio = 0;
                    bool divides   = false;
                    if ((r_val % ghost_val) == 0) {
                        ratio   = r_val / ghost_val;
                        divides = true;
                    }
                    if (divides && ratio != 0) {
                        // Verify at another point
                        std::vector< uint64_t > pt2(bool_real, 5);
                        uint64_t g2     = (pt2[i] * pt2[j]) - (pt2[i] & pt2[j]);
                        uint64_t r2     = residual_eval(pt2);
                        bool consistent = (g2 != 0) && ((ratio * g2) == r2);
                        if (consistent) {
                            std::cerr << "  ** Near-match: " << ratio << " * (v" << i << "*v"
                                      << j << " - v" << i << "&v" << j << ") at pt=3,5\n";
                        }
                    }
                }
            }
        }
    }

    // ── Summary ─────────────────────────────────────────────

    std::cerr << "\n═══════════════════════════════════════════════════════════\n";
    std::cerr << "  SUMMARY\n";
    std::cerr << "  Total: " << total << "\n";
    std::cerr << "  Residual is boolean-null: " << residual_is_bn << "/" << total << "\n";
    std::cerr << "  Single ghost solved: " << single_ghost_solved << "/" << total << "\n";
    std::cerr << "  Factored ghost solved (d<=2): " << factored_ghost_solved << "/" << total
              << "\n";
    std::cerr << "\n  GT motif distribution:\n";
    for (const auto &[m, cnt] : gt_motifs) { std::cerr << "    " << m << ": " << cnt << "\n"; }
    std::cerr << "═══════════════════════════════════════════════════════════\n";
    std::cerr << "\n";

    EXPECT_EQ(total, 15);
}
