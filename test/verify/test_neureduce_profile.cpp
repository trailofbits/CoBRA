/// NeuReduce per-expression profiling.
///
/// Groups expressions by variable count and measures:
///   - time per expression
///   - orchestrator telemetry (expansions, depth, queue HW)
///   - classification and structural flags
///   - whether product-shadow repair fired
///
/// Goal: identify what makes 5-var expressions ~500x slower than
/// 2-var expressions and where solver time is spent.

#include "ExprParser.h"
#include "cobra/core/AnfTransform.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classification.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/CoBExprBuilder.h"
#include "cobra/core/CoeffInterpolator.h"
#include "cobra/core/Evaluator.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/PatternMatcher.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/SignatureSimplifier.h"
#include "cobra/core/Simplifier.h"
#include "cobra/core/SimplifyOutcome.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace cobra;

namespace {

    constexpr uint32_t kBw = 64;

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

    std::string semantic_str(SemanticClass sc) {
        switch (sc) {
            case SemanticClass::kLinear:
                return "linear";
            case SemanticClass::kSemilinear:
                return "semilinear";
            case SemanticClass::kPolynomial:
                return "polynomial";
            case SemanticClass::kNonPolynomial:
                return "non-polynomial";
        }
        return "?";
    }

    std::string flags_short(StructuralFlag flags) {
        std::string s;
        if (HasFlag(flags, kSfHasBitwise)) { s += "B"; }
        if (HasFlag(flags, kSfHasArithmetic)) { s += "A"; }
        if (HasFlag(flags, kSfHasMul)) { s += "M"; }
        if (HasFlag(flags, kSfHasBitwiseOverArith)) { s += "o"; }
        if (HasFlag(flags, kSfHasArithOverBitwise)) { s += "O"; }
        if (HasFlag(flags, kSfHasMixedProduct)) { s += "X"; }
        if (HasFlag(flags, kSfHasMultilinearProduct)) { s += "L"; }
        if (HasFlag(flags, kSfHasSingletonPower)) { s += "P"; }
        return s.empty() ? "-" : s;
    }

    struct ExprProfile
    {
        int line_num         = 0;
        uint32_t nvars       = 0;
        double time_ms       = 0.0;
        uint32_t expns       = 0;
        uint32_t depth       = 0;
        uint32_t queue       = 0;
        uint32_t cands       = 0;
        uint32_t cost        = 0;
        SemanticClass cls    = SemanticClass::kLinear;
        StructuralFlag flags = kSfNone;
        bool exploration     = false;
    };

    struct VarBucket
    {
        uint32_t nvars        = 0;
        int count             = 0;
        double total_ms       = 0.0;
        double max_ms         = 0.0;
        uint64_t total_expns  = 0;
        int exploration_count = 0;
        std::map< SemanticClass, int > by_class;
    };

} // namespace

