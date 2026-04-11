// Pipeline-level signature evaluation profiling.
//
// Runs the full Simplify() pipeline on dataset expressions with
// COBRA_SIG_STATS counters enabled, reporting per-expression and
// aggregate statistics on signature evaluation frequency and cost.
//
// Usage: ./bench_signature_pipeline (manual, not in CI)
// Requires: configuring with -DCOBRA_ENABLE_SIG_STATS=ON

#ifndef COBRA_SIG_STATS
    #error "bench_signature_pipeline requires configuring with -DCOBRA_ENABLE_SIG_STATS=ON"
#endif

#include "ExprParser.h"
#include "cobra/core/Expr.h"
#include "cobra/core/SignatureEvalStats.h"
#include "cobra/core/Simplifier.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace cobra;
using Clock = std::chrono::high_resolution_clock;

namespace {

    struct ExprRecord
    {
        std::string raw;
        uint32_t line = 0;
    };

    std::vector< ExprRecord > LoadDataset(const std::string &path, uint32_t max_lines = 0) {
        std::ifstream in(path);
        EXPECT_TRUE(in.good()) << "Cannot open " << path;
        std::vector< ExprRecord > records;
        std::string line;
        uint32_t lineno = 0;
        while (std::getline(in, line)) {
            lineno++;
            if (line.empty() || line[0] == '#') { continue; }
            // CSV: first column is the expression
            auto sep = line.find(',');
            if (sep == std::string::npos) { sep = line.find('\t'); }
            std::string expr_str = (sep != std::string::npos) ? line.substr(0, sep) : line;
            records.push_back(ExprRecord{ .raw = expr_str, .line = lineno });
            if (max_lines > 0 && records.size() >= max_lines) { break; }
        }
        return records;
    }

    struct PerExprStats
    {
        uint32_t line           = 0;
        uint64_t sig_calls      = 0;
        uint64_t sig_expr_calls = 0;
        uint64_t sig_eval_calls = 0;
        uint64_t sig_points     = 0;
        uint64_t sig_nodes      = 0;
        double sig_us           = 0;
        double total_us         = 0;
        bool simplified         = false;
    };

