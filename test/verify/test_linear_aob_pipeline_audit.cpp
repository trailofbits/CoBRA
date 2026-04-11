/// Linear AoB pipeline audit.
///
/// For expensive solved linear AoB expressions, answers:
///   A. Does the direct linear solve (signature passes) produce
///      a valid candidate before exploration?
///   B. Is the final accepted solution equivalent in cost to
///      the direct linear candidate?
///   C. What gates prevent early termination?
///
/// The approach: run each expression TWICE:
///   1. Full pipeline (normal) — records time, cost, telemetry
///   2. Forced non-exploration path — set flags to avoid
///      IsFoldedAstExplorationCandidate, measuring the direct
///      algebraic solve alone
///
/// If the non-exploration path succeeds with equivalent or better
/// cost, exploration was pure waste.

#include "ExprParser.h"
#include "cobra/core/Classification.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/Simplifier.h"
#include "cobra/core/SimplifyOutcome.h"
#include "dataset_audit_utils.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace cobra;
using cobra::test_support::find_separator;
using cobra::test_support::semantic_str;
using cobra::test_support::trim;

namespace {

    constexpr uint32_t kBw = 64;

    struct PipelineComparison
    {
        int line_num = 0;

        // Baseline classification
        SemanticClass sem_class       = SemanticClass::kLinear;
        StructuralFlag flags          = kSfNone;
        bool is_exploration_candidate = false;

        // Full pipeline (normal run)
        double full_time_ms      = 0.0;
        bool full_simplified     = false;
        uint32_t full_cost       = 0;
        uint32_t full_expansions = 0;
        uint32_t full_depth      = 0;
        uint32_t full_candidates = 0;
        uint32_t full_queue_hw   = 0;

        // Direct algebraic solve (BoA flag masked)
        double direct_time_ms      = 0.0;
        bool direct_simplified     = false;
        uint32_t direct_cost       = 0;
        uint32_t direct_expansions = 0;
        uint32_t direct_depth      = 0;
        uint32_t direct_candidates = 0;

        // Comparison
        bool direct_succeeded   = false;
        bool cost_equal         = false;
        bool cost_better_direct = false;
        bool cost_worse_direct  = false;
        double speedup          = 0.0;

        // Correctness check: does direct result match full result?
        bool results_match = false;
    };

    struct DatasetConfig
    {
        std::string name;
        std::string path;
    };

    void run_pipeline_comparison(
        const DatasetConfig &ds, std::vector< PipelineComparison > &records, double min_time_ms
    ) {
        std::ifstream file(ds.path);
        if (!file.is_open()) {
            std::cerr << "Cannot open " << ds.path << "\n";
            return;
        }

        std::string line;
        int line_num = 0;

        while (std::getline(file, line)) {
            ++line_num;
            if (line.empty()) { continue; }

            size_t sep = find_separator(line);
            if (sep == std::string::npos) { continue; }

            std::string obfuscated = trim(line.substr(0, sep));
            if (obfuscated.empty()) { continue; }

            auto parse_result = ParseAndEvaluate(obfuscated, kBw);
            if (!parse_result.has_value()) { continue; }

            auto ast_result = ParseToAst(obfuscated, kBw);
            if (!ast_result.has_value()) { continue; }

            auto folded      = FoldConstantBitwise(std::move(ast_result.value().expr), kBw);
            const auto &sig  = parse_result.value().sig;
            const auto &vars = parse_result.value().vars;

            // Classify
            auto cls = ClassifyStructural(*folded);

            // Only audit linear expressions with BoA flags
            if (cls.semantic != SemanticClass::kLinear) { continue; }
            if (!HasFlag(cls.flags, kSfHasBitwiseOverArith)
                && !HasFlag(cls.flags, kSfHasArithOverBitwise))
            {
                continue;
            }

            PipelineComparison rec;
            rec.line_num                 = line_num;
            rec.sem_class                = cls.semantic;
            rec.flags                    = cls.flags;
            rec.is_exploration_candidate = IsFoldedAstExplorationCandidate(cls.flags);

            // ── Run 1: Full pipeline (normal) ──
            {
                Options opts{ .bitwidth = kBw, .max_vars = 16, .spot_check = true };

                auto t0     = std::chrono::steady_clock::now();
                auto result = Simplify(sig, vars, folded.get(), opts);
                auto t1     = std::chrono::steady_clock::now();

                rec.full_time_ms = std::chrono::duration< double, std::milli >(t1 - t0).count();

                if (result.has_value()
                    && result.value().kind == SimplifyOutcome::Kind::kSimplified)
                {
                    rec.full_simplified = true;
                    rec.full_cost       = ComputeCost(*result.value().expr).cost.weighted_size;
                }
                rec.full_expansions =
                    result.has_value() ? result.value().telemetry.total_expansions : 0;
                rec.full_depth =
                    result.has_value() ? result.value().telemetry.max_depth_reached : 0;
                rec.full_candidates =
                    result.has_value() ? result.value().telemetry.candidates_verified : 0;
                rec.full_queue_hw =
                    result.has_value() ? result.value().telemetry.queue_high_water : 0;
            }

            // Skip cheap expressions
            if (rec.full_time_ms < min_time_ms) { continue; }

            // ── Run 2: Direct algebraic solve (no AST) ──
            // Pass input_expr=nullptr so the orchestrator uses
            // SeedNoAst — pure signature-based solving without any
            // AST classification or exploration passes.
            {
                // Build evaluator from folded AST for FW verification
                auto eval_ptr = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*folded));
                Evaluator eval = [eval_ptr](const std::vector< uint64_t > &v) -> uint64_t {
                    return EvalExpr(**eval_ptr, v, kBw);
                };

                Options opts{
                    .bitwidth   = kBw,
                    .max_vars   = 16,
                    .spot_check = true,
                    .evaluator  = eval,
                };

                auto t0     = std::chrono::steady_clock::now();
                auto result = Simplify(sig, vars, nullptr, opts);
                auto t1     = std::chrono::steady_clock::now();

                rec.direct_time_ms =
                    std::chrono::duration< double, std::milli >(t1 - t0).count();

                if (result.has_value()
                    && result.value().kind == SimplifyOutcome::Kind::kSimplified)
                {
                    rec.direct_simplified = true;
                    rec.direct_cost = ComputeCost(*result.value().expr).cost.weighted_size;

                    // Verify correctness: direct result should produce same
                    // outputs as the original expression at full width
                    auto fw_check = FullWidthCheck(
                        *folded, static_cast< uint32_t >(vars.size()), *result.value().expr, {},
                        kBw, 16
                    );
                    rec.results_match = fw_check.passed;
                }
                rec.direct_expansions =
                    result.has_value() ? result.value().telemetry.total_expansions : 0;
                rec.direct_depth =
                    result.has_value() ? result.value().telemetry.max_depth_reached : 0;
                rec.direct_candidates =
                    result.has_value() ? result.value().telemetry.candidates_verified : 0;
            }