TEST(NeuReduceProfile, PerVarTimingBreakdown) {
    std::ifstream file(DATASET_DIR "/gamba/neureduce.txt");
    ASSERT_TRUE(file.is_open());

    // Sample up to kMaxPerBucket expressions per variable count
    // to keep runtime manageable (~60s instead of ~2800s).
    constexpr int kMaxPerBucket = 50;
    std::map< uint32_t, int > sampled_count;

    std::vector< ExprProfile > profiles;
    profiles.reserve(1000);
    std::map< uint32_t, VarBucket > buckets;

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

        auto folded = FoldConstantBitwise(std::move(ast_result.value().expr), kBw);
        auto cls    = ClassifyStructural(*folded);
        auto nvars  = static_cast< uint32_t >(parse_result.value().vars.size());

        // Sample limit per variable count
        if (sampled_count[nvars] >= kMaxPerBucket) { continue; }
        sampled_count[nvars]++;

        Options opts{ .bitwidth = kBw, .max_vars = 16, .spot_check = true };

        auto t0 = std::chrono::steady_clock::now();
        auto result =
            Simplify(parse_result.value().sig, parse_result.value().vars, folded.get(), opts);
        auto t1 = std::chrono::steady_clock::now();

        ExprProfile p;
        p.line_num    = line_num;
        p.nvars       = nvars;
        p.time_ms     = std::chrono::duration< double, std::milli >(t1 - t0).count();
        p.cls         = cls.semantic;
        p.flags       = cls.flags;
        p.exploration = IsFoldedAstExplorationCandidate(cls.flags);

        if (result.has_value()) {
            p.expns = result->telemetry.total_expansions;
            p.depth = result->telemetry.max_depth_reached;
            p.queue = result->telemetry.queue_high_water;
            p.cands = result->telemetry.candidates_verified;
            if (result->kind == SimplifyOutcome::Kind::kSimplified) {
                p.cost = ComputeCost(*result->expr).cost.weighted_size;
            }
        }

        profiles.push_back(p);

        auto &b = buckets[nvars];
        b.nvars = nvars;
        b.count++;
        b.total_ms    += p.time_ms;
        b.max_ms       = std::max(b.max_ms, p.time_ms);
        b.total_expns += p.expns;
        if (p.exploration) { b.exploration_count++; }
        b.by_class[p.cls]++;
    }

    // ── Per-variable-count summary ──
    std::cerr << "\n╔════════════════════════════════════════════╗\n";
    std::cerr << "║  NeuReduce Per-Variable-Count Breakdown    ║\n";
    std::cerr << "╚════════════════════════════════════════════╝\n\n";

    double grand_total = 0.0;
    for (const auto &[nv, b] : buckets) { grand_total += b.total_ms; }

    std::cerr << std::fixed << std::setprecision(1);
    std::cerr << "  " << std::left << std::setw(6) << "vars" << std::right << std::setw(7)
              << "count" << std::setw(12) << "total_ms" << std::setw(8) << "pct"
              << std::setw(10) << "avg_ms" << std::setw(10) << "max_ms" << std::setw(10)
              << "avg_exp" << std::setw(8) << "explor"
              << "  class_dist\n";
    std::cerr << "  " << std::string(80, '-') << "\n";

    for (const auto &[nv, b] : buckets) {
        double avg_ms  = b.total_ms / b.count;
        double avg_exp = static_cast< double >(b.total_expns) / b.count;
        double pct     = b.total_ms / grand_total * 100;
        std::cerr << "  " << std::left << std::setw(6) << nv << std::right << std::setw(7)
                  << b.count << std::setw(12) << b.total_ms << std::setw(7) << pct << "%"
                  << std::setw(10) << avg_ms << std::setw(10) << b.max_ms << std::setw(10)
                  << avg_exp << std::setw(8) << b.exploration_count << "  ";
        for (const auto &[c, cnt] : b.by_class) {
            std::cerr << semantic_str(c) << "=" << cnt << " ";
        }
        std::cerr << "\n";
    }
    std::cerr << "\n  Grand total: " << grand_total << "ms\n\n";

    // ── Top 20 slowest expressions ──
    std::vector< const ExprProfile * > sorted;
    for (const auto &p : profiles) { sorted.push_back(&p); }
    std::sort(sorted.begin(), sorted.end(), [](const auto *a, const auto *b) {
        return a->time_ms > b->time_ms;
    });

    std::cerr << "  Top 20 slowest:\n";
    for (int i = 0; i < 20 && i < static_cast< int >(sorted.size()); ++i) {
        const auto *p = sorted[i];
        std::cerr << "    L" << p->line_num << "  " << p->nvars << "v"
                  << "  " << std::setw(8) << p->time_ms << "ms"
                  << "  exp=" << std::setw(5) << p->expns << "  d=" << std::setw(3) << p->depth
                  << "  q=" << std::setw(4) << p->queue << "  c=" << p->cands << "  "
                  << semantic_str(p->cls) << "  " << flags_short(p->flags)
                  << (p->exploration ? " [EXPL]" : "") << "\n";
    }
    std::cerr << "\n";

    // ── Expansion distribution for 5-var expressions ──
    std::vector< uint32_t > exp5;
    for (const auto &p : profiles) {
        if (p.nvars == 5) { exp5.push_back(p.expns); }
    }
    if (!exp5.empty()) {
        std::sort(exp5.begin(), exp5.end());
        size_t n = exp5.size();
        std::cerr << "  5-var expansion distribution (n=" << n << "):\n";
        std::cerr << "    min=" << exp5.front() << "  p50=" << exp5[n / 2]
                  << "  p90=" << exp5[n * 90 / 100] << "  p95=" << exp5[n * 95 / 100]
                  << "  max=" << exp5.back() << "\n\n";
    }

    // ── Expansion distribution for 4-var expressions ──
    std::vector< uint32_t > exp4;
    for (const auto &p : profiles) {
        if (p.nvars == 4) { exp4.push_back(p.expns); }
    }
    if (!exp4.empty()) {
        std::sort(exp4.begin(), exp4.end());
        size_t n = exp4.size();
        std::cerr << "  4-var expansion distribution (n=" << n << "):\n";
        std::cerr << "    min=" << exp4.front() << "  p50=" << exp4[n / 2]
                  << "  p90=" << exp4[n * 90 / 100] << "  p95=" << exp4[n * 95 / 100]
                  << "  max=" << exp4.back() << "\n\n";
    }

    EXPECT_FALSE(profiles.empty());
}

