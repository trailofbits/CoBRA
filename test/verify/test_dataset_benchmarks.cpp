#include "ExprParser.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/Simplifier.h"
#include <algorithm>
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

    struct DatasetStats
    {
        int total           = 0;
        int parsed          = 0;
        int simplified      = 0;
        int unsupported     = 0;
        int skipped_parse   = 0;
        int failed_simplify = 0;
    };

    DatasetStats run_dataset(const std::string &path) {
        DatasetStats stats;
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
            if (sep == std::string::npos) {
                stats.skipped_parse++;
                continue;
            }

            std::string obfuscated = trim(line.substr(0, sep));
            if (obfuscated.empty()) {
                stats.skipped_parse++;
                continue;
            }

            auto parse_result = ParseAndEvaluate(obfuscated, 64);
            if (!parse_result.has_value()) {
                stats.skipped_parse++;
                continue;
            }
            stats.parsed++;

            auto ast_result        = ParseToAst(obfuscated, 64);
            const Expr *input_expr = nullptr;
            std::unique_ptr< Expr > folded;
            if (ast_result.has_value()) {
                folded     = FoldConstantBitwise(std::move(ast_result.value().expr), 64);
                input_expr = folded.get();
            }

            Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
            if (input_expr) {
                opts.evaluator = [&folded](const std::vector< uint64_t > &v) -> uint64_t {
                    return EvalExpr(*folded, v, 64);
                };
            }

            auto result =
                Simplify(parse_result.value().sig, parse_result.value().vars, input_expr, opts);

            if (!result.has_value()) {
                ADD_FAILURE() << "Unexpected error on line " << line_num << ": "
                              << result.error().message;
                stats.failed_simplify++;
                continue;
            }

            switch (result.value().kind) {
                case SimplifyOutcome::Kind::kSimplified:
                    stats.simplified++;
                    break;
                case SimplifyOutcome::Kind::kUnchangedUnsupported:
                    stats.unsupported++;
                    break;
                case SimplifyOutcome::Kind::kError:
                    ADD_FAILURE() << "BugOrGap on line " << line_num << ": "
                                  << result.value().diag.reason;
                    stats.failed_simplify++;
                    break;
            }
        }

        return stats;
    }

} // namespace

TEST(DatasetBenchmark, Univariate64) {
    auto stats = run_dataset(DATASET_DIR "/univariate64.txt");
    EXPECT_EQ(stats.total, 1000);
    EXPECT_EQ(stats.skipped_parse, 0);
    EXPECT_EQ(stats.parsed, 1000);
    // All 1000 are polynomial (x0*x0 terms) but simplify to
    // linear targets (c*x0). Full-width verified.
    EXPECT_EQ(stats.simplified, 1000);
    EXPECT_EQ(stats.unsupported, 0);
    EXPECT_EQ(stats.failed_simplify, 0);
}

TEST(DatasetBenchmark, Multivariate64) {
    auto stats = run_dataset(DATASET_DIR "/multivariate64.txt");
    EXPECT_EQ(stats.total, 1000);
    EXPECT_EQ(stats.skipped_parse, 0);
    EXPECT_EQ(stats.parsed, 1000);
    // 994 polynomial (x0*x1 terms) simplify to linear targets.
    // All 1000 pass full-width verification.
    EXPECT_EQ(stats.simplified, 1000);
    EXPECT_EQ(stats.unsupported, 0);
    EXPECT_EQ(stats.failed_simplify, 0);
}

TEST(DatasetBenchmark, Permutation64) {
    auto stats = run_dataset(DATASET_DIR "/permutation64.txt");
    EXPECT_EQ(stats.total, 13);
    EXPECT_EQ(stats.skipped_parse, 0);
    EXPECT_EQ(stats.parsed, 13);
    EXPECT_EQ(stats.simplified, 13);
    EXPECT_EQ(stats.unsupported, 0);
    EXPECT_EQ(stats.failed_simplify, 0);
}

TEST(DatasetBenchmark, MSiMBA) {
    auto stats = run_dataset(DATASET_DIR "/msimba.txt");
    EXPECT_EQ(stats.total, 1000);
    EXPECT_EQ(stats.skipped_parse, 0);
    EXPECT_EQ(stats.parsed, 1000);
    EXPECT_EQ(stats.simplified, 448);
    EXPECT_EQ(stats.unsupported, 552);
    EXPECT_EQ(stats.failed_simplify, 0);
}

// -- SiMBA datasets (https://github.com/pgarba/SiMBA-) ------------------

// Parameterized test for the e1-e5 expression datasets (2-5 variables).
// Each file has 1001 lines: 1 comment header + 1000 data lines.
struct SiMBAParam
{
    std::string file;
    const char *name;
};

