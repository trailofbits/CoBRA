// Signature evaluation performance harness.
//
// Measures wall-clock cost of EvaluateBooleanSignature across a
// parameter grid: variable count × node count × evaluation mode.
// Output is a structured table suitable for spreadsheet import.
//
// Usage: ./bench_signature_eval (manual, not in CI)

#include "cobra/core/CompiledExpr.h"
#include "cobra/core/Evaluator.h"
#include "cobra/core/Expr.h"
#include "cobra/core/SignatureEval.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace cobra;
using Clock = std::chrono::high_resolution_clock;

namespace {

    // ---------------------------------------------------------------
    // Expression generators
    // ---------------------------------------------------------------

    using ExprPtr = std::unique_ptr< Expr >;

    ExprPtr RandomLeaf(uint32_t num_vars, std::mt19937 &rng) {
        std::uniform_int_distribution< uint32_t > coin(0, 3);
        if (coin(rng) == 0) {
            std::uniform_int_distribution< uint64_t > val(0, 255);
            return Expr::Constant(val(rng));
        }
        std::uniform_int_distribution< uint32_t > var(0, num_vars - 1);
        return Expr::Variable(var(rng));
    }

    ExprPtr RandomBinaryOp(ExprPtr lhs, ExprPtr rhs, std::mt19937 &rng) {
        std::uniform_int_distribution< int > op(0, 4);
        switch (op(rng)) {
            case 0:
                return Expr::Add(std::move(lhs), std::move(rhs));
            case 1:
                return Expr::Mul(std::move(lhs), std::move(rhs));
            case 2:
                return Expr::BitwiseAnd(std::move(lhs), std::move(rhs));
            case 3:
                return Expr::BitwiseOr(std::move(lhs), std::move(rhs));
            default:
                return Expr::BitwiseXor(std::move(lhs), std::move(rhs));
        }
    }

    ExprPtr RandomUnaryOp(ExprPtr child, std::mt19937 &rng) {
        std::uniform_int_distribution< int > op(0, 1);
        if (op(rng) == 0) { return Expr::BitwiseNot(std::move(child)); }
        return Expr::Negate(std::move(child));
    }

    // Build a random MBA expression with approximately `target_nodes`
    // nodes using `num_vars` variables. Grows bottom-up: at each step
    // picks two existing subtrees and combines them, or applies a
    // unary op.
    ExprPtr GenerateRandomExpr(uint32_t num_vars, uint32_t target_nodes, std::mt19937 &rng) {
        std::vector< ExprPtr > pool;
        // Seed the pool with leaves.
        uint32_t initial_leaves = std::max(2U, target_nodes / 3);
        pool.reserve(initial_leaves);
        for (uint32_t i = 0; i < initial_leaves; ++i) {
            pool.push_back(RandomLeaf(num_vars, rng));
        }
        uint32_t nodes = initial_leaves;

        while (nodes < target_nodes && pool.size() >= 2) {
            std::uniform_int_distribution< int > action(0, 4);
            if (action(rng) == 0 && pool.size() >= 1) {
                // Unary op on a random element.
                std::uniform_int_distribution< size_t > idx(0, pool.size() - 1);
                auto i   = idx(rng);
                pool[i]  = RandomUnaryOp(std::move(pool[i]), rng);
                nodes   += 1;
            } else {
                // Binary op: pick two random indices, combine, remove one.
                std::uniform_int_distribution< size_t > idx(0, pool.size() - 1);
                auto a = idx(rng);
                auto b = idx(rng);
                while (b == a) { b = idx(rng); }
                auto combined = RandomBinaryOp(CloneExpr(*pool[a]), CloneExpr(*pool[b]), rng);
                pool[a]       = std::move(combined);
                pool.erase(pool.begin() + static_cast< ptrdiff_t >(b));
                nodes += 1;
            }
        }

        // Fold remaining pool into a single expression.
        while (pool.size() > 1) {
            auto rhs = std::move(pool.back());
            pool.pop_back();
            pool.back() = Expr::Add(std::move(pool.back()), std::move(rhs));
        }
        return std::move(pool[0]);
    }

    // Build expression with repeated subtrees (simulates post-lifting
    // outer expression where virtual variables appear multiple times).
    ExprPtr GenerateRepeatedSubtreeExpr(
        uint32_t num_vars, uint32_t repeats, std::mt19937 & /*rng*/
    ) {
        // Create a non-trivial shared subtree.
        auto subtree = Expr::BitwiseXor(
            Expr::Add(Expr::Variable(0), Expr::Variable(num_vars > 1 ? 1 : 0)),
            Expr::BitwiseNot(Expr::Variable(num_vars > 2 ? 2 : 0))
        );

        ExprPtr result = CloneExpr(*subtree);
        for (uint32_t i = 1; i < repeats; ++i) {
            result = Expr::Add(
                std::move(result), Expr::Mul(Expr::Constant(i + 1), CloneExpr(*subtree))
            );
        }
        return result;
    }