// Drill into a single 5-var expression: compare normal vs no-evaluator
// to isolate how much time is spent in full-width verification.
TEST(NeuReduceProfile, FiveVarEvaluatorCostIsolation) {
    std::ifstream file(DATASET_DIR "/gamba/neureduce.txt");
    ASSERT_TRUE(file.is_open());

    // Collect first 10 five-var expressions
    struct Sample
    {
        int line_num;
        std::string obfuscated;
        std::vector< uint64_t > sig;
        std::vector< std::string > vars;
        std::unique_ptr< Expr > folded;
    };

    std::vector< Sample > samples;

    std::string line;
    int line_num = 0;
    while (std::getline(file, line) && samples.size() < 10) {
        ++line_num;
        if (line.empty()) { continue; }
        size_t sep = find_separator(line);
        if (sep == std::string::npos) { continue; }
        std::string obfuscated = trim(line.substr(0, sep));
        if (obfuscated.empty()) { continue; }

        auto parse = ParseAndEvaluate(obfuscated, kBw);
        if (!parse.has_value()) { continue; }
        if (parse->vars.size() != 5) { continue; }

        auto ast = ParseToAst(obfuscated, kBw);
        if (!ast.has_value()) { continue; }

        samples.push_back(
            {
                .line_num   = line_num,
                .obfuscated = obfuscated,
                .sig        = parse->sig,
                .vars       = parse->vars,
                .folded     = FoldConstantBitwise(std::move(ast->expr), kBw),
            }
        );
    }

    std::cerr << "\n╔════════════════════════════════════════════╗\n";
    std::cerr << "║  5-Var Evaluator Cost Isolation            ║\n";
    std::cerr << "╚════════════════════════════════════════════╝\n\n";

    double total_with_eval = 0.0;
    double total_no_eval   = 0.0;
    double total_no_ast    = 0.0;

    for (auto &s : samples) {
        // Run 1: Normal (with AST + evaluator)
        double t_normal       = 0.0;
        uint32_t expns_normal = 0;
        {
            Options opts{ .bitwidth = kBw, .max_vars = 16, .spot_check = true };
            auto t0  = std::chrono::steady_clock::now();
            auto r   = Simplify(s.sig, s.vars, s.folded.get(), opts);
            auto t1  = std::chrono::steady_clock::now();
            t_normal = std::chrono::duration< double, std::milli >(t1 - t0).count();
            if (r.has_value()) { expns_normal = r->telemetry.total_expansions; }
        }

        // Run 2: No evaluator (skip all FW checks)
        double t_no_eval       = 0.0;
        uint32_t expns_no_eval = 0;
        {
            Options opts{
                .bitwidth   = kBw,
                .max_vars   = 16,
                .spot_check = false,
            };
            auto t0   = std::chrono::steady_clock::now();
            auto r    = Simplify(s.sig, s.vars, nullptr, opts);
            auto t1   = std::chrono::steady_clock::now();
            t_no_eval = std::chrono::duration< double, std::milli >(t1 - t0).count();
            if (r.has_value()) { expns_no_eval = r->telemetry.total_expansions; }
        }

        // Run 3: Signature-only (no AST, with evaluator)
        double t_no_ast       = 0.0;
        uint32_t expns_no_ast = 0;
        {
            auto eval_ptr  = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*s.folded));
            Evaluator eval = [eval_ptr](const std::vector< uint64_t > &v) -> uint64_t {
                return EvalExpr(**eval_ptr, v, kBw);
            };
            Options opts{
                .bitwidth   = kBw,
                .max_vars   = 16,
                .spot_check = true,
                .evaluator  = eval,
            };
            auto t0  = std::chrono::steady_clock::now();
            auto r   = Simplify(s.sig, s.vars, nullptr, opts);
            auto t1  = std::chrono::steady_clock::now();
            t_no_ast = std::chrono::duration< double, std::milli >(t1 - t0).count();
            if (r.has_value()) { expns_no_ast = r->telemetry.total_expansions; }
        }

        total_with_eval += t_normal;
        total_no_eval   += t_no_eval;
        total_no_ast    += t_no_ast;

        std::cerr << std::fixed << std::setprecision(1);
        std::cerr << "  L" << s.line_num << "  normal=" << t_normal << "ms/" << expns_normal
                  << "  no_eval=" << t_no_eval << "ms/" << expns_no_eval
                  << "  no_ast=" << t_no_ast << "ms/" << expns_no_ast << "\n";
    }

    std::cerr << std::fixed << std::setprecision(1);
    std::cerr << "\n  Totals (10 exprs):\n";
    std::cerr << "    normal:  " << total_with_eval << "ms\n";
    std::cerr << "    no_eval: " << total_no_eval << "ms (" << std::setprecision(0)
              << (1.0 - total_no_eval / total_with_eval) * 100 << "% of time is evaluator)\n";
    std::cerr << "    no_ast:  " << total_no_ast << "ms (" << std::setprecision(0)
              << (1.0 - total_no_ast / total_with_eval) * 100
              << "% of time is AST/exploration)\n\n";

    EXPECT_FALSE(samples.empty());
}

