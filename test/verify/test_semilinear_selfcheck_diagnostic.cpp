#include "ExprParser.h"
#include "cobra/core/AtomSimplifier.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/MaskedAtomReconstructor.h"
#include "cobra/core/SelfCheck.h"
#include "cobra/core/SemilinearIR.h"
#include "cobra/core/SemilinearNormalizer.h"
#include "cobra/core/SignatureChecker.h"
#include <algorithm>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <sstream>
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

} // namespace

// Categorize all MSiMBA self-check failures
TEST(SemilinearSelfcheckDiagnostic, AnalyzeFailures) {
    std::ifstream file(DATASET_DIR "/msimba.txt");
    ASSERT_TRUE(file.is_open());

    std::string line;
    int line_num     = 0;
    int total_parsed = 0;

    int normalize_ok   = 0;
    int normalize_fail = 0;
    int selfcheck_pass = 0;
    int selfcheck_fail = 0;
    int probe_pass     = 0;
    int probe_fail     = 0;

    // Self-check failure sub-categories
    std::map< std::string, int > by_sc_reason;
    // Term/atom statistics for passing vs failing
    std::vector< uint32_t > pass_term_counts;
    std::vector< uint32_t > fail_term_counts;
    std::vector< uint32_t > pass_atom_counts;
    std::vector< uint32_t > fail_atom_counts;

    // Detailed failure entries (first N)
    struct FailEntry
    {
        int line;
        std::string sc_detail;
        uint32_t num_terms;
        uint32_t num_atoms;
        uint64_t ir_constant;
        std::string gt;
    };

    std::vector< FailEntry > first_failures;

    while (std::getline(file, line)) {
        ++line_num;
        if (line.empty()) { continue; }

        size_t sep = find_separator(line);
        if (sep == std::string::npos) { continue; }

        std::string expr_str = trim(line.substr(0, sep));
        if (expr_str.empty()) { continue; }

        auto ast_result = ParseToAst(expr_str, 64);
        if (!ast_result.has_value()) { continue; }
        total_parsed++;

        auto folded    = FoldConstantBitwise(std::move(ast_result.value().expr), 64);
        auto vars      = ast_result.value().vars;
        std::string gt = trim(line.substr(sep + 1));

        // Normalize
        auto ir_result = NormalizeToSemilinear(*folded, vars, 64);
        if (!ir_result.has_value()) {
            normalize_fail++;
            continue;
        }
        normalize_ok++;

        auto &ir = ir_result.value();

        // Simplify structure
        SimplifyStructure(ir);

        auto num_terms = static_cast< uint32_t >(ir.terms.size());
        auto num_atoms = static_cast< uint32_t >(ir.atom_table.size());

        // Self-check
        auto plain = ReconstructMaskedAtoms(ir, {});
        auto sc    = SelfCheckSemilinear(ir, *plain, vars, 64);

        if (sc.passed) {
            selfcheck_pass++;
            pass_term_counts.push_back(num_terms);
            pass_atom_counts.push_back(num_atoms);

            // Also check if probe passes
            auto evaluator = [&folded](const std::vector< uint64_t > &v) -> uint64_t {
                return EvalExpr(*folded, v, 64);
            };
            auto probe = FullWidthCheckEval(
                evaluator, static_cast< uint32_t >(vars.size()), *plain, 64, 16
            );
            if (probe.passed) {
                probe_pass++;
            } else {
                probe_fail++;
            }
        } else {
            selfcheck_fail++;
            fail_term_counts.push_back(num_terms);
            fail_atom_counts.push_back(num_atoms);

            // Categorize the failure reason
            std::string reason;
            if (sc.mismatch_detail.find("constant mismatch") != std::string::npos) {
                reason = "constant_mismatch";
            } else if (sc.mismatch_detail.find("term count") != std::string::npos) {
                reason = "term_count_mismatch";
            } else if (sc.mismatch_detail.find("coefficient mismatch") != std::string::npos) {
                reason = "coefficient_mismatch";
            } else if (sc.mismatch_detail.find("missing") != std::string::npos) {
                reason = "atom_missing";
            } else if (sc.mismatch_detail.find("re-normalization") != std::string::npos) {
                reason = "re_normalization_failed";
            } else {
                reason = sc.mismatch_detail.substr(0, 40);
            }
            by_sc_reason[reason]++;

            if (first_failures.size() < 20) {
                first_failures.push_back(
                    { .line        = line_num,
                      .sc_detail   = sc.mismatch_detail,
                      .num_terms   = num_terms,
                      .num_atoms   = num_atoms,
                      .ir_constant = ir.constant,
                      .gt          = gt.substr(0, 60) }
                );
            }
        }
    }

    // Compute median/mean for term counts
    auto median = [](std::vector< uint32_t > v) -> double {
        if (v.empty()) { return 0.0; }
        std::sort(v.begin(), v.end());
        auto n = v.size();
        return (n % 2 == 0) ? (v[n / 2 - 1] + v[n / 2]) / 2.0 : static_cast< double >(v[n / 2]);
    };

    std::cerr << "\n========================================\n";
    std::cerr << "Semilinear Self-Check Diagnostic (MSiMBA)\n";
    std::cerr << "========================================\n";
    std::cerr << "Total parsed:       " << total_parsed << "\n";
    std::cerr << "Normalize OK:       " << normalize_ok << "\n";
    std::cerr << "Normalize fail:     " << normalize_fail << "\n";
    std::cerr << "Self-check pass:    " << selfcheck_pass << "\n";
    std::cerr << "Self-check fail:    " << selfcheck_fail << "\n";
    std::cerr << "Probe pass (of SC pass): " << probe_pass << "\n";
    std::cerr << "Probe fail (of SC pass): " << probe_fail << "\n";

    std::cerr << "\n--- Self-check failure reasons ---\n";
    for (auto &[k, v] : by_sc_reason) { std::cerr << "  " << k << ": " << v << "\n"; }

    std::cerr << "\n--- Term count statistics ---\n";
    std::cerr << "  Passing: n=" << pass_term_counts.size()
              << " median=" << median(pass_term_counts) << "\n";
    std::cerr << "  Failing: n=" << fail_term_counts.size()
              << " median=" << median(fail_term_counts) << "\n";

    std::cerr << "\n--- Atom count statistics ---\n";
    std::cerr << "  Passing: n=" << pass_atom_counts.size()
              << " median=" << median(pass_atom_counts) << "\n";
    std::cerr << "  Failing: n=" << fail_atom_counts.size()
              << " median=" << median(fail_atom_counts) << "\n";

    std::cerr << "\n--- First " << first_failures.size() << " failures (detail) ---\n";
    for (const auto &f : first_failures) {
        std::cerr << "  L" << f.line << " terms=" << f.num_terms << " atoms=" << f.num_atoms
                  << " const=0x" << std::hex << f.ir_constant << std::dec
                  << "\n    SC: " << f.sc_detail << "\n    GT: " << f.gt << "\n";
    }

    std::cerr << "\n";
}

