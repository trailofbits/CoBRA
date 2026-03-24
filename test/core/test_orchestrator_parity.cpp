#include "ExprParser.h"
#include "Orchestrator.h"
#include "OrchestratorPasses.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/Simplifier.h"
#include "cobra/core/SimplifyOutcome.h"
#include <fstream>
#include <gtest/gtest.h>
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

    bool ReasonCodeEqual(const ReasonCode &a, const ReasonCode &b) {
        return a.category == b.category && a.domain == b.domain && a.subcode == b.subcode;
    }

    // Compare the stable contract surface of two SimplifyOutcome
    // results. Returns empty string on match, or a description of
    // the first divergence found.
    std::string StableContractDiff(
        const Result< SimplifyOutcome > &legacy, const Result< SimplifyOutcome > &orchestrated
    ) {
        if (legacy.has_value() != orchestrated.has_value()) {
            return "success/error mismatch: legacy="
                + std::string(legacy.has_value() ? "ok" : "err")
                + " orchestrated=" + std::string(orchestrated.has_value() ? "ok" : "err");
        }

        if (!legacy.has_value()) {
            if (legacy.error().code != orchestrated.error().code) {
                return "error code mismatch";
            }
            return {};
        }

        const auto &a = legacy.value();
        const auto &b = orchestrated.value();

        if (a.kind != b.kind) {
            return "kind mismatch: legacy=" + std::to_string(static_cast< int >(a.kind))
                + " orchestrated=" + std::to_string(static_cast< int >(b.kind));
        }

        if (a.kind == SimplifyOutcome::Kind::kSimplified) {
            if (static_cast< bool >(a.expr) != static_cast< bool >(b.expr)) {
                return "expr null mismatch";
            }
            if (a.expr && b.expr) {
                auto ra = Render(*a.expr, a.real_vars);
                auto rb = Render(*b.expr, b.real_vars);
                if (ra != rb) { return "rendered expr: legacy=" + ra + " orchestrated=" + rb; }
            }
            if (a.real_vars != b.real_vars) { return "real_vars mismatch"; }
            if (a.verified != b.verified) {
                return "verified mismatch: legacy=" + std::to_string(a.verified)
                    + " orchestrated=" + std::to_string(b.verified);
            }
            if (a.sig_vector != b.sig_vector) { return "sig_vector mismatch"; }
        }

        if (a.kind == SimplifyOutcome::Kind::kUnchangedUnsupported) {
            bool a_has = a.diag.reason_code.has_value();
            bool b_has = b.diag.reason_code.has_value();
            if (a_has != b_has) { return "reason_code presence mismatch"; }
            if (a_has) {
                if (!ReasonCodeEqual(*a.diag.reason_code, *b.diag.reason_code)) {
                    return "reason_code triple mismatch: legacy=("
                        + std::to_string(static_cast< int >(a.diag.reason_code->category)) + ","
                        + std::to_string(static_cast< int >(a.diag.reason_code->domain)) + ","
                        + std::to_string(a.diag.reason_code->subcode) + ") orchestrated=("
                        + std::to_string(static_cast< int >(b.diag.reason_code->category)) + ","
                        + std::to_string(static_cast< int >(b.diag.reason_code->domain)) + ","
                        + std::to_string(b.diag.reason_code->subcode) + ")";
                }
            }

            if (a.diag.attempted_route != b.diag.attempted_route) {
                return "attempted_route mismatch: legacy="
                    + std::to_string(static_cast< int >(a.diag.attempted_route))
                    + " orchestrated="
                    + std::to_string(static_cast< int >(b.diag.attempted_route));
            }
            if (a.diag.rewrite_produced_candidate != b.diag.rewrite_produced_candidate) {
                return "rewrite_produced_candidate mismatch: legacy="
                    + std::to_string(a.diag.rewrite_produced_candidate)
                    + " orchestrated=" + std::to_string(b.diag.rewrite_produced_candidate);
            }
            if (a.diag.candidate_failed_verification != b.diag.candidate_failed_verification) {
                return "candidate_failed_verification mismatch: legacy="
                    + std::to_string(a.diag.candidate_failed_verification)
                    + " orchestrated=" + std::to_string(b.diag.candidate_failed_verification);
            }

            if (a.diag.cause_chain.size() != b.diag.cause_chain.size()) {
                return "cause_chain size mismatch: legacy="
                    + std::to_string(a.diag.cause_chain.size())
                    + " orchestrated=" + std::to_string(b.diag.cause_chain.size());
            }
            for (size_t i = 0; i < a.diag.cause_chain.size(); ++i) {
                if (!ReasonCodeEqual(a.diag.cause_chain[i].code, b.diag.cause_chain[i].code)) {
                    return "cause_chain[" + std::to_string(i) + "] code mismatch";
                }
            }
        }

        return {};
    }

    struct ParityResult
    {
        uint32_t total      = 0;
        uint32_t compared   = 0;
        uint32_t mismatches = 0;
        std::vector< std::string > first_diffs;
    };

    ParityResult run_parity(const std::string &path) {
        ParityResult result;
        std::ifstream file(path);
        if (!file.is_open()) { return result; }

        OrchestratorPolicy strict{
            .allow_reroute         = false,
            .strict_route_faithful = true,
        };

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) { continue; }
            result.total++;

            size_t sep = find_separator(line);
            if (sep == std::string::npos) { continue; }
            std::string obfuscated = trim(line.substr(0, sep));
            if (obfuscated.empty()) { continue; }

            auto parse_result = ParseAndEvaluate(obfuscated, 64);
            if (!parse_result.has_value()) { continue; }

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

            auto legacy =
                Simplify(parse_result.value().sig, parse_result.value().vars, input_expr, opts);
            auto orchestrated = OrchestrateSimplifyForTest(
                input_expr, parse_result.value().sig, parse_result.value().vars, opts, strict
            );

            result.compared++;
            auto diff = StableContractDiff(legacy, orchestrated);
            if (!diff.empty()) {
                result.mismatches++;
                if (result.first_diffs.size() < 5) {
                    result.first_diffs.push_back(
                        "Line " + std::to_string(result.total) + ": " + diff + " | "
                        + obfuscated.substr(0, 80)
                    );
                }
            }
        }

        return result;
    }

    struct DatasetParam
    {
        std::string path;
        std::string name;
    };

} // namespace

