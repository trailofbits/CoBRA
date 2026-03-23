#include "ExprParser.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/MixedProductRewriter.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/Simplifier.h"
#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <random>
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

TEST(MSiMBAGroundTruth, SimplifiedASTSize) {
    std::ifstream file(DATASET_DIR "/msimba.txt");
    ASSERT_TRUE(file.is_open());

    int total            = 0;
    int simplified       = 0;
    uint64_t total_nodes = 0;
    int structural_match = 0;
    int max_nodes        = 0;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) { continue; }

        size_t sep = find_separator(line);
        if (sep == std::string::npos) { continue; }

        std::string obfuscated = trim(line.substr(0, sep));
        std::string gt_str     = trim(line.substr(sep + 1));
        if (obfuscated.empty()) { continue; }
        total++;

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

        if (!result.has_value() || result.value().kind != SimplifyOutcome::Kind::kSimplified) {
            continue;
        }
        if (!result.value().expr) { continue; }
        simplified++;

        auto nodes   = NodeCount(*result.value().expr);
        total_nodes += nodes;
        if (static_cast< int >(nodes) > max_nodes) { max_nodes = static_cast< int >(nodes); }

        std::string rendered = Render(*result.value().expr, result.value().real_vars);
        if (rendered == gt_str) { structural_match++; }
    }

    double avg = simplified > 0 ? static_cast< double >(total_nodes) / simplified : 0;

    std::cerr << "\nMSiMBA AST Size: avg=" << avg << " max=" << max_nodes
              << " match=" << structural_match << "/" << total << "\n";

    EXPECT_EQ(simplified, 1000);
    EXPECT_LE(max_nodes, 10);
    EXPECT_GE(structural_match, 990);
}

// 1000 random probes per expression (1M total evaluator checks).
TEST(MSiMBAGroundTruth, ExhaustiveEvalCheck) {
    std::ifstream file(DATASET_DIR "/msimba.txt");
    ASSERT_TRUE(file.is_open());

    constexpr int kProbesPerExpr = 1000;
    std::mt19937_64 rng(42);

    int total      = 0;
    int simplified = 0;
    int failed     = 0;

    std::string line;
    int line_num = 0;
    while (std::getline(file, line)) {
        ++line_num;
        if (line.empty()) { continue; }

        size_t sep = find_separator(line);
        if (sep == std::string::npos) { continue; }

        std::string obfuscated = trim(line.substr(0, sep));
        if (obfuscated.empty()) { continue; }
        total++;

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
        simplified++;

        auto simpl = CloneExpr(*result.value().expr);
        if (result.value().real_vars.size() < parse_result.value().vars.size()) {
            auto idx = BuildVarSupport(parse_result.value().vars, result.value().real_vars);
            RemapVarIndices(*simpl, idx);
        }

        const auto nvars = parse_result.value().vars.size();
        std::vector< uint64_t > vals(nvars);

        for (int probe = 0; probe < kProbesPerExpr; ++probe) {
            for (size_t vi = 0; vi < nvars; ++vi) { vals[vi] = rng(); }
            uint64_t orig = EvalExpr(*folded, vals, 64);
            uint64_t simp = EvalExpr(*simpl, vals, 64);
            if (orig != simp) {
                failed++;
                ADD_FAILURE() << "Mismatch on line " << line_num << " probe " << probe
                              << ": orig=0x" << std::hex << orig << " simp=0x" << simp
                              << std::dec;
                break;
            }
        }
    }

    std::cerr << "\nMSiMBA Eval: " << simplified << " simplified, " << failed << " failed ("
              << kProbesPerExpr << " probes/expr)\n";
    EXPECT_EQ(failed, 0);
    EXPECT_EQ(simplified, 1000);
}
