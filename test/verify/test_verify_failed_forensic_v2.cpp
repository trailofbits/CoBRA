// Forensic audit of the 8 NonPoly verify-failed QSynth expressions.
//
// For each expression:
//   1. CoB reconstruction — identify AND monomials
//   2. AND→MUL conversion — does replacing AND products with MUL verify?
//   3. Direct polynomial recovery — would it work if CoB didn't short-circuit?
//   4. First divergent full-width probe — what breaks?
//
// Goal: answer Q1 (what predicate identifies this cluster?) and
//       Q2 (what path should they take instead?).

#include "ExprParser.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/CoBExprBuilder.h"
#include "cobra/core/CoeffInterpolator.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/MultivarPolyRecovery.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/Simplifier.h"
#include <bit>
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

    const char *semantic_str(SemanticClass s) {
        switch (s) {
            case SemanticClass::kLinear:
                return "Linear";
            case SemanticClass::kSemilinear:
                return "Semilinear";
            case SemanticClass::kPolynomial:
                return "Polynomial";
            case SemanticClass::kNonPolynomial:
                return "NonPoly";
        }
        return "?";
    }

    std::string flag_str(StructuralFlag f) {
        std::string s;
        if (HasFlag(f, kSfHasBitwise)) { s += "Bw|"; }
        if (HasFlag(f, kSfHasArithmetic)) { s += "Ar|"; }
        if (HasFlag(f, kSfHasMul)) { s += "Mul|"; }
        if (HasFlag(f, kSfHasMultilinearProduct)) { s += "Mlp|"; }
        if (HasFlag(f, kSfHasSingletonPower)) { s += "Pow|"; }
        if (HasFlag(f, kSfHasMixedProduct)) { s += "MxP|"; }
        if (HasFlag(f, kSfHasBitwiseOverArith)) { s += "BoA|"; }
        if (HasFlag(f, kSfHasArithOverBitwise)) { s += "AoB|"; }
        if (HasFlag(f, kSfHasMultivarHighPower)) { s += "HiP|"; }
        if (!s.empty()) { s.pop_back(); }
        return s;
    }

    uint32_t count_nodes(const Expr &e) {
        uint32_t n = 1;
        for (const auto &c : e.children) { n += count_nodes(*c); }
        return n;
    }

    // Build MUL-product (arithmetic) instead of AND-product (bitwise)
    // for a given variable mask.
    std::unique_ptr< Expr > build_mul_product(uint64_t mask) {
        std::unique_ptr< Expr > result;
        for (uint32_t bit = 0; bit < 32; ++bit) {
            if ((mask >> bit) & 1) {
                auto var = Expr::Variable(bit);
                if (result) {
                    result = Expr::Mul(std::move(result), std::move(var));
                } else {
                    result = std::move(var);
                }
            }
        }
        if (!result) { result = Expr::Constant(1); }
        return result;
    }

    // Apply coefficient: coeff * expr (mod 2^bw)
    std::unique_ptr< Expr >
    apply_coeff(std::unique_ptr< Expr > expr, uint64_t coeff, uint32_t bw) {
        if (coeff == 1) { return expr; }
        uint64_t mask = (bw < 64) ? ((1ULL << bw) - 1) : ~0ULL;
        uint64_t neg1 = mask;
        if (coeff == neg1) { return Expr::Negate(std::move(expr)); }
        return Expr::Mul(Expr::Constant(coeff), std::move(expr));
    }

    // Build polynomial expression using MUL products from CoB coefficients
    std::unique_ptr< Expr > build_mul_expr(const std::vector< uint64_t > &coeffs, uint32_t bw) {
        std::unique_ptr< Expr > result;
        if (coeffs[0] != 0) { result = Expr::Constant(coeffs[0]); }

        for (size_t i = 1; i < coeffs.size(); ++i) {
            if (coeffs[i] == 0) { continue; }
            auto pc = std::popcount(static_cast< unsigned >(i));
            std::unique_ptr< Expr > term;
            if (pc == 1) {
                // Single variable — AND and MUL are identical
                term = BuildAndProduct(static_cast< uint64_t >(i));
            } else {
                // Multi-variable — use MUL instead of AND
                term = build_mul_product(static_cast< uint64_t >(i));
            }
            term = apply_coeff(std::move(term), coeffs[i], bw);
            if (result) {
                result = Expr::Add(std::move(result), std::move(term));
            } else {
                result = std::move(term);
            }
        }
        if (!result) { result = Expr::Constant(0); }
        return result;
    }

    // The 8 NonPoly verify-failed line numbers
    const std::vector< int > kTargetLines = { 17, 154, 181, 272, 331, 350, 355, 366 };

} // namespace