    uint32_t CountNodes(const Expr &e) {
        uint32_t count = 1;
        for (const auto &c : e.children) { count += CountNodes(*c); }
        return count;
    }

    // ---------------------------------------------------------------
    // Timing helpers
    // ---------------------------------------------------------------

    struct TimingResult
    {
        double min_us;
        double median_us;
        double mean_us;
        double max_us;
    };

    template< typename Fn >
    TimingResult MeasureRepeated(Fn &&fn, uint32_t warmup, uint32_t iters) {
        for (uint32_t i = 0; i < warmup; ++i) { fn(); }

        std::vector< double > times;
        times.reserve(iters);
        for (uint32_t i = 0; i < iters; ++i) {
            auto t0 = Clock::now();
            fn();
            auto t1 = Clock::now();
            times.push_back(std::chrono::duration< double, std::micro >(t1 - t0).count());
        }
        std::sort(times.begin(), times.end());

        double sum = std::accumulate(times.begin(), times.end(), 0.0);
        return TimingResult{
            .min_us    = times.front(),
            .median_us = times[times.size() / 2],
            .mean_us   = sum / static_cast< double >(times.size()),
            .max_us    = times.back(),
        };
    }

    void PrintHeader(const std::string &section) {
        std::cout << "\n=== " << section << " ===\n";
    }

    void PrintTableHeader() {
        std::cout << std::left << std::setw(8) << "vars" << std::setw(8) << "nodes"
                  << std::setw(8) << "2^n" << std::setw(12) << "min_us" << std::setw(12)
                  << "median_us" << std::setw(12) << "mean_us" << std::setw(12) << "max_us"
                  << "\n";
        std::cout << std::string(72, '-') << "\n";
    }

    void PrintRow(uint32_t vars, uint32_t nodes, const TimingResult &t) {
        std::cout << std::left << std::setw(8) << vars << std::setw(8) << nodes << std::setw(8)
                  << (1U << vars) << std::fixed << std::setprecision(1) << std::setw(12)
                  << t.min_us << std::setw(12) << t.median_us << std::setw(12) << t.mean_us
                  << std::setw(12) << t.max_us << "\n";
    }

} // namespace

// ---------------------------------------------------------------
// 1. Parameter grid: var count × node count
// ---------------------------------------------------------------

TEST(SignatureEvalBench, ParameterGrid) {
    constexpr uint32_t kBitwidth = 64;
    constexpr uint32_t kWarmup   = 3;

    struct GridPoint
    {
        uint32_t vars;
        uint32_t target_nodes;
        uint32_t iters;
    };

    // Scale iterations inversely with expected cost (2^vars × nodes).
    const std::vector< GridPoint > grid = {
        {  2,  10, 5000 },
        {  2,  50, 2000 },
        {  2, 200, 1000 },
        {  3,  10, 5000 },
        {  3,  50, 2000 },
        {  3, 200, 1000 },
        {  4,  10, 3000 },
        {  4,  50, 1000 },
        {  4, 200,  500 },
        {  5,  10, 2000 },
        {  5,  50,  500 },
        {  5, 200,  200 },
        {  6,  10, 1000 },
        {  6,  50,  200 },
        {  6, 200,  100 },
        {  8,  10,  500 },
        {  8,  50,  100 },
        {  8, 200,   50 },
        { 10,  10,  200 },
        { 10,  50,   50 },
        { 10, 200,   20 },
        { 12,  10,   50 },
        { 12,  50,   20 },
        { 14,  10,   20 },
        { 16,  10,   10 },
    };

    std::mt19937 rng(42);

    PrintHeader("Parameter Grid: EvaluateBooleanSignature(Expr)");
    PrintTableHeader();

    for (const auto &gp : grid) {
        auto expr  = GenerateRandomExpr(gp.vars, gp.target_nodes, rng);
        auto nodes = CountNodes(*expr);
        auto t     = MeasureRepeated(
            [&]() { EvaluateBooleanSignature(*expr, gp.vars, kBitwidth); }, kWarmup, gp.iters
        );
        PrintRow(gp.vars, nodes, t);
    }
}

// ---------------------------------------------------------------
// 2. One-shot vs repeated evaluation of the same AST
// ---------------------------------------------------------------

