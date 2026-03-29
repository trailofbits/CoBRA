#pragma once

#include "cobra/core/CompiledExpr.h"
#include "cobra/core/Profile.h"

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace cobra {

    enum class EvaluatorTraceKind {
        kNone,
        kRoot,
        kMappedGlobal,
        kMappedOverride,
        kRemainder,
        kLiftedOuter,
        kCliOriginalAst,
    };

    struct EvaluatorWorkspace
    {
        std::vector< uint64_t > remapped_inputs;
        std::vector< uint64_t > stack;
    };

    class Evaluator
    {
      public:
        using Function = std::function< uint64_t(const std::vector< uint64_t > &) >;

        Evaluator()                                 = default;
        Evaluator(const Evaluator &)                = default;
        Evaluator(Evaluator &&) noexcept            = default;
        Evaluator &operator=(const Evaluator &)     = default;
        Evaluator &operator=(Evaluator &&) noexcept = default;

        explicit Evaluator(
            Function fn, EvaluatorTraceKind trace_kind = EvaluatorTraceKind::kNone
        )
            : fn_(std::move(fn)), trace_kind_(trace_kind) {}

        template< typename F >
            requires (
                !std::same_as< std::remove_cvref_t< F >, Evaluator >
                && std::constructible_from< Function, F >
            )
        Evaluator(F &&fn) // NOLINT(google-explicit-constructor)
            : Evaluator(Function(std::forward< F >(fn))) {}

        static Evaluator FromExpr(
            const Expr &expr, uint32_t bitwidth,
            EvaluatorTraceKind trace_kind = EvaluatorTraceKind::kNone
        ) {
            return FromCompiled(
                std::make_shared< CompiledExpr >(CompileExpr(expr, bitwidth)), trace_kind
            );
        }

        static Evaluator FromCompiled(
            std::shared_ptr< const CompiledExpr > compiled,
            EvaluatorTraceKind trace_kind = EvaluatorTraceKind::kNone
        ) {
            Evaluator eval;
            eval.compiled_    = std::move(compiled);
            eval.input_arity_ = eval.compiled_ ? eval.compiled_->arity : 0;
            eval.trace_kind_  = trace_kind;
            return eval;
        }

        Evaluator &operator=(Function fn) {
            fn_ = std::move(fn);
            compiled_.reset();
            input_map_.clear();
            input_arity_ = 0;
            trace_kind_  = EvaluatorTraceKind::kNone;
            return *this;
        }

        template< typename F >
            requires (
                !std::same_as< std::remove_cvref_t< F >, Evaluator >
                && std::constructible_from< Function, F >
            )
        Evaluator &operator=(F &&fn) {
            return *this = Function(std::forward< F >(fn));
        }

        uint64_t operator()(const std::vector< uint64_t > &vals) const {
            return InvokeTraced([&]() { return InvokeUntraced(vals, nullptr); });
        }

        explicit operator bool() const {
            return static_cast< bool >(compiled_) || static_cast< bool >(fn_);
        }

        Evaluator WithTrace(EvaluatorTraceKind trace_kind) const {
            Evaluator eval   = *this;
            eval.trace_kind_ = trace_kind;
            return eval;
        }

        Evaluator Remap(
            const std::vector< uint32_t > &idx_map, uint32_t source_arity,
            EvaluatorTraceKind trace_kind
        ) const {
            if (compiled_) {
                Evaluator eval = *this;
                std::vector< uint32_t > composed;
                composed.reserve(idx_map.size());
                if (input_map_.empty()) {
                    composed = idx_map;
                } else {
                    for (uint32_t idx : idx_map) { composed.push_back(input_map_[idx]); }
                }
                eval.input_map_   = std::move(composed);
                eval.input_arity_ = static_cast< uint32_t >(idx_map.size());
                eval.trace_kind_  = trace_kind;
                return eval;
            }

            Evaluator base    = *this;
            // Buffer must be large enough for the max index in idx_map
            // (which may exceed source_arity when lifted variables are
            // remapped through the elimination pipeline).
            uint32_t buf_size = source_arity;
            for (uint32_t idx : idx_map) { buf_size = std::max(buf_size, idx + 1); }
            return Evaluator(
                [base, idx_map, buf_size, original_vals = std::vector< uint64_t >(buf_size, 0)](
                    const std::vector< uint64_t > &reduced_vals
                ) mutable -> uint64_t {
                    for (size_t i = 0; i < idx_map.size(); ++i) {
                        original_vals[idx_map[i]] = reduced_vals[i];
                    }
                    const uint64_t result = base(original_vals);
                    for (size_t i = 0; i < idx_map.size(); ++i) {
                        original_vals[idx_map[i]] = 0;
                    }
                    return result;
                },
                trace_kind
            );
        }

        bool HasCompiledExpr() const { return static_cast< bool >(compiled_); }

        uint64_t EvaluateWithWorkspace(
            const std::vector< uint64_t > &vals, EvaluatorWorkspace &workspace
        ) const {
            if (compiled_) { return InvokeUntraced(vals, &workspace); }
            return (*this)(vals);
        }

        size_t RequiredStackSize() const { return compiled_ ? compiled_->stack_size : 0; }

        uint32_t InputArity() const { return input_arity_; }

      private:
        template< typename Fn >
        uint64_t InvokeTraced(Fn &&invoke) const {
            switch (trace_kind_) {
                case EvaluatorTraceKind::kRoot: {
                    COBRA_ZONE_N("Evaluator.root");
                    return invoke();
                }
                case EvaluatorTraceKind::kMappedGlobal: {
                    COBRA_ZONE_N("Evaluator.mapped_global");
                    return invoke();
                }
                case EvaluatorTraceKind::kMappedOverride: {
                    COBRA_ZONE_N("Evaluator.mapped_override");
                    return invoke();
                }
                case EvaluatorTraceKind::kRemainder: {
                    COBRA_ZONE_N("Evaluator.remainder");
                    return invoke();
                }
                case EvaluatorTraceKind::kLiftedOuter: {
                    COBRA_ZONE_N("Evaluator.lifted_outer");
                    return invoke();
                }
                case EvaluatorTraceKind::kCliOriginalAst: {
                    COBRA_ZONE_N("Evaluator.cli_original_ast");
                    return invoke();
                }
                case EvaluatorTraceKind::kNone:
                    return invoke();
            }
            return invoke();
        }

        uint64_t InvokeUntraced(
            const std::vector< uint64_t > &vals, EvaluatorWorkspace *workspace
        ) const {
            if (!compiled_) { return fn_(vals); }

            EvaluatorWorkspace local_workspace;
            // NOLINTNEXTLINE(readability-identifier-naming)
            EvaluatorWorkspace &wksp              = workspace ? *workspace : local_workspace;
            const std::vector< uint64_t > *inputs = &vals;

            if (!input_map_.empty()) {
                size_t remapped_arity = compiled_->arity;
                for (uint32_t idx : input_map_) {
                    remapped_arity = std::max(remapped_arity, static_cast< size_t >(idx) + 1);
                }
                if (wksp.remapped_inputs.size() < remapped_arity) {
                    wksp.remapped_inputs.resize(remapped_arity);
                }
                std::fill(wksp.remapped_inputs.begin(), wksp.remapped_inputs.end(), 0);
                for (size_t i = 0; i < input_map_.size(); ++i) {
                    wksp.remapped_inputs[input_map_[i]] = vals[i];
                }
                inputs = &wksp.remapped_inputs;
            }

            return EvalCompiledExpr(*compiled_, *inputs, wksp.stack);
        }

        Function fn_{};
        std::shared_ptr< const CompiledExpr > compiled_{};
        std::vector< uint32_t > input_map_{};
        uint32_t input_arity_          = 0;
        EvaluatorTraceKind trace_kind_ = EvaluatorTraceKind::kNone;
    };

} // namespace cobra
