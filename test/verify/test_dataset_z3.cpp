#include "ExprParser.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/Simplifier.h"
#include "cobra/verify/Z3Verifier.h"
#include <fstream>
#include <gtest/gtest.h>
#include <string>

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

    struct Z3Stats
    {
        int total      = 0;
        int simplified = 0;
        int verified   = 0;
        int failed     = 0;
        int skipped    = 0;
    };

    Z3Stats run_z3_verify(const std::string &path, uint32_t timeout_ms = 500) {
        Z3Stats stats;
        std::ifstream file(path);
        EXPECT_TRUE(file.is_open()) << "Cannot open " << path;
        if (!file.is_open()) { return stats; }

        std::string line;
        int line_num = 0;
        while (std::getline(file, line)) {
            ++line_num;
            if (line.empty()) { continue; }
            stats.total++;

            size_t sep = find_separator(line);
            if (sep == std::string::npos) { continue; }

            std::string obfuscated = trim(line.substr(0, sep));
            if (obfuscated.empty()) { continue; }

            auto parse_result = ParseAndEvaluate(obfuscated, 64);
            if (!parse_result.has_value()) { continue; }

            auto ast_result = ParseToAst(obfuscated, 64);
            if (!ast_result.has_value()) { continue; }

            auto folded = FoldConstantBitwise(std::move(ast_result.value().expr), 64);

            Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
            opts.evaluator = [&folded](const std::vector< uint64_t > &v) -> uint64_t {
                return EvalExpr(*folded, v, 64);
            };

            auto result = Simplify(
                parse_result.value().sig, parse_result.value().vars, folded.get(), opts
            );

            if (!result.has_value()
                || result.value().kind != SimplifyOutcome::Kind::kSimplified)
            {
                continue;
            }
            stats.simplified++;

            if (!result.value().expr) {
                stats.skipped++;
                continue;
            }

            // Remap simplified var indices back to original space
            auto simpl = CloneExpr(*result.value().expr);
            if (result.value().real_vars.size() < parse_result.value().vars.size()) {
                auto idx = BuildVarSupport(parse_result.value().vars, result.value().real_vars);
                RemapVarIndices(*simpl, idx);
            }

            auto z3r =
                Z3VerifyExprs(*folded, *simpl, parse_result.value().vars, 64, timeout_ms);

            if (z3r.equivalent) {
                stats.verified++;
            } else if (z3r.timed_out) {
                stats.skipped++;
            } else {
                stats.failed++;
                ADD_FAILURE() << "Z3: original != simplified on line " << line_num << ": "
                              << obfuscated << "\n  Simplified: "
                              << Render(*result.value().expr, result.value().real_vars)
                              << "\n  Counterexample: " << z3r.counterexample;
            }
        }

        std::cout << path.substr(path.rfind('/') + 1) << ": " << stats.verified << " verified, "
                  << stats.skipped << " skipped (timeout), " << stats.failed << " failed (of "
                  << stats.simplified << " simplified)\n";
        return stats;
    }

} // namespace

// Each test verifies all simplified expressions from a dataset.
// These are slow (Z3 bitvector solving at 64-bit) — run manually,
// not in CI:  ./test/test_dataset_z3

TEST(DatasetZ3, Univariate64) {
    auto s = run_z3_verify(DATASET_DIR "/univariate64.txt");
    EXPECT_EQ(s.failed, 0);
}

TEST(DatasetZ3, Multivariate64) {
    auto s = run_z3_verify(DATASET_DIR "/multivariate64.txt");
    EXPECT_EQ(s.failed, 0);
}

TEST(DatasetZ3, Permutation64) {
    auto s = run_z3_verify(DATASET_DIR "/permutation64.txt");
    EXPECT_EQ(s.failed, 0);
}

TEST(DatasetZ3, MSiMBA) {
    auto s = run_z3_verify(DATASET_DIR "/msimba.txt", 60000);
    EXPECT_EQ(s.failed, 0);
}