// Deep-dive: for self-check failures, compare original IR atom keys
// vs reconstructed IR atom keys to understand the mismatch
TEST(SemilinearSelfcheckDiagnostic, AtomKeyMismatchAnalysis) {
    std::ifstream file(DATASET_DIR "/msimba.txt");
    ASSERT_TRUE(file.is_open());

    std::string line;
    int line_num = 0;
    int analyzed = 0;

    // Track how many atoms appear/disappear during reconstruct→re-normalize
    int total_orig_only  = 0;
    int total_re_only    = 0;
    int total_coeff_diff = 0;

    std::cerr << "\n=== Atom Key Mismatch Deep-Dive ===\n";

    while (std::getline(file, line) && analyzed < 50) {
        ++line_num;
        if (line.empty()) { continue; }

        size_t sep = find_separator(line);
        if (sep == std::string::npos) { continue; }

        std::string expr_str = trim(line.substr(0, sep));
        if (expr_str.empty()) { continue; }

        auto ast_result = ParseToAst(expr_str, 64);
        if (!ast_result.has_value()) { continue; }

        auto folded = FoldConstantBitwise(std::move(ast_result.value().expr), 64);
        auto vars   = ast_result.value().vars;

        auto ir_result = NormalizeToSemilinear(*folded, vars, 64);
        if (!ir_result.has_value()) { continue; }

        auto &ir = ir_result.value();
        SimplifyStructure(ir);

        auto plain = ReconstructMaskedAtoms(ir, {});
        auto sc    = SelfCheckSemilinear(ir, *plain, vars, 64);

        if (sc.passed) { continue; }

        analyzed++;

        // Re-normalize the reconstruction
        auto re_result = NormalizeToSemilinear(*plain, vars, 64);
        if (!re_result.has_value()) {
            std::cerr << "L" << line_num << " re-normalization failed entirely\n";
            continue;
        }
        auto &re = re_result.value();

        // Build coefficient maps for comparison
        const uint64_t kMask = UINT64_MAX;
        std::map< std::string, uint64_t > orig_map;
        std::map< std::string, uint64_t > re_map;

        auto make_key_str = [](const AtomInfo &atom) {
            std::ostringstream oss;
            oss << "sup=[";
            for (size_t i = 0; i < atom.key.support.size(); ++i) {
                if (i > 0) { oss << ","; }
                oss << atom.key.support[i];
            }
            oss << "] tt=[";
            for (size_t i = 0; i < atom.key.truth_table.size(); ++i) {
                if (i > 0) { oss << ","; }
                oss << "0x" << std::hex << atom.key.truth_table[i];
            }
            oss << "]";
            return oss.str();
        };

        for (const auto &term : ir.terms) {
            auto k      = make_key_str(ir.atom_table[term.atom_id]);
            orig_map[k] = (orig_map[k] + term.coeff) & kMask;
        }
        for (const auto &term : re.terms) {
            auto k    = make_key_str(re.atom_table[term.atom_id]);
            re_map[k] = (re_map[k] + term.coeff) & kMask;
        }

        // Remove zeros
        for (auto it = orig_map.begin(); it != orig_map.end();) {
            if (it->second == 0) {
                it = orig_map.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = re_map.begin(); it != re_map.end();) {
            if (it->second == 0) {
                it = re_map.erase(it);
            } else {
                ++it;
            }
        }

        int orig_only  = 0;
        int re_only    = 0;
        int coeff_diff = 0;

        // Find atoms in original but not in re
        for (const auto &[k, v] : orig_map) {
            auto it = re_map.find(k);
            if (it == re_map.end()) {
                orig_only++;
            } else if (it->second != v) {
                coeff_diff++;
            }
        }
        // Find atoms in re but not in original
        for (const auto &[k, v] : re_map) {
            if (orig_map.find(k) == orig_map.end()) { re_only++; }
        }

        total_orig_only  += orig_only;
        total_re_only    += re_only;
        total_coeff_diff += coeff_diff;

        if (analyzed <= 5) {
            std::cerr << "\nL" << line_num << " orig_terms=" << orig_map.size()
                      << " re_terms=" << re_map.size() << " const_orig=0x" << std::hex
                      << (ir.constant & kMask) << " const_re=0x" << (re.constant & kMask)
                      << std::dec << "\n  orig_only=" << orig_only << " re_only=" << re_only
                      << " coeff_diff=" << coeff_diff << "\n  SC: " << sc.mismatch_detail
                      << "\n";

            // Show first few differing atoms
            int shown = 0;
            for (const auto &[k, v] : orig_map) {
                auto it = re_map.find(k);
                if (it == re_map.end()) {
                    std::cerr << "    ORIG-ONLY: " << k << " coeff=0x" << std::hex << v
                              << std::dec << "\n";
                    if (++shown >= 3) { break; }
                } else if (it->second != v) {
                    std::cerr << "    DIFF: " << k << " orig=0x" << std::hex << v << " re=0x"
                              << it->second << std::dec << "\n";
                    if (++shown >= 3) { break; }
                }
            }
        }
    }

    std::cerr << "\n--- Aggregate (first " << analyzed << " failures) ---\n";
    std::cerr << "  Total orig-only atoms: " << total_orig_only << "\n";
    std::cerr << "  Total re-only atoms:   " << total_re_only << "\n";
    std::cerr << "  Total coeff diffs:     " << total_coeff_diff << "\n";
    std::cerr << "\n";
}
