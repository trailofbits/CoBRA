#include "cobra/core/SignatureEval.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include <algorithm>
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
            const uint64_t mask = Bitmask(bitwidth);

            switch (expr.kind) {
                case Expr::Kind::kConstant: {
                    return std::vector< uint64_t >(len, expr.constant_val & mask);
                }
                case Expr::Kind::kVariable: {
                    std::vector< uint64_t > r(len);
                    const uint32_t idx = expr.var_index;
                    for (size_t i = 0; i < len; ++i) {
                        r[i] = (i >> idx) & 1;
                    }
                    return r;
                }
                case Expr::Kind::kNot: {
                    auto child = EvalSigRecursive(*expr.children[0], len, bitwidth);
                    for (size_t i = 0; i < len; ++i) {
                        child[i] = (~child[i]) & mask;
                    }
                    return child;
                }
                case Expr::Kind::kNeg: {
                    auto child = EvalSigRecursive(*expr.children[0], len, bitwidth);
                    for (size_t i = 0; i < len; ++i) {
                        child[i] = (-child[i]) & mask;
                    }
                    return child;
                }
                case Expr::Kind::kShr: {
                    auto child       = EvalSigRecursive(*expr.children[0], len, bitwidth);
                    const uint64_t k = expr.constant_val;
                    if (k >= 64) {
                        std::fill(child.begin(), child.end(), 0);
                    } else {
                        for (size_t i = 0; i < len; ++i) {
                            child[i] = (child[i] >> k) & mask;
                        }
                    }
                    return child;
                }
                case Expr::Kind::kAdd: {
                    auto left  = EvalSigRecursive(*expr.children[0], len, bitwidth);
                    auto right = EvalSigRecursive(*expr.children[1], len, bitwidth);
                    for (size_t i = 0; i < len; ++i) {
                        left[i] = (left[i] + right[i]) & mask;
                    }
                    return left;
                }
                case Expr::Kind::kMul: {
                    auto left  = EvalSigRecursive(*expr.children[0], len, bitwidth);
                    auto right = EvalSigRecursive(*expr.children[1], len, bitwidth);
                    for (size_t i = 0; i < len; ++i) {
                        left[i] = (left[i] * right[i]) & mask;
                    }
                    return left;
                }
                case Expr::Kind::kAnd: {
                    auto left  = EvalSigRecursive(*expr.children[0], len, bitwidth);
                    auto right = EvalSigRecursive(*expr.children[1], len, bitwidth);
                    for (size_t i = 0; i < len; ++i) {
                        left[i] = left[i] & right[i];
                    }
                    return left;
                }
                case Expr::Kind::kOr: {
                    auto left  = EvalSigRecursive(*expr.children[0], len, bitwidth);
                    auto right = EvalSigRecursive(*expr.children[1], len, bitwidth);
                    for (size_t i = 0; i < len; ++i) {
                        left[i] = (left[i] | right[i]) & mask;
                    }
                    return left;
                }
                case Expr::Kind::kXor: {
                    auto left  = EvalSigRecursive(*expr.children[0], len, bitwidth);
                    auto right = EvalSigRecursive(*expr.children[1], len, bitwidth);
                    for (size_t i = 0; i < len; ++i) {
                        left[i] = (left[i] ^ right[i]) & mask;
                    }
                    return left;
                }
            }
            return std::vector< uint64_t >(len, 0);
        }

    } // namespace

    std::vector< uint64_t >
    EvaluateBooleanSignature(const Expr &expr, uint32_t num_vars, uint32_t bitwidth) {
        const size_t len = size_t{ 1 } << num_vars;
        return EvalSigRecursive(expr, len, bitwidth);
    }

} // namespace cobra