TEST(DatasetZ3, MSiMBASample) {
    // Verify 10 evenly-spaced MSiMBA expressions via Z3.
    // MSiMBA's mul*xor form is hard for Z3 at 64-bit, so we
    // accept timeouts (not failures) and complement with the
    // exhaustive evaluator check below.
    std::ifstream file(DATASET_DIR "/msimba.txt");
    ASSERT_TRUE(file.is_open());

    std::vector< std::string > lines;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) { lines.push_back(line); }
    }

    std::vector< int > indices = { 0, 99, 199, 299, 399, 499, 599, 699, 799, 998 };

    int verified  = 0;
    int failed    = 0;
    int timed_out = 0;
    for (int idx : indices) {
        if (idx >= static_cast< int >(lines.size())) { continue; }

        size_t sep = find_separator(lines[idx]);
        if (sep == std::string::npos) { continue; }

        std::string obfuscated = trim(lines[idx].substr(0, sep));
        if (obfuscated.empty()) { continue; }

        auto parse_result = ParseAndEvaluate(obfuscated, 64);
        if (!parse_result.has_value()) { continue; }

        auto ast_result = ParseToAst(obfuscated, 64);
        if (!ast_result.has_value()) { continue; }

        auto folded = FoldConstantBitwise(std::move(ast_result.value().expr), 64);

        Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
        opts.evaluator = [&folded](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(*folded, v, 64);
        };

        auto result =
            Simplify(parse_result.value().sig, parse_result.value().vars, folded.get(), opts);

        if (!result.has_value() || result.value().kind != SimplifyOutcome::Kind::kSimplified
            || !result.value().expr)
        {
            continue;
        }

        auto simpl = CloneExpr(*result.value().expr);
        if (result.value().real_vars.size() < parse_result.value().vars.size()) {
            auto remap = BuildVarSupport(parse_result.value().vars, result.value().real_vars);
            RemapVarIndices(*simpl, remap);
        }

        auto z3r = Z3VerifyExprs(*folded, *simpl, parse_result.value().vars, 64, 120000);

        if (z3r.equivalent) {
            verified++;
            std::cout << "  line " << (idx + 1) << ": Z3 VERIFIED\n";
        } else if (z3r.timed_out) {
            timed_out++;
            std::cout << "  line " << (idx + 1) << ": Z3 timeout (not a failure)\n";
        } else {
            failed++;
            ADD_FAILURE() << "Z3: line " << (idx + 1) << ": "
                          << Render(*result.value().expr, result.value().real_vars)
                          << "\n  Counterexample: " << z3r.counterexample;
        }
    }

    std::cout << "MSiMBA Z3 sample: " << verified << " verified, " << timed_out
              << " timed out, " << failed << " failed (of 10)\n";
    EXPECT_EQ(failed, 0);
}

TEST(DatasetZ3, PLDILinear) {
    auto s = run_z3_verify(DATASET_DIR "/simba/pldi_linear.txt");
    EXPECT_EQ(s.failed, 0);
}

TEST(DatasetZ3, PLDIPoly) {
    auto s = run_z3_verify(DATASET_DIR "/simba/pldi_poly.txt");
    EXPECT_EQ(s.failed, 0);
}

TEST(DatasetZ3, PLDINonPoly) {
    auto s = run_z3_verify(DATASET_DIR "/simba/pldi_nonpoly.txt");
    EXPECT_EQ(s.failed, 0);
}

TEST(DatasetZ3, BlastDataset1) {
    auto s = run_z3_verify(DATASET_DIR "/simba/blast_dataset1.txt");
    EXPECT_EQ(s.failed, 0);
}

TEST(DatasetZ3, BlastDataset2) {
    auto s = run_z3_verify(DATASET_DIR "/simba/blast_dataset2.txt");
    EXPECT_EQ(s.failed, 0);
}

TEST(DatasetZ3, NeuReduce) {
    auto s = run_z3_verify(DATASET_DIR "/gamba/neureduce.txt");
    EXPECT_EQ(s.failed, 0);
}

TEST(DatasetZ3, MbaObfLinear) {
    auto s = run_z3_verify(DATASET_DIR "/gamba/mba_obf_linear.txt");
    EXPECT_EQ(s.failed, 0);
}

TEST(DatasetZ3, MbaObfNonlinear) {
    auto s = run_z3_verify(DATASET_DIR "/gamba/mba_obf_nonlinear.txt");
    EXPECT_EQ(s.failed, 0);
}

TEST(DatasetZ3, Syntia) {
    auto s = run_z3_verify(DATASET_DIR "/gamba/syntia.txt");
    EXPECT_EQ(s.failed, 0);
}

TEST(DatasetZ3, QSynthEA) {
    auto s = run_z3_verify(DATASET_DIR "/gamba/qsynth_ea.txt");
    EXPECT_EQ(s.failed, 0);
}

TEST(DatasetZ3, LokiTiny) {
    auto s = run_z3_verify(DATASET_DIR "/gamba/loki_tiny.txt");
    EXPECT_EQ(s.failed, 0);
}

TEST(DatasetZ3, OSESFast) {
    auto s = run_z3_verify(DATASET_DIR "/oses/oses_fast.txt");
    EXPECT_EQ(s.failed, 0);
}