// Count evaluator invocations to understand FW check frequency.
TEST(NeuReduceProfile, EvaluatorCallCount) {
    std::ifstream file(DATASET_DIR "/gamba/neureduce.txt");
    ASSERT_TRUE(file.is_open());

    // Find first 5-var expression
    std::string line;
    int line_num = 0;
    std::string obfuscated;
    std::vector< uint64_t > sig;
    std::vector< std::string > vars;
    std::unique_ptr< Expr > folded;

    while (std::getline(file, line)) {
        ++line_num;
        if (line.empty()) { continue; }
        size_t sep = find_separator(line);
        if (sep == std::string::npos) { continue; }
        obfuscated = trim(line.substr(0, sep));
        if (obfuscated.empty()) { continue; }

        auto parse = ParseAndEvaluate(obfuscated, kBw);
        if (!parse.has_value() || parse->vars.size() != 5) { continue; }

        auto ast = ParseToAst(obfuscated, kBw);
        if (!ast.has_value()) { continue; }

        sig    = parse->sig;
        vars   = parse->vars;
        folded = FoldConstantBitwise(std::move(ast->expr), kBw);
        break;
    }
    ASSERT_NE(folded, nullptr);

    // Run with lambda evaluator (slow tree-walk)
    uint64_t eval_count_lambda = 0;
    {
        auto counting_eval = [&eval_count_lambda,
                              &folded](const std::vector< uint64_t > &v) -> uint64_t {
            eval_count_lambda++;
            return EvalExpr(*folded, v, kBw);
        };

        Options opts{
            .bitwidth   = kBw,
            .max_vars   = 16,
            .spot_check = true,
            .evaluator  = Evaluator(counting_eval),
        };

        auto t0 = std::chrono::steady_clock::now();
        auto r  = Simplify(sig, vars, nullptr, opts);
        auto t1 = std::chrono::steady_clock::now();

        double ms   = std::chrono::duration< double, std::milli >(t1 - t0).count();
        uint32_t ex = r.has_value() ? r->telemetry.total_expansions : 0;

        std::cerr << "\n  L" << line_num << " (5v, lambda eval):\n";
        std::cerr << "    time:       " << std::fixed << std::setprecision(1) << ms << "ms\n";
        std::cerr << "    expansions: " << ex << "\n";
        std::cerr << "    eval calls: " << eval_count_lambda << "\n";
        std::cerr << "    ms/eval:    " << std::setprecision(4)
                  << (ms / static_cast< double >(eval_count_lambda)) << "\n";
    }

    // Run with compiled evaluator (fast bytecode)
    {
        auto compiled_eval = Evaluator::FromExpr(*folded, kBw);
        Options opts{
            .bitwidth   = kBw,
            .max_vars   = 16,
            .spot_check = true,
            .evaluator  = compiled_eval,
        };

        auto t0 = std::chrono::steady_clock::now();
        auto r  = Simplify(sig, vars, nullptr, opts);
        auto t1 = std::chrono::steady_clock::now();

        double ms   = std::chrono::duration< double, std::milli >(t1 - t0).count();
        uint32_t ex = r.has_value() ? r->telemetry.total_expansions : 0;

        std::cerr << "\n  L" << line_num << " (5v, compiled eval):\n";
        std::cerr << "    time:       " << std::fixed << std::setprecision(1) << ms << "ms\n";
        std::cerr << "    expansions: " << ex << "\n";
        std::cerr << "    ms/call:    " << std::setprecision(4)
                  << (ms / static_cast< double >(eval_count_lambda)) << "\n";
    }

    // Run with AST (normal pipeline, auto-compiled)
    {
        Options opts{ .bitwidth = kBw, .max_vars = 16, .spot_check = true };

        auto t0 = std::chrono::steady_clock::now();
        auto r  = Simplify(sig, vars, folded.get(), opts);
        auto t1 = std::chrono::steady_clock::now();

        double ms   = std::chrono::duration< double, std::milli >(t1 - t0).count();
        uint32_t ex = r.has_value() ? r->telemetry.total_expansions : 0;

        std::cerr << "\n  L" << line_num << " (5v, normal with AST):\n";
        std::cerr << "    time:       " << std::fixed << std::setprecision(1) << ms << "ms\n";
        std::cerr << "    expansions: " << ex << "\n";
        std::cerr << "    ms/call:    " << std::setprecision(4)
                  << (ms / static_cast< double >(eval_count_lambda)) << "\n";
    }

    std::cerr << "\n    total evals: " << eval_count_lambda << "\n\n";
    EXPECT_GT(eval_count_lambda, 0U);
}

