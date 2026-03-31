// Forensic audit of the 14 guard-failed QSynth expressions.
//
// All 14 fail with "too many real variables for polynomial extraction"
// (kRealCount > 6 in DecompositionEngine.cpp:205).
//
// For each expression, extract:
//   1. Named variable count (from parser)
//   2. Boolean aux-var elimination real count
//   3. Full-width aux-var elimination real count (the one the guard uses)
//   4. Whether direct polynomial recovery would work if guard bypassed
//   5. Whether the guard fires differently outside the orchestrator
//
// Goal: answer whether this is a wrong-variable-space gate or a real
// capability gap.

#include "ExprParser.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/CoBExprBuilder.h"
#include "cobra/core/CoeffInterpolator.h"
#include "cobra/core/DecompositionEngine.h"
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

    // The 13 guard-failed line numbers (post evaluator-fix rebaseline)
    const std::vector< int > kTargetLines = { 58,  106, 125, 149, 184, 189, 264,
                                              306, 333, 392, 405, 482, 491 };

} // namespace

TEST(GuardFailedForensic, VariableSpaceAnalysis) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    std::vector< std::string > lines;
    std::string line;
    while (std::getline(file, line)) { lines.push_back(line); }

    int total            = 0;
    int guard_would_fire = 0;
    int poly_direct_ok   = 0;
    int poly_bypass_ok   = 0;

    std::map< uint32_t, int > by_named_vars;
    std::map< uint32_t, int > by_bool_real;
    std::map< uint32_t, int > by_fw_real;

    std::cerr << "\n";
    std::cerr << "═══════════════════════════════════════════════════════════\n";
    std::cerr << "  GUARD-FAILED FORENSIC: 14 NonPoly cases\n";
    std::cerr << "  Guard: kRealCount > 6 in ExtractPolyCore\n";
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

        const auto &sig  = parse.value().sig;
        const auto &vars = parse.value().vars;
        auto nv_named    = static_cast< uint32_t >(vars.size());

        ++total;
        by_named_vars[nv_named]++;

        // ── 1. Boolean aux-var elimination ───────────────────

        auto bool_elim = EliminateAuxVars(sig, vars);
        auto bool_real = static_cast< uint32_t >(bool_elim.real_vars.size());
        by_bool_real[bool_real]++;

        // ── 2. Full-width aux-var elimination ────────────────

        auto folded_ptr     = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*folded));
        Evaluator orig_eval = [folded_ptr,
                               &vars](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, 64);
        };

        auto fw_elim = EliminateAuxVars(sig, vars, orig_eval, 64);
        auto fw_real = static_cast< uint32_t >(fw_elim.real_vars.size());
        by_fw_real[fw_real]++;

        bool guard_fires = (fw_real > 6);
        if (guard_fires) { guard_would_fire++; }

        std::cerr << "\n────────────────────────────────────────────────────\n";
        std::cerr << "L" << target_line << " sem=" << semantic_str(cls.semantic)
                  << " named_vars=" << nv_named << " bool_real=" << bool_real
                  << " fw_real=" << fw_real << " guard=" << (guard_fires ? "FIRES" : "PASSES")
                  << " nodes=" << count_nodes(*folded) << "\n";
        std::cerr << "  flags={" << flag_str(cls.flags) << "}\n";
        std::cerr << "  GT: " << gt_str << "\n";

        // Show which variables are real at each level
        std::cerr << "  bool_real_vars: {";
        for (size_t i = 0; i < bool_elim.real_vars.size(); ++i) {
            if (i > 0) { std::cerr << ", "; }
            std::cerr << bool_elim.real_vars[i];
        }
        std::cerr << "}\n";

        std::cerr << "  fw_real_vars:   {";
        for (size_t i = 0; i < fw_elim.real_vars.size(); ++i) {
            if (i > 0) { std::cerr << ", "; }
            std::cerr << fw_elim.real_vars[i];
        }
        std::cerr << "}\n";

        // Show which variables were spurious at bool but became real at FW
        std::vector< std::string > promoted;
        for (const auto &rv : fw_elim.real_vars) {
            bool was_bool_real = false;
            for (const auto &br : bool_elim.real_vars) {
                if (br == rv) {
                    was_bool_real = true;
                    break;
                }
            }
            if (!was_bool_real) { promoted.push_back(rv); }
        }
        if (!promoted.empty()) {
            std::cerr << "  PROMOTED bool→fw: {";
            for (size_t i = 0; i < promoted.size(); ++i) {
                if (i > 0) { std::cerr << ", "; }
                std::cerr << promoted[i];
            }
            std::cerr << "}\n";
        }

        // ── 3. Direct polynomial recovery (bypassing guard) ─

        if (fw_real <= 6) {
            auto fw_support = BuildVarSupport(vars, fw_elim.real_vars);
            for (uint8_t deg = 2; deg <= 4; ++deg) {
                auto poly = RecoverAndVerifyPoly(orig_eval, fw_support, nv_named, 64, deg, deg);
                if (poly.Succeeded()) {
                    auto pr       = poly.TakePayload();
                    auto poly_str = Render(*pr.expr, fw_elim.real_vars, 64);
                    std::cerr << "  Poly(d=" << static_cast< int >(deg)
                              << ") VERIFIED: " << poly_str.substr(0, 100) << "\n";
                    poly_direct_ok++;
                    break;
                }
                std::cerr << "  Poly(d=" << static_cast< int >(deg) << ") failed\n";
            }
        } else {
            // Bypass guard: try polynomial recovery on FW real vars anyway
            auto fw_support = BuildVarSupport(vars, fw_elim.real_vars);
            std::cerr << "  Trying poly with " << fw_real << " vars (guard bypass)...\n";
            for (uint8_t deg = 2; deg <= 4; ++deg) {
                auto poly = RecoverAndVerifyPoly(orig_eval, fw_support, nv_named, 64, deg, deg);
                if (poly.Succeeded()) {
                    auto pr       = poly.TakePayload();
                    auto poly_str = Render(*pr.expr, fw_elim.real_vars, 64);
                    std::cerr << "  Poly(d=" << static_cast< int >(deg)
                              << ") VERIFIED (bypass): " << poly_str.substr(0, 100) << "\n";
                    poly_bypass_ok++;
                    break;
                }
                std::cerr << "  Poly(d=" << static_cast< int >(deg) << ") failed\n";
            }
        }

        // ── 4. CoB analysis for context ──────────────────────

        auto coeffs   = InterpolateCoefficients(bool_elim.reduced_sig, bool_real, 64);
        auto cob_expr = BuildCobExpr(coeffs, bool_real, 64);
        auto cob_str  = Render(*cob_expr, bool_elim.real_vars, 64);

        int nonlinear_terms = 0;
        for (size_t i = 1; i < coeffs.size(); ++i) {
            if (coeffs[i] == 0) { continue; }
            if (std::popcount(static_cast< unsigned >(i)) > 1) { nonlinear_terms++; }
        }
        std::cerr << "  CoB(" << bool_real << "v): " << cob_str.substr(0, 100) << " ["
                  << nonlinear_terms << " nonlinear]\n";

        // ── 5. Also run through Simplify to confirm it's unsupported ─

        auto folded_ptr2 = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*folded));
        Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
        opts.evaluator = [folded_ptr2](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr2, v, 64);
        };

        auto result = Simplify(sig, vars, folded.get(), opts);
        if (result.has_value() && result.value().kind == SimplifyOutcome::Kind::kSimplified) {
            auto out_str = Render(*result.value().expr, result.value().real_vars, 64);
            std::cerr << "  *** SIMPLIFIED: " << out_str.substr(0, 100) << " ***\n";
        }
    }

    // ── Summary ─────────────────────────────────────────────

    std::cerr << "\n═══════════════════════════════════════════════════════════\n";
    std::cerr << "  SUMMARY\n";
    std::cerr << "  Total: " << total << "\n";
    std::cerr << "\n  Named variables distribution:\n";
    for (const auto &[k, v] : by_named_vars) {
        std::cerr << "    " << k << " vars: " << v << "\n";
    }
    std::cerr << "\n  Boolean real vars distribution:\n";
    for (const auto &[k, v] : by_bool_real) {
        std::cerr << "    " << k << " real: " << v << "\n";
    }
    std::cerr << "\n  Full-width real vars distribution:\n";
    for (const auto &[k, v] : by_fw_real) {
        std::cerr << "    " << k << " real: " << v << "\n";
    }
    std::cerr << "\n  Guard would fire (fw_real>6): " << guard_would_fire << "/" << total
              << "\n";
    std::cerr << "  Poly direct (fw_real<=6): " << poly_direct_ok << "/" << total << "\n";
    std::cerr << "  Poly bypass (fw_real>6): " << poly_bypass_ok << "/" << total << "\n";
    std::cerr << "═══════════════════════════════════════════════════════════\n";
    std::cerr << "\n";

    EXPECT_EQ(total, 13);
}