            // Compare
            rec.direct_succeeded = rec.direct_simplified && rec.results_match;
            if (rec.full_simplified && rec.direct_simplified) {
                if (rec.direct_cost == rec.full_cost) {
                    rec.cost_equal = true;
                } else if (rec.direct_cost < rec.full_cost) {
                    rec.cost_better_direct = true;
                } else {
                    rec.cost_worse_direct = true;
                }
            }
            if (rec.direct_time_ms > 0) { rec.speedup = rec.full_time_ms / rec.direct_time_ms; }

            records.push_back(std::move(rec));
        }
    }

    void print_comparison(
        const std::string &title, const std::vector< PipelineComparison > &records
    ) {
        if (records.empty()) {
            std::cerr << "\n  " << title << ": no qualifying expressions\n\n";
            return;
        }

        std::cerr << "\n╔═══════════════════════════════════════════════════════╗\n";
        std::cerr << "║  " << std::left << std::setw(52) << title << "║\n";
        std::cerr << "╚═══════════════════════════════════════════════════════╝\n\n";

        int total              = static_cast< int >(records.size());
        int full_solved        = 0;
        int direct_solved      = 0;
        int direct_correct     = 0;
        int cost_equal         = 0;
        int cost_better        = 0;
        int cost_worse         = 0;
        double total_full_ms   = 0.0;
        double total_direct_ms = 0.0;

        for (const auto &r : records) {
            if (r.full_simplified) { full_solved++; }
            if (r.direct_simplified) { direct_solved++; }
            if (r.direct_succeeded) { direct_correct++; }
            if (r.cost_equal) { cost_equal++; }
            if (r.cost_better_direct) { cost_better++; }
            if (r.cost_worse_direct) { cost_worse++; }
            total_full_ms   += r.full_time_ms;
            total_direct_ms += r.direct_time_ms;
        }

        std::cerr << "  Qualifying expressions: " << total << "\n";
        std::cerr << "  Full pipeline solved:   " << full_solved << "\n";
        std::cerr << "  Direct solve succeeded: " << direct_correct << " / " << direct_solved
                  << " simplified"
                  << " (" << (direct_solved - direct_correct) << " FW mismatch)\n\n";

        std::cerr << "  Cost comparison (where both solved):\n";
        std::cerr << "    Equal:         " << cost_equal << "\n";
        std::cerr << "    Direct better: " << cost_better << "\n";
        std::cerr << "    Direct worse:  " << cost_worse << "\n\n";

        std::cerr << std::fixed << std::setprecision(1);
        std::cerr << "  Time:\n";
        std::cerr << "    Full pipeline total:  " << total_full_ms << "ms\n";
        std::cerr << "    Direct solve total:   " << total_direct_ms << "ms\n";
        std::cerr << "    Savings:              " << (total_full_ms - total_direct_ms) << "ms ("
                  << std::setprecision(0)
                  << ((total_full_ms - total_direct_ms) / total_full_ms * 100) << "%)\n\n";

        // Expansion comparison
        uint64_t total_full_exp   = 0;
        uint64_t total_direct_exp = 0;
        for (const auto &r : records) {
            total_full_exp   += r.full_expansions;
            total_direct_exp += r.direct_expansions;
        }
        std::cerr << std::setprecision(1);
        std::cerr << "  Expansions:\n";
        std::cerr << "    Full pipeline total:  " << total_full_exp << "\n";
        std::cerr << "    Direct solve total:   " << total_direct_exp << "\n";
        std::cerr << "    Reduction:            " << (total_full_exp - total_direct_exp) << " ("
                  << std::setprecision(0)
                  << (static_cast< double >(total_full_exp - total_direct_exp)
                      / static_cast< double >(total_full_exp) * 100)
                  << "%)\n\n";

        // Speedup distribution
        std::vector< double > speedups;
        for (const auto &r : records) {
            if (r.direct_succeeded) { speedups.push_back(r.speedup); }
        }
        if (!speedups.empty()) {
            std::sort(speedups.begin(), speedups.end());
            size_t n = speedups.size();
            std::cerr << std::setprecision(1);
            std::cerr << "  Speedup (full/direct):\n";
            std::cerr << "    min=" << speedups.front() << "x  p50=" << speedups[n / 2]
                      << "x  p90=" << speedups[n * 90 / 100] << "x  max=" << speedups.back()
                      << "x\n\n";
        }

        // Top 10 by time saved
        std::vector< const PipelineComparison * > sorted;
        for (const auto &r : records) { sorted.push_back(&r); }
        std::sort(sorted.begin(), sorted.end(), [](const auto *a, const auto *b) {
            return (a->full_time_ms - a->direct_time_ms)
                > (b->full_time_ms - b->direct_time_ms);
        });

        std::cerr << "  Top 10 by time saved:\n";
        int shown = 0;
        for (const auto *r : sorted) {
            if (shown >= 10) { break; }
            double saved = r->full_time_ms - r->direct_time_ms;
            std::cerr << std::setprecision(1);
            std::cerr << "    L" << r->line_num << "  full=" << r->full_time_ms
                      << "ms  direct=" << r->direct_time_ms << "ms  saved=" << saved
                      << "ms  expns=" << r->full_expansions << "→" << r->direct_expansions
                      << "  cost=" << r->full_cost << "→" << r->direct_cost;
            if (!r->direct_succeeded) { std::cerr << "  [FAIL]"; }
            if (r->cost_worse_direct) { std::cerr << "  [WORSE]"; }
            std::cerr << "\n";
            shown++;
        }
        std::cerr << "\n";

        // Failure analysis
        int fail_count = 0;
        for (const auto &r : records) {
            if (!r.direct_succeeded && r.full_simplified) {
                if (fail_count == 0) { std::cerr << "  Direct solve failures:\n"; }
                if (fail_count >= 10) {
                    std::cerr << "    ... and more\n";
                    break;
                }
                std::cerr << "    L" << r.line_num << "  simplified=" << r.direct_simplified
                          << "  match=" << r.results_match << "  full_cost=" << r.full_cost
                          << "  direct_cost=" << r.direct_cost
                          << "  full_expns=" << r.full_expansions << "\n";
                fail_count++;
            }
        }
        if (fail_count > 0) { std::cerr << "\n"; }
    }

} // namespace