TEST(VerifyFailedForensicV2, NonPolyCoB) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    std::vector< std::string > lines;
    std::string line;
    while (std::getline(file, line)) { lines.push_back(line); }

    int and_mul_fixed  = 0;
    int poly_direct_ok = 0;
    int total          = 0;

    std::cerr << "\n";
    std::cerr << "═══════════════════════════════════════════════════════════\n";
    std::cerr << "  VERIFY-FAILED FORENSIC: 8 NonPoly CoB Cases\n";
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
        auto cls    = ClassifyStructural(*folded);
        auto elim   = EliminateAuxVars(parse.value().sig, parse.value().vars);

        const auto &vars        = parse.value().vars;
        const auto &real_vars   = elim.real_vars;
        const auto &reduced_sig = elim.reduced_sig;
        auto nv                 = static_cast< uint32_t >(real_vars.size());
        auto nv_orig            = static_cast< uint32_t >(vars.size());

        ++total;

        std::cerr << "\n────────────────────────────────────────────────────\n";
        std::cerr << "L" << target_line << " sem=" << semantic_str(cls.semantic)
                  << " real_vars=" << nv << "/" << nv_orig << " flags={" << flag_str(cls.flags)
                  << "}"
                  << " nodes=" << count_nodes(*folded) << "\n";
        std::cerr << "  GT: " << gt_str << "\n";

        // ── 1. CoB reconstruction ────────────────────────────

        auto coeffs   = InterpolateCoefficients(reduced_sig, nv, 64);
        auto cob_expr = BuildCobExpr(coeffs, nv, 64);
        auto cob_str  = Render(*cob_expr, real_vars, 64);

        std::cerr << "  CoB: " << cob_str.substr(0, 120) << "\n";

        // Identify nonlinear AND monomials
        int linear_terms    = 0;
        int nonlinear_terms = 0;
        std::vector< std::string > and_monomials;
        for (size_t i = 1; i < coeffs.size(); ++i) {
            if (coeffs[i] == 0) { continue; }
            auto pc = std::popcount(static_cast< unsigned >(i));
            if (pc == 1) {
                linear_terms++;
            } else {
                nonlinear_terms++;
                std::string monomial;
                for (uint32_t bit = 0; bit < nv; ++bit) {
                    if ((i >> bit) & 1) {
                        if (!monomial.empty()) { monomial += "&"; }
                        monomial += real_vars[bit];
                    }
                }
                monomial += " [coeff=" + std::to_string(coeffs[i]) + "]";
                and_monomials.push_back(monomial);
            }
        }

        std::cerr << "  CoB terms: " << linear_terms << " linear, " << nonlinear_terms
                  << " nonlinear AND-monomials\n";
        for (const auto &m : and_monomials) { std::cerr << "    AND: " << m << "\n"; }

        // ── 2. Full-width verification of CoB vs original ───

        // Build evaluator mapped to reduced variable space
        auto folded_ptr     = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*folded));
        Evaluator orig_eval = [folded_ptr,
                               &vars](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, 64);
        };

        // Map reduced→original variable indices
        std::vector< uint32_t > var_map;
        for (const auto &rv : real_vars) {
            for (uint32_t j = 0; j < nv_orig; ++j) {
                if (vars[j] == rv) {
                    var_map.push_back(j);
                    break;
                }
            }
        }

        // Build mapped evaluator for reduced space
        Evaluator reduced_eval = [&orig_eval, &var_map,
                                  nv_orig](const std::vector< uint64_t > &v) -> uint64_t {
            std::vector< uint64_t > full(nv_orig, 0);
            for (size_t i = 0; i < var_map.size(); ++i) { full[var_map[i]] = v[i]; }
            return orig_eval(full);
        };

        auto cob_fw = FullWidthCheckEval(reduced_eval, nv, *cob_expr, 64);
        std::cerr << "  CoB FW check: " << (cob_fw.passed ? "PASS" : "FAIL") << "\n";

        if (!cob_fw.passed && !cob_fw.failing_input.empty()) {
            // Show the first divergence
            uint64_t orig_val = reduced_eval(cob_fw.failing_input);
            uint64_t cob_val  = EvalExpr(*cob_expr, cob_fw.failing_input, 64);
            std::cerr << "    diverges at [";
            for (size_t i = 0; i < cob_fw.failing_input.size(); ++i) {
                if (i > 0) { std::cerr << ", "; }
                std::cerr << real_vars[i] << "=0x" << std::hex << cob_fw.failing_input[i]
                          << std::dec;
            }
            std::cerr << "]\n";
            std::cerr << "    original=" << orig_val << " CoB=" << cob_val
                      << " delta=" << (orig_val - cob_val) << "\n";
        }

        // ── 3. AND→MUL conversion ──────────────────────────

        if (nonlinear_terms > 0) {
            auto mul_expr = build_mul_expr(coeffs, 64);
            auto mul_str  = Render(*mul_expr, real_vars, 64);
            std::cerr << "  MUL expr: " << mul_str.substr(0, 120) << "\n";

            auto mul_fw = FullWidthCheckEval(reduced_eval, nv, *mul_expr, 64);
            std::cerr << "  MUL FW check: " << (mul_fw.passed ? "PASS <<<" : "FAIL") << "\n";

            if (mul_fw.passed) {
                ++and_mul_fixed;
                auto cost = ComputeCost(*mul_expr);
                std::cerr << "    cost: wsize=" << cost.cost.weighted_size
                          << " mul=" << cost.cost.nonlinear_mul_count
                          << " depth=" << cost.cost.max_depth << "\n";
            } else if (!mul_fw.failing_input.empty()) {
                uint64_t orig_val = reduced_eval(mul_fw.failing_input);
                uint64_t mul_val  = EvalExpr(*mul_expr, mul_fw.failing_input, 64);
                std::cerr << "    MUL diverges: original=" << orig_val << " mul=" << mul_val
                          << "\n";
            }
        }

        // ── 4. Direct polynomial recovery ───────────────────

        // Full-width aux-var elimination for polynomial recovery
        auto fw_elim       = EliminateAuxVars(parse.value().sig, vars, orig_eval, 64);
        auto fw_real_count = static_cast< uint32_t >(fw_elim.real_vars.size());
        auto fw_support    = BuildVarSupport(vars, fw_elim.real_vars);

        std::cerr << "  FW real vars: " << fw_real_count << " (bool=" << nv << ")\n";

        if (fw_real_count <= 6) {
            // Try degree 2, 3, 4
            for (uint8_t deg = 2; deg <= 4; ++deg) {
                auto poly = RecoverAndVerifyPoly(orig_eval, fw_support, nv_orig, 64, deg, deg);
                if (poly.Succeeded()) {
                    auto pr        = poly.TakePayload();
                    auto poly_str  = Render(*pr.expr, fw_elim.real_vars, 64);
                    auto poly_cost = ComputeCost(*pr.expr);
                    std::cerr << "  Poly(d=" << static_cast< int >(deg)
                              << ") VERIFIED: " << poly_str.substr(0, 100) << "\n";
                    std::cerr << "    cost: wsize=" << poly_cost.cost.weighted_size
                              << " mul=" << poly_cost.cost.nonlinear_mul_count
                              << " depth=" << poly_cost.cost.max_depth << "\n";
                    ++poly_direct_ok;
                    break;
                }
                std::cerr << "  Poly(d=" << static_cast< int >(deg) << ") failed\n";
            }
        } else {
            std::cerr << "  Poly: SKIPPED (fw_real_vars=" << fw_real_count << " > 6)\n";
        }
    }

    // ── Summary ─────────────────────────────────────────────

    std::cerr << "\n═══════════════════════════════════════════════════════════\n";
    std::cerr << "  SUMMARY\n";
    std::cerr << "  Total: " << total << "\n";
    std::cerr << "  AND→MUL fixes: " << and_mul_fixed << "/" << total << "\n";
    std::cerr << "  Direct poly recovery: " << poly_direct_ok << "/" << total << "\n";
    std::cerr << "═══════════════════════════════════════════════════════════\n";
    std::cerr << "\n";

    // We expect all 8 to be present
    EXPECT_EQ(total, 8);
}
