#include "ExprParser.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/SignatureSimplifier.h"
#include "cobra/core/Simplifier.h"
#include <algorithm>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <set>
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
            } else if (line[i] == ',' && depth == 0) {
                last_comma = i;
            }
        }
        return last_comma;
    }

    std::string trim(const std::string &s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) { return ""; }
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
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

    std::string route_str(Route r) {
        switch (r) {
            case Route::kBitwiseOnly:
                return "BitwiseOnly";
            case Route::kMultilinear:
                return "Multilinear";
            case Route::kPowerRecovery:
                return "PowerRecovery";
            case Route::kMixedRewrite:
                return "MixedRewrite";
            case Route::kUnsupported:
                return "Unsupported";
            default:
                return "Unknown";
        }
    }

    std::string semantic_str(SemanticClass s) {
        switch (s) {
            case SemanticClass::kLinear:
                return "Linear";
            case SemanticClass::kSemilinear:
                return "Semilinear";
            case SemanticClass::kPolynomial:
                return "Polynomial";
            case SemanticClass::kNonPolynomial:
                return "NonPoly";
            default:
                return "Unknown";
        }
    }

    // Check if the CoB result (boolean-domain correct) differs from
    // the original evaluator at full width
    bool has_arith_under_bitwise(const Expr &e) {
        // Add or Mul with a child that has non-leaf bitwise
        // (mirrors the classifier logic)
        bool child_has_bitwise = false;
        for (const auto &c : e.children) {
            if (has_arith_under_bitwise(*c)) { return true; }
            // Check if child is a non-leaf bitwise node
            if (c->kind == Expr::Kind::kAnd || c->kind == Expr::Kind::kOr
                || c->kind == Expr::Kind::kXor || c->kind == Expr::Kind::kNot)
            {
                // Only non-leaf if it has variable dependence
                // (simplified check: has children beyond constants)
                child_has_bitwise = true;
            }
        }
        if (child_has_bitwise && (e.kind == Expr::Kind::kAdd || e.kind == Expr::Kind::kMul)) {
            return true;
        }
        return false;
    }

    struct FailureEntry
    {
        int line_num;
        std::string dataset;
        uint32_t num_vars;
        Classification cls;
        std::string obfuscated_prefix;
        std::string ground_truth;
        std::string reason;
    };

    void scan_dataset(
        const std::string &path, const std::string &dataset_name,
        std::vector< FailureEntry > &failures, std::map< std::string, int > &by_route,
        std::map< std::string, int > &by_semantic, std::map< std::string, int > &by_flags,
        std::map< uint32_t, int > &by_vars, std::map< std::string, int > &by_reason,
        int &total_parsed, int &total_simplified, int &total_unsupported
    ) {
        std::ifstream file(path);
        ASSERT_TRUE(file.is_open()) << "Cannot open " << path;

        std::string line;
        int line_num = 0;
        while (std::getline(file, line)) {
            ++line_num;
            if (line.empty()) { continue; }

            size_t sep = find_separator(line);
            if (sep == std::string::npos) { continue; }

            std::string obfuscated = trim(line.substr(0, sep));
            if (obfuscated.empty()) { continue; }

            auto parse_result = ParseAndEvaluate(obfuscated, 64);
            if (!parse_result.has_value()) { continue; }

            auto ast_result = ParseToAst(obfuscated, 64);
            if (!ast_result.has_value()) { continue; }

            total_parsed++;

            auto folded_ptr = std::make_shared< std::unique_ptr< Expr > >(
                FoldConstantBitwise(std::move(ast_result.value().expr), 64)
            );

            Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
            opts.evaluator = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
                return EvalExpr(**folded_ptr, v, 64);
            };

            auto result = Simplify(
                parse_result.value().sig, parse_result.value().vars, folded_ptr->get(), opts
            );
            if (!result.has_value()) { continue; }

            if (result.value().kind == SimplifyOutcome::Kind::kSimplified) {
                total_simplified++;
                continue;
            }
            if (result.value().kind != SimplifyOutcome::Kind::kUnchangedUnsupported) {
                continue;
            }

            total_unsupported++;

            auto cls  = ClassifyStructural(**folded_ptr);
            auto elim = EliminateAuxVars(parse_result.value().sig, parse_result.value().vars);
            auto rv   = static_cast< uint32_t >(elim.real_vars.size());

            std::string reason;
            if (result.value().diag.reason.find("full-width verification") != std::string::npos)
            {
                reason = "FW verification failed";
            } else if (result.value().diag.reason.find("Rewrite") != std::string::npos) {
                reason = "Rewrite failed";
            } else {
                reason = result.value().diag.reason.substr(0, 60);
            }

            std::string gt    = trim(line.substr(sep + 1));
            std::string flags = flag_str(cls.flags);

            by_route[route_str(cls.route)]++;
            by_semantic[semantic_str(cls.semantic)]++;
            by_flags[flags]++;
            by_vars[rv]++;
            by_reason[reason]++;

            // Only collect FW-verification failures (ArithOverBitwise)
            if (result.value().diag.reason.find("full-width verification") != std::string::npos)
            {
                failures.push_back(
                    { .line_num          = line_num,
                      .dataset           = dataset_name,
                      .num_vars          = rv,
                      .cls               = cls,
                      .obfuscated_prefix = obfuscated.substr(0, 80),
                      .ground_truth      = gt.substr(0, 80),
                      .reason            = reason }
                );
            }
        }
    }

} // namespace

