#include "ExprParser.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/Simplifier.h"
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
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) { return ""; }
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
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

    std::string category_str(ReasonCategory cat) {
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

    uint32_t count_nodes(const Expr &e) {
        uint32_t n = 1;
        for (const auto &c : e.children) { n += count_nodes(*c); }
        return n;
    }

    struct ExprResult
    {
        int line_num;
        std::string ground_truth;
        Classification cls;
        uint32_t real_vars;
        uint32_t tree_size;

        // Outcome
        bool simplified;
        std::string reason_category; // empty if simplified
        std::string reason_message;

        // Telemetry
        uint32_t expansions;
        uint32_t candidates_verified;

        // If simplified: the result
        std::string simplified_expr;
        uint32_t simplified_cost;

        // Cause chain info
        std::string deepest_cause;
    };

    // Known baseline: line numbers that were unsupported before the
    // top-K lifting change, categorized by reason.
    // From test_masked_arith_audit and test_search_exhausted_audit runs.
    //
    // Rather than hardcoding, we compare against the known aggregate
    // counts: 388 simplified, 77 search-exhausted, 14 verify-failed,
    // 7 representation-gap, 14 resource-limit/other.

} // namespace

TEST(ForensicAudit, QSynthCategoryShifts) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    std::vector< ExprResult > results;
    std::string line;
    int line_num = 0;

    while (std::getline(file, line)) {
        ++line_num;
        if (line.empty() || line[0] == '#') { continue; }

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

        Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
        opts.evaluator = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, 64);
        };

        auto result = Simplify(
            parse_result.value().sig, parse_result.value().vars, folded_ptr->get(), opts
        );
        if (!result.has_value()) { continue; }

        auto cls       = ClassifyStructural(**folded_ptr);
        auto elim      = EliminateAuxVars(parse_result.value().sig, parse_result.value().vars);
        auto rv        = static_cast< uint32_t >(elim.real_vars.size());
        auto tree_size = count_nodes(**folded_ptr);

        std::string gt_str = trim(line.substr(sep + 1));

        ExprResult rec;
        rec.line_num            = line_num;
        rec.ground_truth        = gt_str.substr(0, 80);
        rec.cls                 = cls;
        rec.real_vars           = rv;
        rec.tree_size           = tree_size;
        rec.expansions          = result.value().telemetry.total_expansions;
        rec.candidates_verified = result.value().telemetry.candidates_verified;

        if (result.value().kind == SimplifyOutcome::Kind::kSimplified) {
            rec.simplified = true;
            if (result.value().expr) {
                rec.simplified_expr =
                    Render(*result.value().expr, result.value().real_vars, 64).substr(0, 80);
                rec.simplified_cost = ComputeCost(*result.value().expr).cost.weighted_size;
            }
        } else {
            rec.simplified = false;
            if (result.value().diag.reason_code.has_value()) {
                rec.reason_category = category_str(result.value().diag.reason_code->category);
            } else {
                rec.reason_category = "unknown";
            }
            rec.reason_message = result.value().diag.reason.substr(0, 100);

            // Deepest cause in chain
            if (!result.value().diag.cause_chain.empty()) {
                rec.deepest_cause =
                    result.value().diag.cause_chain.back().message.substr(0, 100);
            }
        }

        results.push_back(std::move(rec));
    }

    // ═══════════════════════════════════════════════════════════
    // Aggregate current counts
    // ═══════════════════════════════════════════════════════════

    int total_simplified  = 0;
    int total_unsupported = 0;
    std::map< std::string, int > by_category;

    for (const auto &r : results) {
        if (r.simplified) {
            total_simplified++;
        } else {
            total_unsupported++;
            by_category[r.reason_category]++;
        }
    }

    std::cerr << "\n=== QSynth Forensic Audit ===\n";
    std::cerr << "Total: " << results.size() << " parsed\n";
    std::cerr << "Simplified: " << total_simplified << "\n";
    std::cerr << "Unsupported: " << total_unsupported << "\n";
    std::cerr << "\nBy category:\n";
    for (const auto &[cat, count] : by_category) {
        std::cerr << "  " << cat << ": " << count << "\n";
    }

    // ═══════════════════════════════════════════════════════════
    // Identify unsupported by line number for comparison
    // ═══════════════════════════════════════════════════════════

    // List all currently unsupported with full details
    std::cerr << "\n════════════════════════════════════════════\n";
    std::cerr << "All Unsupported Expressions (current)\n";
    std::cerr << "════════════════════════════════════════════\n";

    for (const auto &r : results) {
        if (r.simplified) { continue; }
        std::cerr << "  L" << r.line_num << " vars=" << r.real_vars << " nodes=" << r.tree_size
                  << " semantic=" << semantic_str(r.cls.semantic)
                  << " cat=" << r.reason_category << " exp=" << r.expansions
                  << " verified=" << r.candidates_verified << "\n";
        std::cerr << "    GT: " << r.ground_truth << "\n";
        if (!r.reason_message.empty()) {
            std::cerr << "    Reason: " << r.reason_message << "\n";
        }
        if (!r.deepest_cause.empty()) { std::cerr << "    Cause: " << r.deepest_cause << "\n"; }
    }

    // ═══════════════════════════════════════════════════════════
    // Identify verify-failed with structural detail
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n════════════════════════════════════════════\n";
    std::cerr << "Verify-Failed Detail\n";
    std::cerr << "════════════════════════════════════════════\n";

    for (const auto &r : results) {
        if (r.simplified || r.reason_category != "verify-failed") { continue; }
        std::cerr << "\n  L" << r.line_num << " vars=" << r.real_vars
                  << " nodes=" << r.tree_size << " semantic=" << semantic_str(r.cls.semantic)
                  << " flags={" << flag_str(r.cls.flags) << "}"
                  << " exp=" << r.expansions << " verified=" << r.candidates_verified << "\n";
        std::cerr << "    GT: " << r.ground_truth << "\n";
        if (!r.deepest_cause.empty()) {
            std::cerr << "    Deepest cause: " << r.deepest_cause << "\n";
        }
    }

    // ═══════════════════════════════════════════════════════════
    // Show all simplified expressions by line number
    // (for diffing against baseline to find newly solved)
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n════════════════════════════════════════════\n";
    std::cerr << "Simplified Line Numbers (for baseline diff)\n";
    std::cerr << "════════════════════════════════════════════\n";

    std::vector< int > simplified_lines;
    for (const auto &r : results) {
        if (r.simplified) { simplified_lines.push_back(r.line_num); }
    }
    std::cerr << "Count: " << simplified_lines.size() << "\n";
    // Print as compact list for easy diffing
    for (size_t i = 0; i < simplified_lines.size(); ++i) {
        if (i > 0) { std::cerr << ","; }
        std::cerr << simplified_lines[i];
    }
    std::cerr << "\n";

    // Also print unsupported line numbers grouped by category
    std::cerr << "\n════════════════════════════════════════════\n";
    std::cerr << "Unsupported Line Numbers by Category\n";
    std::cerr << "════════════════════════════════════════════\n";

    std::map< std::string, std::vector< int > > unsup_by_cat;
    for (const auto &r : results) {
        if (!r.simplified) { unsup_by_cat[r.reason_category].push_back(r.line_num); }
    }
    for (const auto &[cat, lines] : unsup_by_cat) {
        std::cerr << cat << " (" << lines.size() << "): ";
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i > 0) { std::cerr << ","; }
            std::cerr << lines[i];
        }
        std::cerr << "\n";
    }

    std::cerr << "\n";
}