// ── Per-dataset tests ──
// Use min_time_ms=5 to focus on expressions where exploration matters

TEST(LinearAoBPipelineAudit, Syntia) {
    std::vector< PipelineComparison > records;
    run_pipeline_comparison({ "syntia", DATASET_DIR "/gamba/syntia.txt" }, records, 5.0);
    print_comparison("Syntia — Linear AoB (>5ms)", records);
    EXPECT_TRUE(true);
}

TEST(LinearAoBPipelineAudit, QSynthEA) {
    std::vector< PipelineComparison > records;
    run_pipeline_comparison({ "qsynth", DATASET_DIR "/gamba/qsynth_ea.txt" }, records, 5.0);
    print_comparison("QSynth — Linear AoB (>5ms)", records);
    EXPECT_TRUE(true);
}

TEST(LinearAoBPipelineAudit, LokiTiny) {
    std::vector< PipelineComparison > records;
    run_pipeline_comparison({ "loki_tiny", DATASET_DIR "/gamba/loki_tiny.txt" }, records, 3.0);
    print_comparison("LokiTiny — Linear AoB (>3ms)", records);
    EXPECT_TRUE(true);
}

TEST(LinearAoBPipelineAudit, MbaObfLinear) {
    std::vector< PipelineComparison > records;
    run_pipeline_comparison(
        { "mba_obf_linear", DATASET_DIR "/gamba/mba_obf_linear.txt" }, records, 5.0
    );
    print_comparison("MbaObfLinear — Linear AoB (>5ms)", records);
    EXPECT_TRUE(true);
}

TEST(LinearAoBPipelineAudit, NeuReduceSample) {
    // NeuReduce is huge; sample the first 1000 lines
    std::vector< PipelineComparison > records;
    run_pipeline_comparison({ "neureduce", DATASET_DIR "/gamba/neureduce.txt" }, records, 50.0);
    print_comparison("NeuReduce — Linear AoB (>50ms)", records);
    EXPECT_TRUE(true);
}