class ParityTest : public ::testing::TestWithParam< DatasetParam >
{};

TEST_P(ParityTest, OrchestratorMatchesLegacy) {
    std::string full = std::string(DATASET_DIR) + "/" + GetParam().path;
    auto result      = run_parity(full);
    EXPECT_GT(result.compared, 0u) << "No expressions compared for " << GetParam().path;

    if (!result.first_diffs.empty()) {
        std::string details;
        for (const auto &d : result.first_diffs) { details += "  " + d + "\n"; }
        EXPECT_EQ(result.mismatches, 0u) << GetParam().path << ": " << result.mismatches << "/"
                                         << result.compared << " parity failures\n"
                                         << details;
    } else {
        EXPECT_EQ(result.mismatches, 0u);
    }
}

INSTANTIATE_TEST_SUITE_P(
    Datasets, ParityTest,
    ::testing::Values(
        DatasetParam{ "oses/oses_fast.txt", "OSES_Fast" },
        DatasetParam{ "gamba/qsynth_ea.txt", "QSynth_EA" },
        DatasetParam{ "gamba/syntia.txt", "Syntia" }, DatasetParam{ "msimba.txt", "MSiMBA" },
        DatasetParam{ "simba/pldi_linear.txt", "PLDI_Linear" },
        DatasetParam{ "simba/pldi_poly.txt", "PLDI_Poly" },
        DatasetParam{ "simba/pldi_nonpoly.txt", "PLDI_NonPoly" },
        DatasetParam{ "simba/e1_2vars.txt", "E1_2Vars" },
        DatasetParam{ "simba/e2_2vars.txt", "E2_2Vars" },
        DatasetParam{ "simba/e3_2vars.txt", "E3_2Vars" },
        DatasetParam{ "simba/e4_2vars.txt", "E4_2Vars" },
        DatasetParam{ "simba/e5_2vars.txt", "E5_2Vars" },
        DatasetParam{ "simba/e5_3vars.txt", "E5_3Vars" },
        DatasetParam{ "simba/e5_4vars.txt", "E5_4Vars" }
    ),
    [](const auto &info) { return info.param.name; }
);
