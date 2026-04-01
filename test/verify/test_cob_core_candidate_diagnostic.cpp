#include "ExprParser.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/Simplifier.h"
#include <cstdint>
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

    struct RegressionCase
    {
        int line_num;
        std::string mba;
        std::string ground_truth;
    };

    // Run Simplify and return the outcome
    SimplifyOutcome run_simplify(const std::string &mba) {
        auto parse_result = ParseAndEvaluate(mba, 64);
        EXPECT_TRUE(parse_result.has_value()) << "Failed to parse: " << mba;

        auto ast_result = ParseToAst(mba, 64);
        EXPECT_TRUE(ast_result.has_value());

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
        EXPECT_TRUE(result.has_value());
        return std::move(result.value());
    }

    // Run Simplify WITHOUT the evaluator (no full-width check)
    SimplifyOutcome run_simplify_no_eval(const std::string &mba) {
        auto parse_result = ParseAndEvaluate(mba, 64);
        EXPECT_TRUE(parse_result.has_value()) << "Failed to parse: " << mba;

        auto ast_result = ParseToAst(mba, 64);
        EXPECT_TRUE(ast_result.has_value());

        auto folded_ptr = std::make_shared< std::unique_ptr< Expr > >(
            FoldConstantBitwise(std::move(ast_result.value().expr), 64)
        );

        Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

        auto result = Simplify(
            parse_result.value().sig, parse_result.value().vars, folded_ptr->get(), opts
        );
        EXPECT_TRUE(result.has_value());
        return std::move(result.value());
    }

} // namespace

// Targeted diagnostic for the Syntia regression introduced by 7cde94a
// ("emit CoB as core candidate on full-width failure").
//
// Two expressions regressed from simplified to unsupported:
//   L66:  ((((~e|b)+e)+1)&d)*((((~e|b)+e)+1)|d)+((((~e|b)+e)+1)&~d)*(~(((~e|b)+e)+1)&d)
//         GT: ((e&b)*d)
//   L334: (((~a|d)-~a)&c)*(((~a|d)-~a)|c)+(((~a|d)-~a)&~c)*(~((~a|d)-~a)&c)
//         GT: ((a&d)*c)
//
// Before 7cde94a, CoB found the boolean structure, full-width check failed,
// and the expression was retained for other passes which succeeded.
// After 7cde94a, the FW-failed CoB is emitted as a CoreCandidatePayload,
// routing it to the residual-recovery path which fails.