TEST(SignatureEvalBench, RepeatedSameAst) {
    constexpr uint32_t kBitwidth = 64;
    constexpr uint32_t kWarmup   = 3;
    std::mt19937 rng(123);

    struct Config
    {
        uint32_t vars;
        uint32_t nodes;
        uint32_t repeats;
    };

    const std::vector< Config > configs = {
        {  4, 30, 100 },
        {  6, 30, 100 },
        {  8, 30,  50 },
        { 10, 30,  20 },
    };

    PrintHeader("Repeated Same AST (no caching, full re-eval)");
    std::cout << std::left << std::setw(8) << "vars" << std::setw(8) << "nodes" << std::setw(10)
              << "repeats" << std::setw(12) << "first_us" << std::setw(12) << "rest_us"
              << std::setw(12) << "ratio"
              << "\n";
    std::cout << std::string(62, '-') << "\n";

    for (const auto &cfg : configs) {
        auto expr  = GenerateRandomExpr(cfg.vars, cfg.nodes, rng);
        auto nodes = CountNodes(*expr);

        // Warmup
        for (uint32_t i = 0; i < kWarmup; ++i) {
            EvaluateBooleanSignature(*expr, cfg.vars, kBitwidth);
        }

        // First eval
        auto t0 = Clock::now();
        EvaluateBooleanSignature(*expr, cfg.vars, kBitwidth);
        auto t1         = Clock::now();
        double first_us = std::chrono::duration< double, std::micro >(t1 - t0).count();

        // Subsequent evals
        auto t2 = Clock::now();
        for (uint32_t i = 1; i < cfg.repeats; ++i) {
            EvaluateBooleanSignature(*expr, cfg.vars, kBitwidth);
        }
        auto t3        = Clock::now();
        double rest_us = std::chrono::duration< double, std::micro >(t3 - t2).count()
            / static_cast< double >(cfg.repeats - 1);

        std::cout << std::left << std::setw(8) << cfg.vars << std::setw(8) << nodes
                  << std::setw(10) << cfg.repeats << std::fixed << std::setprecision(1)
                  << std::setw(12) << first_us << std::setw(12) << rest_us << std::setw(12)
                  << first_us / rest_us << "\n";
    }
}

// ---------------------------------------------------------------
// 3. Expr tree-walk vs compiled Evaluator (point-by-point)
// ---------------------------------------------------------------

TEST(SignatureEvalBench, ExprVsCompiled) {
    constexpr uint32_t kBitwidth = 64;
    constexpr uint32_t kWarmup   = 3;
    std::mt19937 rng(456);

    struct Config
    {
        uint32_t vars;
        uint32_t nodes;
        uint32_t iters;
    };

    const std::vector< Config > configs = {
        {  3, 20, 2000 },
        {  4, 30, 1000 },
        {  5, 30,  500 },
        {  6, 30,  200 },
        {  8, 30,   50 },
        { 10, 30,   20 },
    };

    PrintHeader("Expr tree-walk vs Compiled Evaluator (point-by-point)");
    std::cout << std::left << std::setw(8) << "vars" << std::setw(8) << "nodes" << std::setw(14)
              << "expr_us" << std::setw(14) << "compiled_us" << std::setw(10) << "ratio"
              << "\n";
    std::cout << std::string(54, '-') << "\n";

    for (const auto &cfg : configs) {
        auto expr      = GenerateRandomExpr(cfg.vars, cfg.nodes, rng);
        auto nodes     = CountNodes(*expr);
        auto evaluator = Evaluator::FromExpr(*expr, kBitwidth);

        auto t_expr = MeasureRepeated(
            [&]() { EvaluateBooleanSignature(*expr, cfg.vars, kBitwidth); }, kWarmup, cfg.iters
        );
        auto t_compiled = MeasureRepeated(
            [&]() { EvaluateBooleanSignature(evaluator, cfg.vars, kBitwidth); }, kWarmup,
            cfg.iters
        );

        std::cout << std::left << std::setw(8) << cfg.vars << std::setw(8) << nodes
                  << std::fixed << std::setprecision(1) << std::setw(14) << t_expr.median_us
                  << std::setw(14) << t_compiled.median_us << std::setw(10)
                  << t_expr.median_us / t_compiled.median_us << "\n";
    }
}

// ---------------------------------------------------------------
// 4. Repeated subtrees (post-lifting scenario)
// ---------------------------------------------------------------

