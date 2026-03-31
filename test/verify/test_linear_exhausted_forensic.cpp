// Forensic audit of the 8 Linear search-exhausted QSynth expressions.
//
// These are classified as Linear at {0,1} but have ArithOverBitwise
// structure. CoB should find a correct Boolean representation, but
// full-width behavior may diverge if the expression isn't genuinely
// linear at full width.
//
// For each expression:
//   1. Classification + variable counts
//   2. CoB reconstruction — boolean-correct? full-width-correct?
//   3. Full-width polynomial degree probe (is it polynomial at FW?)
//   4. Standalone Simplify with full telemetry + cause chain
//   5. Ground-truth analysis
//
// Goal: determine whether these are routing/pipeline issues or
// genuine capability gaps for Linear expressions.

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

    const char *category_str(ReasonCategory cat) {
        switch (cat) {
            case ReasonCategory::kSearchExhausted:
                return "search-exhausted";
            case ReasonCategory::kVerifyFailed:
                return "verify-failed";
            case ReasonCategory::kRepresentationGap:
                return "representation-gap";
            case ReasonCategory::kNoSolution:
                return "no-solution";
            case ReasonCategory::kResourceLimit:
                return "resource-limit";
            case ReasonCategory::kGuardFailed:
                return "guard-failed";
            case ReasonCategory::kInapplicable:
                return "inapplicable";
            case ReasonCategory::kCostRejected:
                return "cost-rejected";
            case ReasonCategory::kInternalInvariant:
                return "internal";
            case ReasonCategory::kNone:
                return "none";
        }
        return "unknown";
    }

    const char *domain_str(ReasonDomain d) {
        switch (d) {
            case ReasonDomain::kOrchestrator:
                return "Orchestrator";
            case ReasonDomain::kSemilinear:
                return "Semilinear";
            case ReasonDomain::kSignature:
                return "Signature";
            case ReasonDomain::kStructuralTransform:
                return "StructXform";
            case ReasonDomain::kDecomposition:
                return "Decomposition";
            case ReasonDomain::kTemplateDecomposer:
                return "TemplateDec";
            case ReasonDomain::kWeightedPolyFit:
                return "WeightedPoly";
            case ReasonDomain::kMultivarPoly:
                return "MultivarPoly";
            case ReasonDomain::kPolynomialRecovery:
                return "PolyRecovery";
            case ReasonDomain::kBitwiseDecomposer:
                return "BitwiseDec";
            case ReasonDomain::kHybridDecomposer:
                return "HybridDec";
            case ReasonDomain::kGhostResidual:
                return "GhostResidual";
            case ReasonDomain::kOperandSimplifier:
                return "OperandSimp";
            case ReasonDomain::kLifting:
                return "Lifting";
            case ReasonDomain::kVerifier:
                return "Verifier";
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

    // The 8 Linear search-exhausted line numbers
    const std::vector< int > kTargetLines = { 56, 85, 96, 115, 131, 199, 301, 344 };

} // namespace

TEST(LinearExhaustedForensic, DeepAnalysis) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    std::vector< std::string > lines;
    std::string line;
    while (std::getline(file, line)) { lines.push_back(line); }

    int total       = 0;
    int cob_bool_ok = 0;
    int cob_fw_ok   = 0;
    int poly_ok     = 0;

    std::cerr << "\n";
    std::cerr << "═══════════════════════════════════════════════════════════\n";
    std::cerr << "  LINEAR SEARCH-EXHAUSTED FORENSIC: 8 cases\n";
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
        auto nv          = static_cast< uint32_t >(vars.size());

        auto bool_elim = EliminateAuxVars(sig, vars);
        auto bool_real = static_cast< uint32_t >(bool_elim.real_vars.size());

        auto folded_ptr     = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*folded));
        Evaluator orig_eval = [folded_ptr,
                               &vars](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, 64);
        };

        auto fw_elim = EliminateAuxVars(sig, vars, orig_eval, 64);
        auto fw_real = static_cast< uint32_t >(fw_elim.real_vars.size());

        ++total;

        std::cerr << "\n────────────────────────────────────────────────────\n";
        std::cerr << "L" << target_line << " sem=Linear"
                  << " named=" << nv << " bool_real=" << bool_real << " fw_real=" << fw_real
                  << " nodes=" << count_nodes(*folded) << " flags={" << flag_str(cls.flags)
                  << "}\n";
        std::cerr << "  GT: " << gt_str << "\n";

        // ── 1. GT classification ─────────────────────────────

        auto gt_ast = ParseToAst(gt_str, 64);
        if (gt_ast.has_value()) {
            auto gt_folded = FoldConstantBitwise(std::move(gt_ast.value().expr), 64);
            auto gt_cls    = ClassifyStructural(*gt_folded);
            std::cerr << "  GT class: sem="
                      << (gt_cls.semantic == SemanticClass::kLinear           ? "Linear"
                              : gt_cls.semantic == SemanticClass::kSemilinear ? "Semilinear"
                              : gt_cls.semantic == SemanticClass::kPolynomial ? "Polynomial"
                                                                              : "NonPoly")
                      << " flags={" << flag_str(gt_cls.flags) << "}"
                      << " nodes=" << count_nodes(*gt_folded) << "\n";
        }

        // ── 2. CoB reconstruction ────────────────────────────

        auto coeffs   = InterpolateCoefficients(bool_elim.reduced_sig, bool_real, 64);
        auto cob_expr = BuildCobExpr(coeffs, bool_real, 64);
        auto cob_str  = Render(*cob_expr, bool_elim.real_vars, 64);

        // Count nonlinear terms
        int nonlinear_terms = 0;
        for (size_t i = 1; i < coeffs.size(); ++i) {
            if (coeffs[i] != 0 && std::popcount(static_cast< unsigned >(i)) > 1) {
                nonlinear_terms++;
            }
        }

        // Boolean check
        auto bool_check = SignatureCheck(bool_elim.reduced_sig, *cob_expr, bool_real, 64);
        if (bool_check.passed) { cob_bool_ok++; }

        // Full-width check (in reduced variable space)
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

        auto fw_check = FullWidthCheckEval(reduced_eval, bool_real, *cob_expr, 64);
        if (fw_check.passed) { cob_fw_ok++; }

        std::cerr << "  CoB: " << cob_str.substr(0, 100) << " [" << nonlinear_terms
                  << " nonlinear]\n";
        std::cerr << "  CoB bool=" << (bool_check.passed ? "OK" : "FAIL")
                  << " fw=" << (fw_check.passed ? "OK <<<" : "FAIL") << "\n";

        if (!fw_check.passed && !fw_check.failing_input.empty()) {
            uint64_t orig_val = reduced_eval(fw_check.failing_input);
            uint64_t cob_val  = EvalExpr(*cob_expr, fw_check.failing_input, 64);
            std::cerr << "    diverges at [";
            for (size_t i = 0; i < fw_check.failing_input.size(); ++i) {
                if (i > 0) { std::cerr << ", "; }
                std::cerr << bool_elim.real_vars[i] << "=0x" << std::hex
                          << fw_check.failing_input[i] << std::dec;
            }
            std::cerr << "]\n";
            std::cerr << "    orig=" << orig_val << " cob=" << cob_val << "\n";
        }

        // ── 3. FW polynomial probe ──────────────────────────

        auto fw_support = BuildVarSupport(vars, fw_elim.real_vars);
        bool any_poly   = false;
        for (uint8_t deg = 1; deg <= 4; ++deg) {
            auto poly = RecoverAndVerifyPoly(orig_eval, fw_support, nv, 64, deg, deg);
            if (poly.Succeeded()) {
                auto pr       = poly.TakePayload();
                auto poly_str = Render(*pr.expr, fw_elim.real_vars, 64);
                std::cerr << "  Poly(d=" << static_cast< int >(deg)
                          << ") VERIFIED: " << poly_str.substr(0, 100) << "\n";
                poly_ok++;
                any_poly = true;
                break;
            }
        }
        if (!any_poly) { std::cerr << "  Poly: NOT polynomial (d=1..4 all fail)\n"; }

        // ── 4. Simplify with full telemetry ──────────────────

        auto folded_ptr2 = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*folded));
        Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
        opts.evaluator = [folded_ptr2](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr2, v, 64);
        };

        auto result = Simplify(sig, vars, folded.get(), opts);
        if (!result.has_value()) {
            std::cerr << "  Simplify: ERROR\n";
            continue;
        }

        const auto &out = result.value();
        if (out.kind == SimplifyOutcome::Kind::kSimplified) {
            auto out_str = Render(*out.expr, out.real_vars, 64);
            std::cerr << "  Simplify: SOLVED → " << out_str.substr(0, 100) << "\n";
            continue;
        }

        std::cerr << "  Simplify: UNSUPPORTED"
                  << " exp=" << out.telemetry.total_expansions
                  << " cands=" << out.telemetry.candidates_verified
                  << " qhw=" << out.telemetry.queue_high_water << "\n";

        if (out.diag.reason_code.has_value()) {
            std::cerr << "    reason: " << category_str(out.diag.reason_code->category) << "/"
                      << domain_str(out.diag.reason_code->domain)
                      << " sub=" << out.diag.reason_code->subcode << "\n";
        }
        std::cerr << "    message: " << out.diag.reason.substr(0, 100) << "\n";

        // Print cause chain
        for (size_t i = 0; i < out.diag.cause_chain.size() && i < 8; ++i) {
            const auto &frame = out.diag.cause_chain[i];
            std::cerr << "    cause[" << i << "]: " << domain_str(frame.code.domain) << "/"
                      << category_str(frame.code.category) << " sub=" << frame.code.subcode
                      << " — " << frame.message.substr(0, 80) << "\n";
        }

        std::cerr << "    xform_rounds=" << out.diag.structural_transform_rounds
                  << " xform_produced=" << out.diag.transform_produced_candidate
                  << " cand_failed_verify=" << out.diag.candidate_failed_verification << "\n";
    }

    // ── Summary ─────────────────────────────────────────────

    std::cerr << "\n═══════════════════════════════════════════════════════════\n";
    std::cerr << "  SUMMARY\n";
    std::cerr << "  Total: " << total << "\n";
    std::cerr << "  CoB boolean-correct: " << cob_bool_ok << "/" << total << "\n";
    std::cerr << "  CoB full-width-correct: " << cob_fw_ok << "/" << total << "\n";
    std::cerr << "  Polynomial at FW (d=1..4): " << poly_ok << "/" << total << "\n";
    std::cerr << "═══════════════════════════════════════════════════════════\n";
    std::cerr << "\n";

    EXPECT_EQ(total, 8);
}