// Scan all datasets for ArithOverBitwise failures and categorize them
TEST(ArithOverBitwiseDiagnostic, FullScan) {
    struct DatasetInfo
    {
        std::string path;
        std::string name;
    };

    std::vector< DatasetInfo > datasets = {
        {   DATASET_DIR "/oses/oses_all.txt",   "OSES" },
        { DATASET_DIR "/gamba/qsynth_ea.txt", "QSynth" },
        {    DATASET_DIR "/gamba/syntia.txt", "Syntia" },
    };

    std::vector< FailureEntry > all_failures;
    std::map< std::string, int > by_route;
    std::map< std::string, int > by_semantic;
    std::map< std::string, int > by_flags;
    std::map< uint32_t, int > by_vars;
    std::map< std::string, int > by_reason;

    for (const auto &ds : datasets) {
        int parsed = 0, simplified = 0, unsupported = 0;
        std::cerr << "\n=== Scanning " << ds.name << " ===\n";
        scan_dataset(
            ds.path, ds.name, all_failures, by_route, by_semantic, by_flags, by_vars, by_reason,
            parsed, simplified, unsupported
        );
        std::cerr << "  parsed=" << parsed << " simplified=" << simplified
                  << " unsupported=" << unsupported << "\n";
    }

    // Separate FW-fail from other unsupported
    int fw_fail_count = 0;
    std::map< std::string, int > fw_by_dataset;
    std::map< std::string, int > fw_by_route;
    std::map< std::string, int > fw_by_semantic;
    std::map< std::string, int > fw_by_flags;
    std::map< uint32_t, int > fw_by_vars;

    for (const auto &f : all_failures) {
        fw_fail_count++;
        fw_by_dataset[f.dataset]++;
        fw_by_route[route_str(f.cls.route)]++;
        fw_by_semantic[semantic_str(f.cls.semantic)]++;
        fw_by_flags[flag_str(f.cls.flags)]++;
        fw_by_vars[f.num_vars]++;
    }

    std::cerr << "\n========================================\n";
    std::cerr << "ArithOverBitwise FW-Verification Failures\n";
    std::cerr << "========================================\n";
    std::cerr << "Total: " << fw_fail_count << "\n";

    std::cerr << "\n--- By dataset ---\n";
    for (auto &[k, v] : fw_by_dataset) { std::cerr << "  " << k << ": " << v << "\n"; }

    std::cerr << "\n--- By route ---\n";
    for (auto &[k, v] : fw_by_route) { std::cerr << "  " << k << ": " << v << "\n"; }

    std::cerr << "\n--- By semantic class ---\n";
    for (auto &[k, v] : fw_by_semantic) { std::cerr << "  " << k << ": " << v << "\n"; }

    std::cerr << "\n--- By structural flags ---\n";
    for (auto &[k, v] : fw_by_flags) { std::cerr << "  {" << k << "}: " << v << "\n"; }

    std::cerr << "\n--- By variable count ---\n";
    for (auto &[k, v] : fw_by_vars) { std::cerr << "  " << k << " vars: " << v << "\n"; }

    std::cerr << "\n--- All unsupported by reason ---\n";
    for (auto &[k, v] : by_reason) { std::cerr << "  " << k << ": " << v << "\n"; }

    // Detailed listing of FW failures by dataset
    for (const auto &ds_name : std::vector< std::string >{ "OSES", "QSynth", "Syntia" }) {
        std::cerr << "\n--- " << ds_name << " FW failures ---\n";
        for (const auto &f : all_failures) {
            if (f.dataset != ds_name) { continue; }
            std::cerr << "  L" << f.line_num << " vars=" << f.num_vars
                      << " route=" << route_str(f.cls.route) << " flags={"
                      << flag_str(f.cls.flags) << "}"
                      << " GT: " << f.ground_truth << "\n";
        }
    }

    std::cerr << "\n";
}

