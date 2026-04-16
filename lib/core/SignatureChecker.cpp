#include "cobra/core/SignatureChecker.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include "cobra/core/Profile.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/Trace.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

        std::vector< uint64_t > BuildAdversarialValues(uint32_t bitwidth) {
            const uint64_t kMask = Bitmask(bitwidth);
            std::vector< uint64_t > vals;
            vals.reserve(4 * bitwidth);

            auto push = [&](uint64_t v) { vals.push_back(v & kMask); };

            push(0);
            push(1);
            push(kMask);     // -1
            push(kMask - 1); // -2
            push(kMask - 2); // -3
            push(kMask - 3); // -4

            for (uint32_t k = 1; k < bitwidth; ++k) {
                uint64_t pow = uint64_t{ 1 } << k;
                push(pow - 1);
                push(pow);
                if (k + 1 < bitwidth) { push(pow + 1); }
            }

            push(3);
            push(5);
            push(7);
            push(0x5555555555555555ULL);
            push(0xAAAAAAAAAAAAAAAAULL);

            std::sort(vals.begin(), vals.end());
            vals.erase(std::unique(vals.begin(), vals.end()), vals.end());

            return vals;
        }

        void CollectConstantsAndShifts(
            const Expr &expr, std::vector< uint64_t > &constants,
            std::vector< uint64_t > &shift_amounts
        ) {
            if (expr.kind == Expr::Kind::kConstant) {
                constants.push_back(expr.constant_val);
            } else if (expr.kind == Expr::Kind::kShr) {
                shift_amounts.push_back(expr.constant_val);
            }
            for (const auto &child : expr.children) {
                CollectConstantsAndShifts(*child, constants, shift_amounts);
            }
        }

        std::vector< uint64_t >
        BuildExprDerivedProbes(const Expr *expr_a, const Expr *expr_b, uint32_t bitwidth) {
            const uint64_t kMask = Bitmask(bitwidth);
            std::vector< uint64_t > raw;
            std::vector< uint64_t > shifts;
            if (expr_a != nullptr) { CollectConstantsAndShifts(*expr_a, raw, shifts); }
            if (expr_b != nullptr) { CollectConstantsAndShifts(*expr_b, raw, shifts); }

            for (auto &c : raw) { c &= kMask; }
            std::sort(raw.begin(), raw.end());
            raw.erase(std::unique(raw.begin(), raw.end()), raw.end());
            std::erase(raw, 0ULL);
            std::erase(raw, 1ULL);

            std::sort(shifts.begin(), shifts.end());
            shifts.erase(std::unique(shifts.begin(), shifts.end()), shifts.end());

            std::vector< uint64_t > derived;
            derived.reserve(raw.size() * 6 + raw.size() * raw.size());
            for (uint64_t c : raw) {
                derived.push_back(c);
                derived.push_back((c + 1) & kMask);
                derived.push_back((c - 1) & kMask);
                derived.push_back((~c) & kMask);
                for (uint64_t k : shifts) {
                    if (k < bitwidth) { derived.push_back((c >> k) & kMask); }
                }
            }

            if (raw.size() <= 8) {
                for (size_t i = 0; i < raw.size(); ++i) {
                    for (size_t j = i + 1; j < raw.size(); ++j) {
                        derived.push_back((raw[i] ^ raw[j]) & kMask);
                        derived.push_back((raw[i] + raw[j]) & kMask);
                        derived.push_back((raw[i] - raw[j]) & kMask);
                    }
                }
            }

            std::sort(derived.begin(), derived.end());
            derived.erase(std::unique(derived.begin(), derived.end()), derived.end());
            std::erase(derived, 0ULL);
            std::erase(derived, 1ULL);

            constexpr size_t kMaxDerived = 128;
            if (derived.size() > kMaxDerived) { derived.resize(kMaxDerived); }

            return derived;
        }

        template< typename ProbeFn >
        bool ForEachFullWidthProbe(
            uint32_t num_vars, uint32_t bitwidth, uint32_t num_samples,
            std::vector< uint64_t > &inputs, const std::vector< uint64_t > &expr_constants,
            ProbeFn &&probe_fn
        ) {
            const uint64_t kMask = Bitmask(bitwidth);
            auto adv             = BuildAdversarialValues(bitwidth);
            size_t probe_index   = 0;

            auto zero_inputs = [&]() { std::fill(inputs.begin(), inputs.end(), 0); };

            // Phase 1: adversarial broadcast — all vars = same value.
            for (const auto &val : adv) {
                std::fill(inputs.begin(), inputs.end(), val);
                if (!probe_fn(probe_index++)) { return false; }
            }

            // Phase 2: adversarial per-variable — one var = value, rest = 0.
            for (uint32_t v = 0; v < num_vars; ++v) {
                for (const auto &val : adv) {
                    zero_inputs();
                    inputs[v] = val;
                    if (!probe_fn(probe_index++)) { return false; }
                }
            }

            // Phase 3: constant broadcast — all vars = same expression constant.
            for (const auto &val : expr_constants) {
                std::fill(inputs.begin(), inputs.end(), val);
                if (!probe_fn(probe_index++)) { return false; }
            }

            // Phase 4: constant per-variable — one var = expression constant, rest = 0.
            for (uint32_t v = 0; v < num_vars; ++v) {
                for (const auto &val : expr_constants) {
                    zero_inputs();
                    inputs[v] = val;
                    if (!probe_fn(probe_index++)) { return false; }
                }
            }

            // Phase 5: two-variable constant combinations across all
            // variable pairs. Catches multi-variable Diracs at (C_i, C_j).
            if (num_vars >= 2 && expr_constants.size() >= 2) {
                constexpr size_t kMaxProbes = 64;
                size_t probes               = 0;
                for (uint32_t va = 0; va < num_vars && probes < kMaxProbes; ++va) {
                    for (uint32_t vb = va + 1; vb < num_vars && probes < kMaxProbes; ++vb) {
                        for (size_t ci = 0; ci < expr_constants.size() && probes < kMaxProbes;
                             ++ci)
                        {
                            for (size_t cj = ci + 1;
                                 cj < expr_constants.size() && probes < kMaxProbes; ++cj)
                            {
                                zero_inputs();
                                inputs[va] = expr_constants[ci];
                                inputs[vb] = expr_constants[cj];
                                if (!probe_fn(probe_index++)) { return false; }
                                inputs[va] = expr_constants[cj];
                                inputs[vb] = expr_constants[ci];
                                if (!probe_fn(probe_index++)) { return false; }
                                probes += 2;
                            }
                        }
                    }
                }
            }

            // Phase 6: random probes.
            uint64_t rng_state = (static_cast< uint64_t >(bitwidth) * 2654435761ULL)
                + (static_cast< uint64_t >(num_vars) * 40503ULL);
            for (uint32_t s = 0; s < num_samples; ++s) {
                for (uint32_t v = 0; v < num_vars; ++v) {
                    inputs[v] = Splitmix64(rng_state) & kMask;
                }
                if (!probe_fn(probe_index++)) { return false; }
            }

            return true;
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
        const auto original_eval   = CompileExpr(original, bitwidth);
        const auto simplified_eval = CompileExpr(simplified, bitwidth);

        const uint32_t kSimpNumVars =
            var_map.empty() ? original_num_vars : static_cast< uint32_t >(var_map.size());

        if (original_eval.arity > original_num_vars || simplified_eval.arity > kSimpNumVars) {
            auto result = CheckResult{ .passed = false, .failing_input = {} };
            COBRA_TRACE("Verifier", "FullWidthCheck: passed={}", result.passed);
            return result;
        }

        auto expr_constants = BuildExprDerivedProbes(&original, &simplified, bitwidth);
        std::vector< uint64_t > orig_inputs(original_num_vars);
        std::vector< uint64_t > simp_inputs(kSimpNumVars);
        std::vector< uint64_t > original_stack(original_eval.stack_size);
        std::vector< uint64_t > simplified_stack(simplified_eval.stack_size);

        std::vector< uint64_t > failing_input;
        const bool passed = ForEachFullWidthProbe(
            original_num_vars, bitwidth, num_samples, orig_inputs, expr_constants,
            [&](size_t) -> bool {
                for (uint32_t v = 0; v < kSimpNumVars; ++v) {
                    const uint32_t kOrigIdx = var_map.empty() ? v : var_map[v];
                    simp_inputs[v]          = orig_inputs[kOrigIdx];
                }
                if (EvalCompiledExpr(original_eval, orig_inputs, original_stack)
                    != EvalCompiledExpr(simplified_eval, simp_inputs, simplified_stack))
                {
                    failing_input = orig_inputs;
                    return false;
                }
                return true;
            }
        );

        if (!passed) {
            auto result =
                CheckResult{ .passed = false, .failing_input = std::move(failing_input) };
            COBRA_TRACE("Verifier", "FullWidthCheck: passed={}", result.passed);
            return result;
        }
        auto result = CheckResult{ .passed = true, .failing_input = {} };
        COBRA_TRACE("Verifier", "FullWidthCheck: passed={}", result.passed);
        return result;
    }

    CheckResult FullWidthCheckEval(
        const Evaluator &eval_original, uint32_t num_vars, const Expr &simplified,
        uint32_t bitwidth, uint32_t num_samples
    ) {
        COBRA_ZONE_N("FullWidthCheckEval");
        COBRA_TRACE(
            "Verifier", "FullWidthCheckEval: vars={} bitwidth={} samples={}", num_vars,
            bitwidth, num_samples
        );
        const uint64_t kMask       = Bitmask(bitwidth);
        const auto simplified_eval = [&]() {
            COBRA_ZONE_N("FullWidthCheckEval.compile");
            return CompileExpr(simplified, bitwidth);
        }();

        if ((eval_original.HasCompiledExpr() && eval_original.InputArity() > num_vars)
            || simplified_eval.arity > num_vars)
        {
            auto result = CheckResult{ .passed = false, .failing_input = {} };
            COBRA_TRACE("Verifier", "FullWidthCheckEval: passed={}", result.passed);
            return result;
        }

        auto expr_constants = BuildExprDerivedProbes(nullptr, &simplified, bitwidth);
        std::vector< uint64_t > inputs(num_vars);
        std::vector< uint64_t > simplified_stack(simplified_eval.stack_size);
        EvaluatorWorkspace original_workspace;

        std::vector< uint64_t > failing_input;
        const bool passed = ForEachFullWidthProbe(
            num_vars, bitwidth, num_samples, inputs, expr_constants, [&](size_t) -> bool {
                const uint64_t original_val = eval_original.HasCompiledExpr()
                    ? (eval_original.EvaluateWithWorkspace(inputs, original_workspace) & kMask)
                    : (eval_original(inputs) & kMask);
                const uint64_t simplified_val =
                    EvalCompiledExpr(simplified_eval, inputs, simplified_stack);
                if (original_val != simplified_val) {
                    failing_input = inputs;
                    return false;
                }
                return true;
            }
        );

        if (!passed) {
            auto result =
                CheckResult{ .passed = false, .failing_input = std::move(failing_input) };
            COBRA_TRACE("Verifier", "FullWidthCheckEval: passed={}", result.passed);
            return result;
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
        COBRA_ZONE_N("SignatureCheck");
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