// Trace a single 5-var NeuReduce expression through each pipeline
// stage to find where and why FW check fails.
TEST(NeuReduceProfile, SingleExpressionTrace) {
    // NeuReduce line 7 (5-var, linear, ~2.5s):
    // 3*(~x^y&~z)+(x|y|~z)-(x&(y^~z))-2*(x^y^z)+(x&~y&z)+2*(x^y|z)-4*(~x)-2*(x&y^z)-(~(a|t))-1
    // Ground truth: -3*(~x&(y^z))+2*(x&(y^z))+(a|t)
    const char *expr_str = "3*(~x^y&~z)+(x|y|~z)-(x&(y^~z))-2*(x^y^z)+(x&~y&z)+2*(x^y|z)"
                           "-4*(~x)-2*(x&y^z)-(~(a|t))-1";

    auto parse = ParseAndEvaluate(expr_str, kBw);
    ASSERT_TRUE(parse.has_value());
    auto ast = ParseToAst(expr_str, kBw);
    ASSERT_TRUE(ast.has_value());

    auto folded      = FoldConstantBitwise(std::move(ast->expr), kBw);
    const auto &sig  = parse->sig;
    const auto &vars = parse->vars;
    auto num_vars    = static_cast< uint32_t >(vars.size());

    auto cls  = ClassifyStructural(*folded);
    auto eval = Evaluator::FromExpr(*folded, kBw);

    std::cerr << "\n╔════════════════════════════════════════════╗\n";
    std::cerr << "║  Single 5-Var Expression Trace             ║\n";
    std::cerr << "╚════════════════════════════════════════════╝\n\n";

    std::cerr << "  vars: ";
    for (const auto &v : vars) { std::cerr << v << " "; }
    std::cerr << "\n  num_vars: " << num_vars << "\n  sig_len: " << sig.size()
              << "\n  class: " << semantic_str(cls.semantic)
              << "\n  boolean_valued: " << IsBooleanValued(sig) << "\n\n";

    // Step 1: AuxVar elimination
    auto elim = EliminateAuxVars(sig, vars);
    std::cerr << "  AuxVar elimination:\n";
    std::cerr << "    real_vars: ";
    for (const auto &v : elim.real_vars) { std::cerr << v << " "; }
    std::cerr << "\n    spurious: ";
    for (const auto &v : elim.spurious_vars) { std::cerr << v << " "; }
    std::cerr << "\n    reduced_sig_len: " << elim.reduced_sig.size() << "\n\n";

    // Enhanced elimination with evaluator
    auto fw_elim = EliminateAuxVars(sig, vars, eval, kBw);
    std::cerr << "  AuxVar elimination (enhanced):\n";
    std::cerr << "    real_vars: ";
    for (const auto &v : fw_elim.real_vars) { std::cerr << v << " "; }
    std::cerr << "\n    spurious: ";
    for (const auto &v : fw_elim.spurious_vars) { std::cerr << v << " "; }
    std::cerr << "\n\n";

    // Use reduced sig for remaining steps
    const auto &reduced_sig = elim.reduced_sig;
    auto reduced_nvars      = static_cast< uint32_t >(elim.real_vars.size());

    // Step 2: Pattern match
    auto pm = MatchPattern(reduced_sig, reduced_nvars, kBw);
    std::cerr << "  Pattern match: " << (pm ? "HIT" : "miss") << "\n";
    if (pm) {
        std::cerr << "    result: " << Render(**pm, elim.real_vars) << "\n";
        auto pm_fw = FullWidthCheckEval(eval, num_vars, **pm, kBw);
        std::cerr << "    FW check (full vars): " << pm_fw.passed << "\n";

        // Remap and check
        auto support  = BuildVarSupport(vars, elim.real_vars);
        auto remapped = CloneExpr(**pm);
        RemapVarIndices(*remapped, support);
        auto pm_fw2 = FullWidthCheckEval(eval, num_vars, *remapped, kBw);
        std::cerr << "    FW check (remapped): " << pm_fw2.passed << "\n";
    }
    std::cerr << "\n";

    // Step 3: ANF
    auto anf      = ComputeAnf(reduced_sig, reduced_nvars);
    auto anf_expr = BuildAnfExpr(anf, reduced_nvars);
    std::cerr << "  ANF: " << Render(*anf_expr, elim.real_vars) << "\n";
    {
        auto support  = BuildVarSupport(vars, elim.real_vars);
        auto remapped = CloneExpr(*anf_expr);
        RemapVarIndices(*remapped, support);
        auto anf_fw = FullWidthCheckEval(eval, num_vars, *remapped, kBw);
        std::cerr << "  ANF FW check: " << anf_fw.passed << "\n";
    }

    // Step 4: CoB coefficients
    auto coeffs = InterpolateCoefficients(reduced_sig, reduced_nvars, kBw);
    std::cerr << "\n  CoB coefficients (" << coeffs.size() << "):\n    ";
    for (size_t i = 0; i < coeffs.size(); ++i) {
        if (coeffs[i] != 0) {
            std::cerr << "[" << i << "]=" << static_cast< int64_t >(coeffs[i]) << " ";
        }
    }
    std::cerr << "\n";

    // Step 5: Build CoB expression
    auto cob_expr = BuildCobExpr(coeffs, reduced_nvars, kBw);
    if (cob_expr) {
        std::cerr << "  CoB expr: " << Render(*cob_expr, elim.real_vars) << "\n";
        std::cerr << "  CoB cost: " << ComputeCost(*cob_expr).cost.weighted_size << "\n";

        // Sig check
        auto sig_check = SignatureCheck(reduced_sig, *cob_expr, reduced_nvars, kBw);
        std::cerr << "  CoB sig check: " << sig_check.passed << "\n";

        // FW check on reduced vars (wrong — uses num_vars = reduced_nvars)
        auto cob_fw_reduced = FullWidthCheckEval(eval, reduced_nvars, *cob_expr, kBw);
        std::cerr << "  CoB FW check (reduced nvars=" << reduced_nvars
                  << "): " << cob_fw_reduced.passed << "\n";

        // FW check with remapping (correct)
        auto support  = BuildVarSupport(vars, elim.real_vars);
        auto remapped = CloneExpr(*cob_expr);
        RemapVarIndices(*remapped, support);
        auto cob_fw_full = FullWidthCheckEval(eval, num_vars, *remapped, kBw);
        std::cerr << "  CoB FW check (remapped nvars=" << num_vars
                  << "): " << cob_fw_full.passed << "\n";

        if (!cob_fw_full.passed && !cob_fw_full.failing_input.empty()) {
            std::cerr << "  Failing input: ";
            for (size_t i = 0; i < cob_fw_full.failing_input.size(); ++i) {
                std::cerr << vars[i] << "=" << cob_fw_full.failing_input[i] << " ";
            }
            auto orig_val = eval(cob_fw_full.failing_input);
            auto cob_val  = EvalExpr(*remapped, cob_fw_full.failing_input, kBw);
            std::cerr << "\n  original=" << static_cast< int64_t >(orig_val)
                      << "  cob=" << static_cast< int64_t >(cob_val) << "\n";
        }
    }
    std::cerr << "\n";

    EXPECT_TRUE(true);
}