// Detailed analysis: for each FW failure, show what CoB produces
// vs what the evaluator produces at a failing input
TEST(ArithOverBitwiseDiagnostic, CoBMismatchDetail) {
    std::ifstream file(DATASET_DIR "/gamba/syntia.txt");
    ASSERT_TRUE(file.is_open());

    std::string line;
    int line_num = 0;
    int shown    = 0;

    std::cerr << "\n=== Syntia CoB Mismatch Details ===\n";

    while (std::getline(file, line) && shown < 10) {
        ++line_num;
        if (line.empty()) { continue; }

        size_t sep = find_separator(line);
        if (sep == std::string::npos) { continue; }

        std::string obfuscated = trim(line.substr(0, sep));
        if (obfuscated.empty()) { continue; }

        auto parse_result = ParseAndEvaluate(obfuscated, 64);
        if (!parse_result.has_value()) { continue; }

        auto ast_result = ParseToAst(obfuscated, 64);
        if (!ast_result.has_value()) { continue; }

        auto folded_ptr = std::make_shared< std::unique_ptr< Expr > >(
            FoldConstantBitwise(std::move(ast_result.value().expr), 64)
        );

        const auto &vars = parse_result.value().vars;

        // Run without evaluator to get the CoB result
        Options opts_no_eval{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
        auto result_unverified =
            Simplify(parse_result.value().sig, vars, folded_ptr->get(), opts_no_eval);

        // Run with evaluator to see if it's caught
        Options opts_eval{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
        opts_eval.evaluator = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, 64);
        };
        auto result_verified =
            Simplify(parse_result.value().sig, vars, folded_ptr->get(), opts_eval);

        if (!result_unverified.has_value() || !result_verified.has_value()) { continue; }

        // We want cases where unverified says simplified but verified
        // says unsupported
        bool unverified_ok =
            result_unverified.value().kind == SimplifyOutcome::Kind::kSimplified;
        bool verified_ok = result_verified.value().kind == SimplifyOutcome::Kind::kSimplified;

        if (!unverified_ok || verified_ok) { continue; }

        // This is an ArithOverBitwise false simplification caught by FW check
        auto cls       = ClassifyStructural(**folded_ptr);
        std::string gt = trim(line.substr(sep + 1));

        std::cerr << "\nL" << line_num << " route=" << route_str(cls.route) << " flags=0x"
                  << std::hex << cls.flags << std::dec << " vars=" << vars.size() << "\n";
        std::cerr << "  GT:  " << gt << "\n";
        std::cerr
            << "  CoB: "
            << Render(*result_unverified.value().expr, result_unverified.value().real_vars, 64)
            << "\n";

        // Spot-check at a concrete input
        std::vector< uint64_t > test_input(vars.size(), 0x5555555555555555ULL);
        uint64_t orig_val = EvalExpr(**folded_ptr, test_input, 64);
        uint64_t cob_val  = EvalExpr(*result_unverified.value().expr, test_input, 64);

        std::cerr << "  At 0x5555...: orig=0x" << std::hex << orig_val << " cob=0x" << cob_val
                  << std::dec << "\n";

        shown++;
    }

    std::cerr << "\n";
}