TEST(CoBCoreCandidateDiagnostic, SyntiaRegressions) {
    std::vector< RegressionCase > cases = {
        {  66, "((((~e|b)+e)+1)&d)*((((~e|b)+e)+1)|d)+((((~e|b)+e)+1)&~d)*(~(((~e|b)+e)+1)&d)",
         "((e&b)*d)" },
        { 334,             "(((~a|d)-~a)&c)*(((~a|d)-~a)|c)+(((~a|d)-~a)&~c)*(~((~a|d)-~a)&c)",
         "((a&d)*c)" },
    };

    for (const auto &tc : cases) {
        std::cerr << "\n═══════════════════════════════════════════\n";
        std::cerr << "Syntia L" << tc.line_num << "\n";
        std::cerr << "═══════════════════════════════════════════\n";
        std::cerr << "  MBA: " << tc.mba << "\n";
        std::cerr << "  GT:  " << tc.ground_truth << "\n";

        // Classify the input
        auto ast = ParseToAst(tc.mba, 64);
        ASSERT_TRUE(ast.has_value());
        auto folded = FoldConstantBitwise(std::move(ast.value().expr), 64);
        auto cls    = ClassifyStructural(*folded);
        auto parse  = ParseAndEvaluate(tc.mba, 64);
        ASSERT_TRUE(parse.has_value());
        auto elim = EliminateAuxVars(parse.value().sig, parse.value().vars);

        std::cerr << "  Semantic: " << semantic_str(cls.semantic) << "\n";
        std::cerr << "  Flags:    {" << flag_str(cls.flags) << "}\n";
        std::cerr << "  Vars:     " << parse.value().vars.size()
                  << " (real: " << elim.real_vars.size() << ")\n";

        // Run WITH evaluator (what the test framework does)
        std::cerr << "\n  --- With evaluator (full-width check) ---\n";
        auto result_eval = run_simplify(tc.mba);
        std::cerr << "  Kind: "
                  << (result_eval.kind == SimplifyOutcome::Kind::kSimplified ? "SIMPLIFIED"
                                                                             : "UNSUPPORTED")
                  << "\n";
        if (result_eval.kind == SimplifyOutcome::Kind::kSimplified) {
            std::cerr << "  Result: " << Render(*result_eval.expr, result_eval.real_vars, 64)
                      << "\n";
            std::cerr << "  Verified: " << (result_eval.verified ? "yes" : "no") << "\n";
        } else {
            std::cerr << "  Reason: " << result_eval.diag.reason << "\n";
            if (result_eval.diag.reason_code.has_value()) {
                std::cerr << "  ReasonCode: cat="
                          << static_cast< int >(result_eval.diag.reason_code->category)
                          << " domain="
                          << static_cast< int >(result_eval.diag.reason_code->domain)
                          << " sub=" << result_eval.diag.reason_code->subcode << "\n";
            }
            if (!result_eval.diag.cause_chain.empty()) {
                std::cerr << "  Cause chain (" << result_eval.diag.cause_chain.size()
                          << " frames):\n";
                for (const auto &frame : result_eval.diag.cause_chain) {
                    std::cerr << "    - [cat=" << static_cast< int >(frame.code.category)
                              << " domain=" << static_cast< int >(frame.code.domain)
                              << " sub=" << frame.code.subcode << "] " << frame.message << "\n";
                }
            }
        }

        // Telemetry
        std::cerr << "  Telemetry: expansions=" << result_eval.telemetry.total_expansions
                  << " depth=" << result_eval.telemetry.max_depth_reached
                  << " verified=" << result_eval.telemetry.candidates_verified
                  << " hwm=" << result_eval.telemetry.queue_high_water << "\n";

        // Run WITHOUT evaluator (no FW check — CoB would succeed)
        std::cerr << "\n  --- Without evaluator (boolean domain only) ---\n";
        auto result_no_eval = run_simplify_no_eval(tc.mba);
        std::cerr << "  Kind: "
                  << (result_no_eval.kind == SimplifyOutcome::Kind::kSimplified ? "SIMPLIFIED"
                                                                                : "UNSUPPORTED")
                  << "\n";
        if (result_no_eval.kind == SimplifyOutcome::Kind::kSimplified) {
            std::cerr << "  Result: "
                      << Render(*result_no_eval.expr, result_no_eval.real_vars, 64) << "\n";

            // Spot-check: does the CoB result match at full width?
            auto parse2 = ParseAndEvaluate(tc.mba, 64);
            auto ast2   = ParseToAst(tc.mba, 64);
            auto orig   = FoldConstantBitwise(std::move(ast2.value().expr), 64);

            std::vector< uint64_t > test_vals = {
                0x5555555555555555ULL,
                0xAAAAAAAAAAAAAAAAULL,
                0x0123456789ABCDEFULL,
            };
            std::cerr << "  Full-width spot checks:\n";
            for (size_t i = 0; i < test_vals.size(); ++i) {
                std::vector< uint64_t > input(parse2.value().vars.size(), test_vals[i]);
                uint64_t orig_val = EvalExpr(*orig, input, 64);
                uint64_t cob_val  = EvalExpr(*result_no_eval.expr, input, 64);
                bool match        = (orig_val == cob_val);
                std::cerr << "    input=0x" << std::hex << test_vals[i] << ": orig=0x"
                          << orig_val << " cob=0x" << cob_val << (match ? " OK" : " MISMATCH")
                          << std::dec << "\n";
            }
        }

        std::cerr << "  Telemetry: expansions=" << result_no_eval.telemetry.total_expansions
                  << " depth=" << result_no_eval.telemetry.max_depth_reached
                  << " verified=" << result_no_eval.telemetry.candidates_verified
                  << " hwm=" << result_no_eval.telemetry.queue_high_water << "\n";

        // The actual assertion: these should simplify
        EXPECT_EQ(result_eval.kind, SimplifyOutcome::Kind::kSimplified)
            << "Syntia L" << tc.line_num << " regressed to unsupported";
    }

    std::cerr << "\n";
}

