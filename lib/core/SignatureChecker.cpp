#include "cobra/core/SignatureChecker.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include "cobra/core/SignatureEval.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace cobra {

    namespace {

        // SplitMix64: deterministic, fast, good avalanche.
        uint64_t Splitmix64(uint64_t &state) {
            state      += 0x9E3779B97F4A7C15ULL;
            uint64_t z  = state;
            z           = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z           = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            return z ^ (z >> 31);
        }

    } // namespace

    CheckResult FullWidthCheck(
        const Expr &original, uint32_t original_num_vars, const Expr &simplified,
        const std::vector< uint32_t > &var_map, uint32_t bitwidth, uint32_t num_samples
    ) {
        const uint64_t mask = Bitmask(bitwidth);

        // Deterministic seed from parameters so results are reproducible.
        uint64_t rng_state = (static_cast< uint64_t >(bitwidth) * 2654435761ULL)
            + (static_cast< uint64_t >(original_num_vars) * 40503ULL);

        // Determine the simplified variable count from the mapping.
        // If var_map is empty, assume identity (same variable set).
        const uint32_t simp_num_vars =
            var_map.empty() ? original_num_vars : static_cast< uint32_t >(var_map.size());

        std::vector< uint64_t > orig_inputs(original_num_vars);
        std::vector< uint64_t > simp_inputs(simp_num_vars);
        for (uint32_t s = 0; s < num_samples; ++s) {
            for (uint32_t v = 0; v < original_num_vars; ++v) {
                orig_inputs[v] = Splitmix64(rng_state) & mask;
            }

            // Build the simplified input vector by mapping indices.
            for (uint32_t v = 0; v < simp_num_vars; ++v) {
                const uint32_t orig_idx = var_map.empty() ? v : var_map[v];
                simp_inputs[v]          = orig_inputs[orig_idx];
            }

            const uint64_t orig_val = EvalExpr(original, orig_inputs, bitwidth);
            const uint64_t simp_val = EvalExpr(simplified, simp_inputs, bitwidth);
            if (orig_val != simp_val) {
                return CheckResult{ .passed = false, .failing_input = std::move(orig_inputs) };
            }
        }
        return CheckResult{ .passed = true, .failing_input = {} };
    }

    CheckResult FullWidthCheckEval(
        const std::function< uint64_t(const std::vector< uint64_t > &) > &eval_original,
        uint32_t num_vars, const Expr &simplified, uint32_t bitwidth, uint32_t num_samples
    ) {
        const uint64_t mask = Bitmask(bitwidth);
        uint64_t rng_state  = (static_cast< uint64_t >(bitwidth) * 2654435761ULL)
            + (static_cast< uint64_t >(num_vars) * 40503ULL);

        std::vector< uint64_t > inputs(num_vars);
        for (uint32_t s = 0; s < num_samples; ++s) {
            for (uint32_t v = 0; v < num_vars; ++v) {
                inputs[v] = Splitmix64(rng_state) & mask;
            }
            const uint64_t orig_val = eval_original(inputs) & mask;
            const uint64_t simp_val = EvalExpr(simplified, inputs, bitwidth);
            if (orig_val != simp_val) {
                return CheckResult{ .passed = false, .failing_input = std::move(inputs) };
            }
        }
        return CheckResult{ .passed = true, .failing_input = {} };
    }

    uint64_t
    EvalExpr(const Expr &expr, const std::vector< uint64_t > &var_values, uint32_t bitwidth) {
        const uint64_t mask = Bitmask(bitwidth);
        switch (expr.kind) {
            case Expr::Kind::kConstant:
                return expr.constant_val & mask;
            case Expr::Kind::kVariable:
                return var_values[expr.var_index] & mask;
            case Expr::Kind::kAdd:
                return ModAdd(
                    EvalExpr(*expr.children[0], var_values, bitwidth),
                    EvalExpr(*expr.children[1], var_values, bitwidth), bitwidth
                );
            case Expr::Kind::kMul:
                return ModMul(
                    EvalExpr(*expr.children[0], var_values, bitwidth),
                    EvalExpr(*expr.children[1], var_values, bitwidth), bitwidth
                );
            case Expr::Kind::kAnd:
                return EvalExpr(*expr.children[0], var_values, bitwidth)
                    & EvalExpr(*expr.children[1], var_values, bitwidth) & mask;
            case Expr::Kind::kOr:
                return (EvalExpr(*expr.children[0], var_values, bitwidth)
                        | EvalExpr(*expr.children[1], var_values, bitwidth))
                    & mask;
            case Expr::Kind::kXor:
                return (EvalExpr(*expr.children[0], var_values, bitwidth)
                        ^ EvalExpr(*expr.children[1], var_values, bitwidth))
                    & mask;
            case Expr::Kind::kNot:
                return ModNot(EvalExpr(*expr.children[0], var_values, bitwidth), bitwidth);
            case Expr::Kind::kNeg:
                return ModNeg(EvalExpr(*expr.children[0], var_values, bitwidth), bitwidth);
            case Expr::Kind::kShr:
                return ModShr(
                    EvalExpr(*expr.children[0], var_values, bitwidth), expr.constant_val,
                    bitwidth
                );
        }
        return 0; // unreachable
    }

    CheckResult SignatureCheck(
        const std::vector< uint64_t > &original_sig, const Expr &simplified, uint32_t num_vars,
        uint32_t bitwidth
    ) {
        // Bottom-up evaluation: walk the tree once to get all outputs.
        auto computed       = EvaluateBooleanSignature(simplified, num_vars, bitwidth);
        const uint64_t mask = Bitmask(bitwidth);
        const size_t len    = size_t{ 1 } << num_vars;
        for (size_t i = 0; i < len; ++i) {
            if (computed[i] != (original_sig[i] & mask)) {
                std::vector< uint64_t > inputs(num_vars);
                for (uint32_t v = 0; v < num_vars; ++v) {
                    inputs[v] = (i >> v) & 1;
                }
                return CheckResult{ .passed = false, .failing_input = std::move(inputs) };
            }
        }
        return CheckResult{ .passed = true, .failing_input = {} };
    }

} // namespace cobra