// Instrument FullWidthCheckEval via a wrapper evaluator that hashes
// candidates before/after to measure dedup opportunity.
TEST(NeuReduceProfile, VerificationCacheAudit) {
    std::ifstream file(DATASET_DIR "/gamba/neureduce.txt");
    ASSERT_TRUE(file.is_open());

    // Collect first 5 expressions of each var count (3,4,5)
    struct Sample
    {
        int line_num;
        uint32_t nvars;
        std::vector< uint64_t > sig;
        std::vector< std::string > vars;
        std::unique_ptr< Expr > folded;
    };

    std::vector< Sample > samples;
    std::map< uint32_t, int > per_var;

    std::string line;
    int line_num = 0;
    while (std::getline(file, line)) {
        ++line_num;
        if (line.empty()) { continue; }
        size_t sep = find_separator(line);
        if (sep == std::string::npos) { continue; }
        std::string obfuscated = trim(line.substr(0, sep));
        if (obfuscated.empty()) { continue; }

        auto parse = ParseAndEvaluate(obfuscated, kBw);
        if (!parse.has_value()) { continue; }
        auto nv = static_cast< uint32_t >(parse->vars.size());
        if (nv < 3 || nv > 5) { continue; }
        if (per_var[nv] >= 5) { continue; }
        per_var[nv]++;

        auto ast = ParseToAst(obfuscated, kBw);
        if (!ast.has_value()) { continue; }

        samples.push_back(
            {
                .line_num = line_num,
                .nvars    = nv,
                .sig      = parse->sig,
                .vars     = parse->vars,
                .folded   = FoldConstantBitwise(std::move(ast->expr), kBw),
            }
        );

        bool all_done = true;
        for (uint32_t v = 3; v <= 5; ++v) {
            if (per_var[v] < 5) { all_done = false; }
        }
        if (all_done) { break; }
    }

    std::cerr << "\n╔════════════════════════════════════════════╗\n";
    std::cerr << "║  Verification Cache Audit                  ║\n";
    std::cerr << "╚════════════════════════════════════════════╝\n\n";

    // For each sample, run with a counting evaluator that tracks
    // unique probe-result signatures to estimate cache hit rate.
    // We hash the first 4 probe responses to fingerprint each
    // FullWidthCheckEval call site.
    for (auto &s : samples) {
        // Track evaluation patterns: groups of 8 consecutive calls
        // form one FW check. Record original-side results per group.
        uint64_t call_count = 0;
        std::vector< uint64_t > all_original_results;

        auto counting_eval = [&call_count, &all_original_results,
                              &s](const std::vector< uint64_t > &v) -> uint64_t {
            call_count++;
            uint64_t val = EvalExpr(*s.folded, v, kBw);
            all_original_results.push_back(val);
            return val;
        };

        Options opts{
            .bitwidth   = kBw,
            .max_vars   = 16,
            .spot_check = true,
            .evaluator  = Evaluator(counting_eval),
        };

        auto t0 = std::chrono::steady_clock::now();
        auto r  = Simplify(s.sig, s.vars, nullptr, opts);
        auto t1 = std::chrono::steady_clock::now();

        double ms   = std::chrono::duration< double, std::milli >(t1 - t0).count();
        uint32_t ex = r.has_value() ? r->telemetry.total_expansions : 0;

        // Estimate FW check count: calls / 8 (default num_samples)
        // Some calls use 64 samples (spot_check), most use 8.
        uint64_t est_fw_checks = call_count / 8;

        // Group calls into FW-check-sized blocks and hash results
        // to find duplicate verification patterns.
        // Each FW check evaluates original at the same random probes.
        // If two checks produce the same original-side result sequence,
        // they are on the same expression (though the candidate differs).
        //
        // Better metric: count how many candidate-side verifications
        // produce the same pass/fail outcome at the same probes.
        // But since we only have the original side here, just count
        // unique probe-result sequences.

        std::cerr << std::fixed << std::setprecision(1);
        std::cerr << "  L" << s.line_num << " (" << s.nvars << "v):"
                  << "  time=" << ms << "ms"
                  << "  expns=" << ex << "  eval_calls=" << call_count
                  << "  est_fw_checks=" << est_fw_checks
                  << "  calls/expn=" << std::setprecision(0)
                  << (ex > 0 ? static_cast< double >(call_count) / ex : 0) << "\n";
    }

    std::cerr << "\n";
    EXPECT_FALSE(samples.empty());
}