class SiMBAExprDataset : public ::testing::TestWithParam< SiMBAParam >
{};

TEST_P(SiMBAExprDataset, SimplifiesAll) {
    auto stats = run_dataset(DATASET_DIR "/simba/" + GetParam().file);
    EXPECT_EQ(stats.total, 1001);
    EXPECT_EQ(stats.skipped_parse, 1);
    EXPECT_EQ(stats.parsed, 1000);
    EXPECT_EQ(stats.simplified, 1000);
    EXPECT_EQ(stats.unsupported, 0);
    EXPECT_EQ(stats.failed_simplify, 0);
}

INSTANTIATE_TEST_SUITE_P(
    SiMBA, SiMBAExprDataset,
    ::testing::Values(
        SiMBAParam{ "e1_2vars.txt", "E1_2Vars" }, SiMBAParam{ "e1_3vars.txt", "E1_3Vars" },
        SiMBAParam{ "e1_4vars.txt", "E1_4Vars" }, SiMBAParam{ "e1_5vars.txt", "E1_5Vars" },
        SiMBAParam{ "e2_2vars.txt", "E2_2Vars" }, SiMBAParam{ "e2_3vars.txt", "E2_3Vars" },
        SiMBAParam{ "e2_4vars.txt", "E2_4Vars" }, SiMBAParam{ "e3_2vars.txt", "E3_2Vars" },
        SiMBAParam{ "e3_3vars.txt", "E3_3Vars" }, SiMBAParam{ "e3_4vars.txt", "E3_4Vars" },
        SiMBAParam{ "e4_2vars.txt", "E4_2Vars" }, SiMBAParam{ "e4_3vars.txt", "E4_3Vars" },
        SiMBAParam{ "e4_4vars.txt", "E4_4Vars" }, SiMBAParam{ "e5_2vars.txt", "E5_2Vars" },
        SiMBAParam{ "e5_3vars.txt", "E5_3Vars" }, SiMBAParam{ "e5_4vars.txt", "E5_4Vars" }
    ),
    [](const auto &info) { return info.param.name; }
);

TEST(SiMBADataset, PLDILinear) {
    auto stats = run_dataset(DATASET_DIR "/simba/pldi_linear.txt");
    // 4 comment lines (#complex,groundtruth, #N-variable headers)
    EXPECT_EQ(stats.total, 1012);
    EXPECT_EQ(stats.skipped_parse, 4);
    EXPECT_EQ(stats.parsed, 1008);
    EXPECT_EQ(stats.simplified, 1008);
    EXPECT_EQ(stats.unsupported, 0);
    EXPECT_EQ(stats.failed_simplify, 0);
}

TEST(SiMBADataset, PLDIPoly) {
    auto stats = run_dataset(DATASET_DIR "/simba/pldi_poly.txt");
    EXPECT_EQ(stats.total, 1009);
    EXPECT_EQ(stats.skipped_parse, 1);
    EXPECT_EQ(stats.parsed, 1008);
    EXPECT_EQ(stats.simplified, 1008);
    EXPECT_EQ(stats.unsupported, 0);
    EXPECT_EQ(stats.failed_simplify, 0);
}

TEST(SiMBADataset, PLDINonPoly) {
    auto stats = run_dataset(DATASET_DIR "/simba/pldi_nonpoly.txt");
    EXPECT_EQ(stats.total, 1004);
    EXPECT_EQ(stats.skipped_parse, 1); // #complex,groundtruth header
    EXPECT_EQ(stats.parsed, 1003);
    // 844 linear + 55 polynomial + 92 mixed + 12 product identity
    EXPECT_EQ(stats.simplified, 1003);
    EXPECT_EQ(stats.unsupported, 0);
    EXPECT_EQ(stats.failed_simplify, 0);
}

TEST(SiMBADataset, TestData) {
    auto stats = run_dataset(DATASET_DIR "/simba/test_data.txt");
    EXPECT_EQ(stats.total, 10000);
    EXPECT_EQ(stats.skipped_parse, 0);
    EXPECT_EQ(stats.parsed, 10000);
    EXPECT_EQ(stats.simplified, 10000);
    EXPECT_EQ(stats.unsupported, 0);
    EXPECT_EQ(stats.failed_simplify, 0);
}

TEST(SiMBADataset, BlastDataset1) {
    auto stats = run_dataset(DATASET_DIR "/simba/blast_dataset1.txt");
    EXPECT_EQ(stats.total, 63);
    EXPECT_EQ(stats.skipped_parse, 1);
    EXPECT_EQ(stats.parsed, 62);
    EXPECT_EQ(stats.simplified, 62);
    EXPECT_EQ(stats.unsupported, 0);
    EXPECT_EQ(stats.failed_simplify, 0);
}