// Scan Syntia for ALL expressions that differ between eval/no-eval paths
// to find the full scope of the CoB core-candidate regression
TEST(CoBCoreCandidateDiagnostic, SyntiaFullScan) {
    std::ifstream file(DATASET_DIR "/gamba/syntia.txt");
    ASSERT_TRUE(file.is_open());

    int parsed = 0, simplified = 0, unsupported = 0;
    int cob_divergent = 0; // simplified without eval, unsupported with eval

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

        parsed++;

        auto result_eval    = run_simplify(obfuscated);
        auto result_no_eval = run_simplify_no_eval(obfuscated);

        if (result_eval.kind == SimplifyOutcome::Kind::kSimplified) {
            simplified++;
        } else {
            unsupported++;
            std::string gt = trim(line.substr(sep + 1));
            std::cerr << "  L" << line_num << " UNSUPPORTED\n";
            std::cerr << "    MBA: " << obfuscated.substr(0, 80) << "\n";
            std::cerr << "    GT:  " << gt << "\n";
            std::cerr << "    Reason: " << result_eval.diag.reason << "\n";
            if (result_eval.diag.reason_code.has_value()) {
                std::cerr << "    ReasonCode: cat="
                          << static_cast< int >(result_eval.diag.reason_code->category)
                          << " domain="
                          << static_cast< int >(result_eval.diag.reason_code->domain)
                          << " sub=" << result_eval.diag.reason_code->subcode << "\n";
            }
            if (!result_eval.diag.cause_chain.empty()) {
                std::cerr << "    Cause chain:\n";
                for (const auto &frame : result_eval.diag.cause_chain) {
                    std::cerr << "      - " << frame.message << "\n";
                }
            }
            std::cerr << "    Telemetry: expansions=" << result_eval.telemetry.total_expansions
                      << " depth=" << result_eval.telemetry.max_depth_reached
                      << " verified=" << result_eval.telemetry.candidates_verified << "\n";
            bool no_eval_unsup = result_no_eval.kind != SimplifyOutcome::Kind::kSimplified;
            std::cerr << "    Also unsupported without eval: " << (no_eval_unsup ? "YES" : "no")
                      << "\n";
        }

        bool eval_ok    = result_eval.kind == SimplifyOutcome::Kind::kSimplified;
        bool no_eval_ok = result_no_eval.kind == SimplifyOutcome::Kind::kSimplified;

        if (no_eval_ok && !eval_ok) {
            cob_divergent++;
            std::string gt = trim(line.substr(sep + 1));
            std::cerr << "  L" << line_num << " divergent: CoB succeeds, FW-checked fails\n";
            std::cerr << "    GT: " << gt << "\n";
            std::cerr << "    CoB: "
                      << Render(*result_no_eval.expr, result_no_eval.real_vars, 64) << "\n";
            if (result_eval.diag.reason_code.has_value()) {
                std::cerr << "    Reason: cat="
                          << static_cast< int >(result_eval.diag.reason_code->category)
                          << " domain="
                          << static_cast< int >(result_eval.diag.reason_code->domain) << "\n";
            }
            if (!result_eval.diag.cause_chain.empty()) {
                std::cerr << "    Terminal cause: "
                          << result_eval.diag.cause_chain.back().message << "\n";
            }
        }
    }

    std::cerr << "\n=== CoB Core-Candidate Divergence Scan ===\n";
    std::cerr << "parsed=" << parsed << " simplified=" << simplified
              << " unsupported=" << unsupported << "\n";
    std::cerr << "CoB-divergent (simplified without eval, unsupported with): " << cob_divergent
              << "\n\n";
}
