#include "cobra/core/SignatureEval.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include "cobra/core/Profile.h"
#include "cobra/core/SignatureEvalStats.h"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cobra {

    namespace {

        // Bottom-up evaluation: walk the expression tree once,
        // computing all 2^n outputs per node instead of calling
        // eval_expr 2^n times. Complexity: O(tree_size * 2^n)
        // element-wise ops in a single tree walk, vs O(tree_size)
        // recursive calls * 2^n invocations in the naive approach.
        // The key win is eliminating function call/dispatch overhead.
        std::vector< uint64_t >
        EvalSigRecursive(const Expr &expr, size_t len, uint32_t bitwidth) {
            const uint64_t kMask = Bitmask(bitwidth);

            switch (expr.kind) {
                case Expr::Kind::kConstant: {
                    return std::vector< uint64_t >(len, expr.constant_val & kMask);
                }
                case Expr::Kind::kVariable: {
                    std::vector< uint64_t > r(len);
                    const uint32_t kIdx = expr.var_index;
                    for (size_t i = 0; i < len; ++i) { r[i] = (i >> kIdx) & 1; }
                    return r;
                }
                case Expr::Kind::kNot: {
                    auto child = EvalSigRecursive(*expr.children[0], len, bitwidth);
                    for (size_t i = 0; i < len; ++i) { child[i] = (~child[i]) & kMask; }
                    return child;
                }
                case Expr::Kind::kNeg: {
                    auto child = EvalSigRecursive(*expr.children[0], len, bitwidth);
                    for (size_t i = 0; i < len; ++i) { child[i] = (-child[i]) & kMask; }
                    return child;
                }
                case Expr::Kind::kShr: {
                    auto child        = EvalSigRecursive(*expr.children[0], len, bitwidth);
                    const uint64_t kK = expr.constant_val;
                    if (kK >= 64) {
                        std::fill(child.begin(), child.end(), 0);
                    } else {
                        for (size_t i = 0; i < len; ++i) {
                            child[i] = (child[i] >> kK) & kMask;
                        }
                    }
                    return child;
                }
                case Expr::Kind::kAdd: {
                    auto left  = EvalSigRecursive(*expr.children[0], len, bitwidth);
                    auto right = EvalSigRecursive(*expr.children[1], len, bitwidth);
                    for (size_t i = 0; i < len; ++i) { left[i] = (left[i] + right[i]) & kMask; }
                    return left;
                }
                case Expr::Kind::kMul: {
                    auto left  = EvalSigRecursive(*expr.children[0], len, bitwidth);
                    auto right = EvalSigRecursive(*expr.children[1], len, bitwidth);
                    for (size_t i = 0; i < len; ++i) { left[i] = (left[i] * right[i]) & kMask; }
                    return left;
                }
                case Expr::Kind::kAnd: {
                    auto left  = EvalSigRecursive(*expr.children[0], len, bitwidth);
                    auto right = EvalSigRecursive(*expr.children[1], len, bitwidth);
                    for (size_t i = 0; i < len; ++i) { left[i] = left[i] & right[i]; }
                    return left;
                }
                case Expr::Kind::kOr: {
                    auto left  = EvalSigRecursive(*expr.children[0], len, bitwidth);
                    auto right = EvalSigRecursive(*expr.children[1], len, bitwidth);
                    for (size_t i = 0; i < len; ++i) { left[i] = (left[i] | right[i]) & kMask; }
                    return left;
                }
                case Expr::Kind::kXor: {
                    auto left  = EvalSigRecursive(*expr.children[0], len, bitwidth);
                    auto right = EvalSigRecursive(*expr.children[1], len, bitwidth);
                    for (size_t i = 0; i < len; ++i) { left[i] = (left[i] ^ right[i]) & kMask; }
                    return left;
                }
            }
            return std::vector< uint64_t >(len, 0);
        }

        uint32_t CountNodesLocal(const Expr &e) {
            uint32_t n = 1;
            for (const auto &c : e.children) { n += CountNodesLocal(*c); }
            return n;
        }

    } // namespace

    std::vector< uint64_t >
    EvaluateBooleanSignature(const Expr &expr, uint32_t num_vars, uint32_t bitwidth) {
        COBRA_ZONE_N("EvaluateBooleanSignature");
        [[maybe_unused]] auto t0 = std::chrono::high_resolution_clock::now();
        const size_t kLen        = size_t{ 1 } << num_vars;
        auto result              = EvalSigRecursive(expr, kLen, bitwidth);
#ifdef COBRA_SIG_STATS
        auto t1   = std::chrono::high_resolution_clock::now();
        double us = std::chrono::duration< double, std::micro >(t1 - t0).count();
        SigStatsRecordExpr(num_vars, CountNodesLocal(expr), us);
#endif
        return result;
    }

    std::vector< uint64_t >
    EvaluateBooleanSignature(const Evaluator &eval, uint32_t num_vars, uint32_t bitwidth) {
        [[maybe_unused]] auto t0 = std::chrono::high_resolution_clock::now();
        const size_t kLen        = size_t{ 1 } << num_vars;
        const uint64_t kMask     = Bitmask(bitwidth);
        std::vector< uint64_t > sig(kLen);
        std::vector< uint64_t > point(num_vars);
        EvaluatorWorkspace workspace;
        for (size_t i = 0; i < kLen; ++i) {
            for (uint32_t v = 0; v < num_vars; ++v) { point[v] = (i >> v) & 1; }
            sig[i] = (eval.HasCompiledExpr() ? eval.EvaluateWithWorkspace(point, workspace)
                                             : eval(point))
                & kMask;
        }
#ifdef COBRA_SIG_STATS
        auto t1   = std::chrono::high_resolution_clock::now();
        double us = std::chrono::duration< double, std::micro >(t1 - t0).count();
        SigStatsRecordEval(num_vars, us);
#endif
        return sig;
    }

    // ---------------------------------------------------------------
    // Stats implementation (only when COBRA_SIG_STATS is defined)
    // ---------------------------------------------------------------

#ifdef COBRA_SIG_STATS

    namespace {
        thread_local SigEvalStats tl_stats{};
    }

    void SigStatsRecordExpr(uint32_t num_vars, uint32_t node_count, double elapsed_us) {
        tl_stats.calls++;
        tl_stats.expr_calls++;
        tl_stats.total_points += size_t{ 1 } << num_vars;
        tl_stats.total_nodes  += node_count;
        tl_stats.total_us     += elapsed_us;
    }

    void SigStatsRecordEval(uint32_t num_vars, double elapsed_us) {
        tl_stats.calls++;
        tl_stats.eval_calls++;
        tl_stats.total_points += size_t{ 1 } << num_vars;
        tl_stats.total_us     += elapsed_us;
    }

    SigEvalStats SigStatsSnapshot() { return tl_stats; }

    void SigStatsReset() { tl_stats = SigEvalStats{}; }

#endif

} // namespace cobra