    void PrintAggregateReport(
        const std::string &dataset_name, const std::vector< PerExprStats > &stats
    ) {
        uint64_t total_calls      = 0;
        uint64_t total_expr_calls = 0;
        uint64_t total_eval_calls = 0;
        uint64_t total_points     = 0;
        uint64_t total_nodes      = 0;
        double total_sig_us       = 0;
        double total_pipeline_us  = 0;
        uint32_t simplified       = 0;

        std::vector< uint64_t > calls_per_expr;
        std::vector< double > sig_frac_per_expr;

        for (const auto &s : stats) {
            total_calls       += s.sig_calls;
            total_expr_calls  += s.sig_expr_calls;
            total_eval_calls  += s.sig_eval_calls;
            total_points      += s.sig_points;
            total_nodes       += s.sig_nodes;
            total_sig_us      += s.sig_us;
            total_pipeline_us += s.total_us;
            if (s.simplified) { simplified++; }

            calls_per_expr.push_back(s.sig_calls);
            if (s.total_us > 0) { sig_frac_per_expr.push_back(s.sig_us / s.total_us); }
        }

        std::sort(calls_per_expr.begin(), calls_per_expr.end());
        std::sort(sig_frac_per_expr.begin(), sig_frac_per_expr.end());

        auto pct = [](const auto &v, double p) -> auto {
            auto idx = static_cast< size_t >(static_cast< double >(v.size()) * p);
            if (idx >= v.size()) { idx = v.size() - 1; }
            return v[idx];
        };

        std::cout << "\n=== " << dataset_name << " ===\n";
        std::cout << "expressions: " << stats.size() << " (simplified: " << simplified << ")\n";
        std::cout << "\nSignature evaluation totals:\n";
        std::cout << "  calls:       " << total_calls << " (expr: " << total_expr_calls
                  << ", eval: " << total_eval_calls << ")\n";
        std::cout << "  points:      " << total_points << "\n";
        std::cout << "  nodes:       " << total_nodes << "\n";
        std::cout << std::fixed << std::setprecision(1);
        std::cout << "  sig time:    " << total_sig_us << " us ("
                  << (total_sig_us / total_pipeline_us * 100.0) << "% of pipeline)\n";
        std::cout << "  pipeline:    " << total_pipeline_us << " us\n";

        std::cout << "\nPer-expression sig calls distribution:\n";
        std::cout << "  min:    " << calls_per_expr.front() << "\n";
        std::cout << "  p50:    " << pct(calls_per_expr, 0.50) << "\n";
        std::cout << "  p90:    " << pct(calls_per_expr, 0.90) << "\n";
        std::cout << "  p99:    " << pct(calls_per_expr, 0.99) << "\n";
        std::cout << "  max:    " << calls_per_expr.back() << "\n";
        std::cout << "  mean:   " << std::setprecision(1)
                  << static_cast< double >(total_calls) / static_cast< double >(stats.size())
                  << "\n";

        if (!sig_frac_per_expr.empty()) {
            std::cout << "\nPer-expression sig time fraction:\n";
            std::cout << std::setprecision(1);
            std::cout << "  p50:    " << (pct(sig_frac_per_expr, 0.50) * 100.0) << "%\n";
            std::cout << "  p90:    " << (pct(sig_frac_per_expr, 0.90) * 100.0) << "%\n";
            std::cout << "  p99:    " << (pct(sig_frac_per_expr, 0.99) * 100.0) << "%\n";
            std::cout << "  max:    " << (sig_frac_per_expr.back() * 100.0) << "%\n";
        }

        // Top 10 by sig calls
        std::vector< size_t > indices(stats.size());
        std::iota(indices.begin(), indices.end(), 0);
        std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
            return stats[a].sig_calls > stats[b].sig_calls;
        });

        std::cout << "\nTop 10 by sig calls:\n";
        std::cout << std::left << std::setw(8) << "line" << std::setw(10) << "calls"
                  << std::setw(10) << "expr" << std::setw(10) << "eval" << std::setw(12)
                  << "sig_us" << std::setw(12) << "total_us" << std::setw(10) << "sig%"
                  << std::setw(8) << "ok?"
                  << "\n";
        std::cout << std::string(80, '-') << "\n";
        for (size_t rank = 0; rank < 10 && rank < indices.size(); ++rank) {
            const auto &s = stats[indices[rank]];
            double frac   = s.total_us > 0 ? (s.sig_us / s.total_us * 100.0) : 0;
            std::cout << std::left << std::setw(8) << s.line << std::setw(10) << s.sig_calls
                      << std::setw(10) << s.sig_expr_calls << std::setw(10) << s.sig_eval_calls
                      << std::fixed << std::setprecision(0) << std::setw(12) << s.sig_us
                      << std::setw(12) << s.total_us << std::setprecision(1) << std::setw(10)
                      << frac << std::setw(8) << (s.simplified ? "yes" : "NO") << "\n";
        }
    }

    void RunDatasetProfile(
        const std::string &dataset_name, const std::string &path, uint32_t max_lines = 0
    ) {
        auto records = LoadDataset(path, max_lines);
        std::vector< PerExprStats > all_stats;
        all_stats.reserve(records.size());

        for (const auto &rec : records) {
            auto parse_result = ParseAndEvaluate(rec.raw, 64);
            if (!parse_result.has_value()) { continue; }

            auto ast_result        = ParseToAst(rec.raw, 64);
            const Expr *input_expr = ast_result.has_value() ? ast_result->expr.get() : nullptr;

            Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };

            SigStatsReset();
            auto t0     = Clock::now();
            auto result = Simplify(parse_result->sig, parse_result->vars, input_expr, opts);
            auto t1     = Clock::now();

            auto snap = SigStatsSnapshot();

            PerExprStats ps;
            ps.line           = rec.line;
            ps.sig_calls      = snap.calls;
            ps.sig_expr_calls = snap.expr_calls;
            ps.sig_eval_calls = snap.eval_calls;
            ps.sig_points     = snap.total_points;
            ps.sig_nodes      = snap.total_nodes;
            ps.sig_us         = snap.total_us;
            ps.total_us       = std::chrono::duration< double, std::micro >(t1 - t0).count();
            ps.simplified =
                result.has_value() && (result->kind == SimplifyOutcome::Kind::kSimplified);
            all_stats.push_back(ps);
        }

        PrintAggregateReport(dataset_name, all_stats);
    }

} // namespace

TEST(SignaturePipelineBench, MSiMBA) { RunDatasetProfile("MSiMBA", DATASET_DIR "/msimba.txt"); }

TEST(SignaturePipelineBench, QSynthEA) {
    RunDatasetProfile("QSynth EA", DATASET_DIR "/gamba/qsynth_ea.txt");
}

TEST(SignaturePipelineBench, Syntia) {
    RunDatasetProfile("Syntia", DATASET_DIR "/gamba/syntia.txt");
}