TEST(SignatureEvalBench, SharedSubtrees) {
    constexpr uint32_t kBitwidth = 64;
    constexpr uint32_t kWarmup   = 3;
    std::mt19937 rng(789);

    struct Config
    {
        uint32_t vars;
        uint32_t repeats;
        uint32_t iters;
    };

    const std::vector< Config > configs = {
        { 3,  2, 2000 },
        { 3,  5, 1000 },
        { 3, 10,  500 },
        { 4,  2, 1000 },
        { 4,  5,  500 },
        { 4, 10,  200 },
        { 6,  2,  200 },
        { 6,  5,  100 },
        { 6, 10,   50 },
    };

    PrintHeader("Repeated Subtrees (simulated post-lifting outer)");
    std::cout << std::left << std::setw(8) << "vars" << std::setw(10) << "repeats"
              << std::setw(8) << "nodes" << std::setw(12) << "median_us" << std::setw(14)
              << "us_per_node"
              << "\n";
    std::cout << std::string(52, '-') << "\n";

    for (const auto &cfg : configs) {
        auto expr  = GenerateRepeatedSubtreeExpr(cfg.vars, cfg.repeats, rng);
        auto nodes = CountNodes(*expr);

        auto t = MeasureRepeated(
            [&]() { EvaluateBooleanSignature(*expr, cfg.vars, kBitwidth); }, kWarmup, cfg.iters
        );

        std::cout << std::left << std::setw(8) << cfg.vars << std::setw(10) << cfg.repeats
                  << std::setw(8) << nodes << std::fixed << std::setprecision(1)
                  << std::setw(12) << t.median_us << std::setprecision(3) << std::setw(14)
                  << t.median_us / static_cast< double >(nodes) << "\n";
    }
}

// ---------------------------------------------------------------
// 5. Allocation cost isolation: measure vector allocation overhead
//    by comparing signature eval to an equivalent computation that
//    reuses pre-allocated buffers.
// ---------------------------------------------------------------

TEST(SignatureEvalBench, AllocationOverhead) {
    constexpr uint32_t kBitwidth = 64;
    constexpr uint32_t kWarmup   = 3;
    const uint64_t kMask         = (kBitwidth < 64) ? (1ULL << kBitwidth) - 1 : ~0ULL;
    std::mt19937 rng(321);

    struct Config
    {
        uint32_t vars;
        uint32_t nodes;
        uint32_t iters;
    };

    const std::vector< Config > configs = {
        {  4, 30, 1000 },
        {  6, 30,  200 },
        {  8, 30,   50 },
        { 10, 30,   20 },
    };

    PrintHeader("Allocation Overhead: tree-walk vs flat compiled");
    std::cout << std::left << std::setw(8) << "vars" << std::setw(8) << "nodes" << std::setw(14)
              << "treewalk_us" << std::setw(14) << "flatloop_us" << std::setw(10) << "ratio"
              << std::setw(12) << "alloc_frac"
              << "\n";
    std::cout << std::string(66, '-') << "\n";

    for (const auto &cfg : configs) {
        auto expr     = GenerateRandomExpr(cfg.vars, cfg.nodes, rng);
        auto nodes    = CountNodes(*expr);
        auto compiled = CompileExpr(*expr, kBitwidth);

        // Method 1: current tree-walk (allocates per node)
        auto t_tree = MeasureRepeated(
            [&]() { EvaluateBooleanSignature(*expr, cfg.vars, kBitwidth); }, kWarmup, cfg.iters
        );

        // Method 2: flat compiled loop (single allocation + reuse)
        const size_t kLen = size_t{ 1 } << cfg.vars;
        auto t_flat       = MeasureRepeated(
            [&]() {
                std::vector< uint64_t > sig(kLen);
                std::vector< uint64_t > point(cfg.vars);
                std::vector< uint64_t > stack(compiled.stack_size);
                for (size_t i = 0; i < kLen; ++i) {
                    for (uint32_t v = 0; v < cfg.vars; ++v) { point[v] = (i >> v) & 1; }
                    sig[i] = EvalCompiledExpr(compiled, point, stack) & kMask;
                }
            },
            kWarmup, cfg.iters
        );

        double alloc_frac = 1.0 - t_flat.median_us / t_tree.median_us;

        std::cout << std::left << std::setw(8) << cfg.vars << std::setw(8) << nodes
                  << std::fixed << std::setprecision(1) << std::setw(14) << t_tree.median_us
                  << std::setw(14) << t_flat.median_us << std::setprecision(2) << std::setw(10)
                  << t_tree.median_us / t_flat.median_us << std::setprecision(1)
                  << std::setw(12) << (alloc_frac * 100.0) << "%"
                  << "\n";
    }
}
