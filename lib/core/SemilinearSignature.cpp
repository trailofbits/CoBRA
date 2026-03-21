#include "cobra/core/SemilinearSignature.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include "cobra/core/Trace.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cobra {

    namespace {

        /// Bottom-up evaluation with variables taking {0, 2^bit_pos} values.
        /// Structurally identical to SignatureEval::EvalSigRecursive, but
        /// the kVariable case returns 2^bit_pos instead of 1.
        std::vector< uint64_t > EvalSemilinearRecursive(
            const Expr &expr, size_t len, uint32_t bitwidth, uint32_t bit_pos
        ) {
            const uint64_t kMask   = Bitmask(bitwidth);
            const uint64_t kBitVal = (bit_pos < 64) ? (1ULL << bit_pos) : 0;

            switch (expr.kind) {
                case Expr::Kind::kConstant:
                    return std::vector< uint64_t >(len, expr.constant_val & kMask);

                case Expr::Kind::kVariable: {
                    std::vector< uint64_t > r(len);
                    const uint32_t kIdx = expr.var_index;
                    for (size_t i = 0; i < len; ++i) {
                        r[i] = ((i >> kIdx) & 1) != 0 ? kBitVal : 0;
                    }
                    return r;
                }

                case Expr::Kind::kNot: {
                    auto child =
                        EvalSemilinearRecursive(*expr.children[0], len, bitwidth, bit_pos);
                    for (auto &v : child) { v = (~v) & kMask; }
                    return child;
                }
                case Expr::Kind::kNeg: {
                    auto child =
                        EvalSemilinearRecursive(*expr.children[0], len, bitwidth, bit_pos);
                    for (auto &v : child) { v = (-v) & kMask; }
                    return child;
                }
                case Expr::Kind::kShr: {
                    auto child =
                        EvalSemilinearRecursive(*expr.children[0], len, bitwidth, bit_pos);
                    const uint64_t kK = expr.constant_val;
                    if (kK >= 64) {
                        std::fill(child.begin(), child.end(), 0);
                    } else {
                        for (auto &v : child) { v = (v >> kK) & kMask; }
                    }
                    return child;
                }

                case Expr::Kind::kAdd: {
                    auto left =
                        EvalSemilinearRecursive(*expr.children[0], len, bitwidth, bit_pos);
                    auto right =
                        EvalSemilinearRecursive(*expr.children[1], len, bitwidth, bit_pos);
                    for (size_t i = 0; i < len; ++i) { left[i] = (left[i] + right[i]) & kMask; }
                    return left;
                }
                case Expr::Kind::kMul: {
                    auto left =
                        EvalSemilinearRecursive(*expr.children[0], len, bitwidth, bit_pos);
                    auto right =
                        EvalSemilinearRecursive(*expr.children[1], len, bitwidth, bit_pos);
                    for (size_t i = 0; i < len; ++i) { left[i] = (left[i] * right[i]) & kMask; }
                    return left;
                }

                case Expr::Kind::kAnd: {
                    auto left =
                        EvalSemilinearRecursive(*expr.children[0], len, bitwidth, bit_pos);
                    auto right =
                        EvalSemilinearRecursive(*expr.children[1], len, bitwidth, bit_pos);
                    for (size_t i = 0; i < len; ++i) { left[i] = left[i] & right[i]; }
                    return left;
                }
                case Expr::Kind::kOr: {
                    auto left =
                        EvalSemilinearRecursive(*expr.children[0], len, bitwidth, bit_pos);
                    auto right =
                        EvalSemilinearRecursive(*expr.children[1], len, bitwidth, bit_pos);
                    for (size_t i = 0; i < len; ++i) { left[i] = (left[i] | right[i]) & kMask; }
                    return left;
                }
                case Expr::Kind::kXor: {
                    auto left =
                        EvalSemilinearRecursive(*expr.children[0], len, bitwidth, bit_pos);
                    auto right =
                        EvalSemilinearRecursive(*expr.children[1], len, bitwidth, bit_pos);
                    for (size_t i = 0; i < len; ++i) { left[i] = (left[i] ^ right[i]) & kMask; }
                    return left;
                }
            }
            return std::vector< uint64_t >(len, 0);
        }

    } // namespace

    std::vector< uint64_t > EvaluateSemilinearRow(
        const Expr &expr, uint32_t num_vars, uint32_t bitwidth, uint32_t bit_pos
    ) {
        const size_t kLen = size_t{ 1 } << num_vars;
        auto result       = EvalSemilinearRecursive(expr, kLen, bitwidth, bit_pos);

        if (bit_pos > 0 && bit_pos < 64) {
            const uint64_t kMask = Bitmask(bitwidth);
            for (auto &v : result) { v = (v >> bit_pos) & kMask; }
        }
        return result;
    }

    namespace {

        /// Evaluate expression at a specific variable assignment.
        uint64_t
        EvalAtPoint(const Expr &expr, const std::vector< uint64_t > &var_vals, uint64_t mask) {
            switch (expr.kind) {
                case Expr::Kind::kConstant:
                    return expr.constant_val & mask;
                case Expr::Kind::kVariable:
                    return var_vals[expr.var_index] & mask;
                case Expr::Kind::kNot:
                    return (~EvalAtPoint(*expr.children[0], var_vals, mask)) & mask;
                case Expr::Kind::kNeg:
                    return (-EvalAtPoint(*expr.children[0], var_vals, mask)) & mask;
                case Expr::Kind::kShr: {
                    auto v = EvalAtPoint(*expr.children[0], var_vals, mask);
                    return (expr.constant_val >= 64) ? 0 : (v >> expr.constant_val) & mask;
                }
                case Expr::Kind::kAdd:
                    return (EvalAtPoint(*expr.children[0], var_vals, mask)
                            + EvalAtPoint(*expr.children[1], var_vals, mask))
                        & mask;
                case Expr::Kind::kMul:
                    return (EvalAtPoint(*expr.children[0], var_vals, mask)
                            * EvalAtPoint(*expr.children[1], var_vals, mask))
                        & mask;
                case Expr::Kind::kAnd:
                    return EvalAtPoint(*expr.children[0], var_vals, mask)
                        & EvalAtPoint(*expr.children[1], var_vals, mask);
                case Expr::Kind::kOr:
                    return (EvalAtPoint(*expr.children[0], var_vals, mask)
                            | EvalAtPoint(*expr.children[1], var_vals, mask))
                        & mask;
                case Expr::Kind::kXor:
                    return (EvalAtPoint(*expr.children[0], var_vals, mask)
                            ^ EvalAtPoint(*expr.children[1], var_vals, mask))
                        & mask;
            }
            return 0;
        }

    } // namespace

    bool IsLinearShortcut(const Expr &expr, uint32_t num_vars, uint32_t bitwidth) {
        COBRA_TRACE(
            "SemilinearSig", "IsLinearShortcut: vars={} bitwidth={}", num_vars, bitwidth
        );
        if (bitwidth == 0 || num_vars > 20) { return false; }

        const uint64_t kMask = Bitmask(bitwidth);
        std::vector< uint64_t > assignment(num_vars, 0);
        const uint64_t kFZero = EvalAtPoint(expr, assignment, kMask);

        // Extract per-variable coefficient from bit 0.
        std::vector< uint64_t > coeff(num_vars);
        for (uint32_t j = 0; j < num_vars; ++j) {
            assignment[j] = 1;
            coeff[j]      = (EvalAtPoint(expr, assignment, kMask) - kFZero) & kMask;
            assignment[j] = 0;
        }

        // Verify at each bit position: delta == coeff[j] * 2^bit.
        for (uint32_t bit = 1; bit < bitwidth; ++bit) {
            const uint64_t kBitVal = 1ULL << bit;
            for (uint32_t j = 0; j < num_vars; ++j) {
                assignment[j]         = kBitVal;
                const uint64_t kDelta = (EvalAtPoint(expr, assignment, kMask) - kFZero) & kMask;
                const uint64_t kExpect = (coeff[j] * kBitVal) & kMask;
                if (kDelta != kExpect) {
                    COBRA_TRACE(
                        "SemilinearSig",
                        "IsLinearShortcut: false (var={} bit={} delta={} expect={})", j, bit,
                        kDelta, kExpect
                    );
                    return false;
                }
                assignment[j] = 0;
            }
        }

        COBRA_TRACE("SemilinearSig", "IsLinearShortcut: true");
        return true;
    }

} // namespace cobra
