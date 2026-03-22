#include "cobra/core/SignatureChecker.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/Trace.h"

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

        // Deterministic adversarial scalar values that stress carry
        // propagation, sign boundaries, and alternating bits.
        std::vector< uint64_t > BuildAdversarialValues(uint32_t bitwidth) {
            const uint64_t kMask = Bitmask(bitwidth);
            std::vector< uint64_t > vals;
            vals.reserve(32);

            vals.push_back(0);
            vals.push_back(1);
            vals.push_back(kMask);
            vals.push_back((kMask - 1) & kMask);

            // Carry-propagation: 2^k - 1, 2^k, 2^k + 1
            for (uint32_t k = 1; k < bitwidth; ++k) {
                uint64_t pow = uint64_t{ 1 } << k;
                vals.push_back((pow - 1) & kMask);
                vals.push_back(pow & kMask);
                if (k + 1 < bitwidth) { vals.push_back((pow + 1) & kMask); }
            }

            vals.push_back(0x5555555555555555ULL & kMask);
            vals.push_back(0xAAAAAAAAAAAAAAAAULL & kMask);

            return vals;
        }

    } // namespace

    CheckResult FullWidthCheck(
        const Expr &original, uint32_t original_num_vars, const Expr &simplified,
        const std::vector< uint32_t > &var_map, uint32_t bitwidth, uint32_t num_samples
    ) {
        COBRA_TRACE(
            "Verifier", "FullWidthCheck: vars={} bitwidth={} samples={}", original_num_vars,
            bitwidth, num_samples
        );
        const uint64_t kMask = Bitmask(bitwidth);

        const uint32_t kSimpNumVars =
            var_map.empty() ? original_num_vars : static_cast< uint32_t >(var_map.size());

        std::vector< uint64_t > orig_inputs(original_num_vars);
        std::vector< uint64_t > simp_inputs(kSimpNumVars);

        auto check_one = [&]() -> bool {
            for (uint32_t v = 0; v < kSimpNumVars; ++v) {
                const uint32_t kOrigIdx = var_map.empty() ? v : var_map[v];
                simp_inputs[v]          = orig_inputs[kOrigIdx];
            }
            return EvalExpr(original, orig_inputs, bitwidth)
                == EvalExpr(simplified, simp_inputs, bitwidth);
        };

        auto adv = BuildAdversarialValues(bitwidth);

        // Phase 1a: broadcast each adversarial value to all variables.
        for (const auto &val : adv) {
            for (uint32_t v = 0; v < original_num_vars; ++v) { orig_inputs[v] = val; }
            if (!check_one()) {
                auto result =
                    CheckResult{ .passed = false, .failing_input = std::move(orig_inputs) };
                COBRA_TRACE("Verifier", "FullWidthCheck: passed={}", result.passed);
                return result;
            }
        }

        // Phase 1b: per-variable probes — set one variable to an
        // adversarial value while others stay 0. Catches cross-variable
        // carry interactions (e.g., f(3,0) ≠ simplified(3,0)).
        for (uint32_t v = 0; v < original_num_vars; ++v) {
            for (const auto &val : adv) {
                for (uint32_t u = 0; u < original_num_vars; ++u) {
                    orig_inputs[u] = (u == v) ? val : 0;
                }
                if (!check_one()) {
                    auto result =
                        CheckResult{ .passed = false, .failing_input = std::move(orig_inputs) };
                    COBRA_TRACE("Verifier", "FullWidthCheck: passed={}", result.passed);
                    return result;
                }
            }
        }

        // Phase 2: random full-width probes.
        uint64_t rng_state = (static_cast< uint64_t >(bitwidth) * 2654435761ULL)
            + (static_cast< uint64_t >(original_num_vars) * 40503ULL);
        for (uint32_t s = 0; s < num_samples; ++s) {
            for (uint32_t v = 0; v < original_num_vars; ++v) {
                orig_inputs[v] = Splitmix64(rng_state) & kMask;
            }
            if (!check_one()) {
                auto result =
                    CheckResult{ .passed = false, .failing_input = std::move(orig_inputs) };
                COBRA_TRACE("Verifier", "FullWidthCheck: passed={}", result.passed);
                return result;
            }
        }
        auto result = CheckResult{ .passed = true, .failing_input = {} };
        COBRA_TRACE("Verifier", "FullWidthCheck: passed={}", result.passed);
        return result;
    }

    CheckResult FullWidthCheckEval(
        const std::function< uint64_t(const std::vector< uint64_t > &) > &eval_original,
        uint32_t num_vars, const Expr &simplified, uint32_t bitwidth, uint32_t num_samples
    ) {
        COBRA_TRACE(
            "Verifier", "FullWidthCheckEval: vars={} bitwidth={} samples={}", num_vars,
            bitwidth, num_samples
        );
        const uint64_t kMask = Bitmask(bitwidth);

        std::vector< uint64_t > inputs(num_vars);

        auto check_one = [&]() -> bool {
            return (eval_original(inputs) & kMask) == EvalExpr(simplified, inputs, bitwidth);
        };

        auto adv = BuildAdversarialValues(bitwidth);

        // Phase 1a: broadcast each adversarial value to all variables.
        for (const auto &val : adv) {
            for (uint32_t v = 0; v < num_vars; ++v) { inputs[v] = val; }
            if (!check_one()) {
                auto result =
                    CheckResult{ .passed = false, .failing_input = std::move(inputs) };
                COBRA_TRACE("Verifier", "FullWidthCheckEval: passed={}", result.passed);
                return result;
            }
        }

        // Phase 1b: per-variable probes — set one variable to an
        // adversarial value while others stay 0.
        for (uint32_t v = 0; v < num_vars; ++v) {
            for (const auto &val : adv) {
                for (uint32_t u = 0; u < num_vars; ++u) { inputs[u] = (u == v) ? val : 0; }
                if (!check_one()) {
                    auto result =
                        CheckResult{ .passed = false, .failing_input = std::move(inputs) };
                    COBRA_TRACE("Verifier", "FullWidthCheckEval: passed={}", result.passed);
                    return result;
                }
            }
        }

        // Phase 2: random full-width probes.
        uint64_t rng_state = (static_cast< uint64_t >(bitwidth) * 2654435761ULL)
            + (static_cast< uint64_t >(num_vars) * 40503ULL);
        for (uint32_t s = 0; s < num_samples; ++s) {
            for (uint32_t v = 0; v < num_vars; ++v) {
                inputs[v] = Splitmix64(rng_state) & kMask;
            }
            if (!check_one()) {
                auto result =
                    CheckResult{ .passed = false, .failing_input = std::move(inputs) };
                COBRA_TRACE("Verifier", "FullWidthCheckEval: passed={}", result.passed);
                return result;
            }
        }
        auto result = CheckResult{ .passed = true, .failing_input = {} };
        COBRA_TRACE("Verifier", "FullWidthCheckEval: passed={}", result.passed);
        return result;
    }

    uint64_t
    EvalExpr(const Expr &expr, const std::vector< uint64_t > &var_values, uint32_t bitwidth) {
        const uint64_t kMask = Bitmask(bitwidth);
        switch (expr.kind) {
            case Expr::Kind::kConstant:
                return expr.constant_val & kMask;
            case Expr::Kind::kVariable:
                return var_values[expr.var_index] & kMask;
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
                    & EvalExpr(*expr.children[1], var_values, bitwidth) & kMask;
            case Expr::Kind::kOr:
                return (EvalExpr(*expr.children[0], var_values, bitwidth)
                        | EvalExpr(*expr.children[1], var_values, bitwidth))
                    & kMask;
            case Expr::Kind::kXor:
                return (EvalExpr(*expr.children[0], var_values, bitwidth)
                        ^ EvalExpr(*expr.children[1], var_values, bitwidth))
                    & kMask;
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
        COBRA_TRACE("Verifier", "SignatureCheck: vars={} bitwidth={}", num_vars, bitwidth);
        // Bottom-up evaluation: walk the tree once to get all outputs.
        auto computed        = EvaluateBooleanSignature(simplified, num_vars, bitwidth);
        const uint64_t kMask = Bitmask(bitwidth);
        const size_t kLen    = size_t{ 1 } << num_vars;
        for (size_t i = 0; i < kLen; ++i) {
            if (computed[i] != (original_sig[i] & kMask)) {
                std::vector< uint64_t > inputs(num_vars);
                for (uint32_t v = 0; v < num_vars; ++v) { inputs[v] = (i >> v) & 1; }
                auto result =
                    CheckResult{ .passed = false, .failing_input = std::move(inputs) };
                COBRA_TRACE("Verifier", "SignatureCheck: passed={}", result.passed);
                return result;
            }
        }
        auto result = CheckResult{ .passed = true, .failing_input = {} };
        COBRA_TRACE("Verifier", "SignatureCheck: passed={}", result.passed);
        return result;
    }

} // namespace cobra