TEST(SiMBADataset, BlastDataset2) {
    auto stats = run_dataset(DATASET_DIR "/simba/blast_dataset2.txt");
    EXPECT_EQ(stats.total, 2501);
    EXPECT_EQ(stats.skipped_parse, 1);
    EXPECT_EQ(stats.parsed, 2500);
    EXPECT_EQ(stats.simplified, 2500);
    EXPECT_EQ(stats.unsupported, 0);
    EXPECT_EQ(stats.failed_simplify, 0);
}

// -- GAMBA datasets (https://github.com/DenuvoSoftwareSolutions/GAMBA) ----

TEST(GAMBADataset, NeuReduce) {
    auto stats = run_dataset(DATASET_DIR "/gamba/neureduce.txt");
    EXPECT_EQ(stats.total, 10000);
    EXPECT_EQ(stats.skipped_parse, 0);
    EXPECT_EQ(stats.parsed, 10000);
    EXPECT_EQ(stats.simplified, 10000);
    EXPECT_EQ(stats.unsupported, 0);
    EXPECT_EQ(stats.failed_simplify, 0);
}

TEST(GAMBADataset, MbaObfLinear) {
    auto stats = run_dataset(DATASET_DIR "/gamba/mba_obf_linear.txt");
    EXPECT_EQ(stats.total, 1001);
    EXPECT_EQ(stats.skipped_parse, 1); // #linear,groundtruth header
    EXPECT_EQ(stats.parsed, 1000);
    EXPECT_EQ(stats.simplified, 1000);
    EXPECT_EQ(stats.unsupported, 0);
    EXPECT_EQ(stats.failed_simplify, 0);
}

TEST(GAMBADataset, MbaObfNonlinear) {
    auto stats = run_dataset(DATASET_DIR "/gamba/mba_obf_nonlinear.txt");
    EXPECT_EQ(stats.total, 1002);
    EXPECT_EQ(stats.skipped_parse, 2); // #poly + #nonpoly headers
    EXPECT_EQ(stats.parsed, 1000);
    // 500 linear (nonpoly section), 500 polynomial with linear
    // targets. All 1000 pass full-width verification.
    EXPECT_EQ(stats.simplified, 1000);
    EXPECT_EQ(stats.unsupported, 0);
    EXPECT_EQ(stats.failed_simplify, 0);
}

TEST(GAMBADataset, Syntia) {
    auto stats = run_dataset(DATASET_DIR "/gamba/syntia.txt");
    EXPECT_EQ(stats.total, 501);
    EXPECT_EQ(stats.skipped_parse, 1); // header
    EXPECT_EQ(stats.parsed, 500);
    EXPECT_EQ(stats.simplified, 480);
    EXPECT_EQ(stats.unsupported, 20);
    EXPECT_EQ(stats.failed_simplify, 0);
}

TEST(GAMBADataset, QSynthEA) {
    auto stats = run_dataset(DATASET_DIR "/gamba/qsynth_ea.txt");
    EXPECT_EQ(stats.total, 501);
    EXPECT_EQ(stats.skipped_parse, 1); // header only
    EXPECT_EQ(stats.parsed, 500);
    EXPECT_EQ(stats.simplified, 288);
    EXPECT_EQ(stats.unsupported, 212);
    EXPECT_EQ(stats.failed_simplify, 0);
}

TEST(GAMBADataset, LokiTiny) {
    auto stats = run_dataset(DATASET_DIR "/gamba/loki_tiny.txt");
    EXPECT_EQ(stats.total, 25025);
    EXPECT_EQ(stats.skipped_parse, 25); // 25 section headers
    EXPECT_EQ(stats.parsed, 25000);
    // All are linear 2-var (x+y, x&y, x|y, x-y, x^y)
    EXPECT_EQ(stats.simplified, 25000);
    EXPECT_EQ(stats.unsupported, 0);
    EXPECT_EQ(stats.failed_simplify, 0);
}

// -- OSES dataset (oracle-synthesis-meets-equality-saturation) ----

TEST(OSESDataset, All) {
    auto stats = run_dataset(DATASET_DIR "/oses/oses_all.txt");
    EXPECT_EQ(stats.total, 473);
    EXPECT_EQ(stats.skipped_parse, 7);
    EXPECT_EQ(stats.parsed, 466);
    EXPECT_EQ(stats.simplified, 389);
    EXPECT_EQ(stats.unsupported, 77);
    EXPECT_EQ(stats.failed_simplify, 0);
}
