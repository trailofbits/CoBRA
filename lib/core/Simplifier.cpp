#include "cobra/core/Simplifier.h"
#include "SemilinearPasses.h"
#include "SimplifierInternal.h"
#include "cobra/core/AtomSimplifier.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/BitPartitioner.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/MaskedAtomReconstructor.h"
#include "cobra/core/PatternMatcher.h"
#include "cobra/core/Result.h"
#include "cobra/core/SelfCheck.h"
#include "cobra/core/SemilinearIR.h"
#include "cobra/core/SemilinearNormalizer.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureSimplifier.h"
#include "cobra/core/StructureRecovery.h"
#include "cobra/core/TermRefiner.h"
#include "cobra/core/Trace.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cobra {

    namespace internal {

        CheckResult VerifyInOriginalSpace(
            const Evaluator &eval, const std::vector< std::string > &all_vars,
            const std::vector< std::string > &real_vars, const Expr &reduced_expr,
            uint32_t bitwidth
        ) {
            const auto kAllCount = static_cast< uint32_t >(all_vars.size());
            if (real_vars.empty() || real_vars.size() == all_vars.size()) {
                return FullWidthCheckEval(eval, kAllCount, reduced_expr, bitwidth);
            }
            std::vector< uint32_t > idx_map;
            idx_map.reserve(real_vars.size());
            for (const auto &rv : real_vars) {
                for (uint32_t j = 0; j < all_vars.size(); ++j) {
                    if (all_vars[j] == rv) {
                        idx_map.push_back(j);
                        break;
                    }
                }
            }
            auto remapped = CloneExpr(reduced_expr);
            RemapVarIndices(*remapped, idx_map);
            return FullWidthCheckEval(eval, kAllCount, *remapped, bitwidth);
        }

        bool IsPurelyArithmetic(const Expr &e) {
            switch (e.kind) {
                case Expr::Kind::kConstant:
                case Expr::Kind::kVariable:
                case Expr::Kind::kAdd:
                case Expr::Kind::kMul:
                case Expr::Kind::kNeg:
                    break;
                default:
                    return false;
            }
            return std::ranges::all_of(e.children, [](const auto &child) {
                return IsPurelyArithmetic(*child);
            });
        }

        bool HasNotOverArith(const Expr &e) {
            if (e.kind == Expr::Kind::kNot && !e.children.empty()
                && IsPurelyArithmetic(*e.children[0]))
            {
                return true;
            }
            return std::ranges::any_of(e.children, [](const auto &child) {
                return HasNotOverArith(*child);
            });
        }

        std::unique_ptr< Expr > LowerNotOverArith(
            std::unique_ptr< Expr > e, uint32_t bitwidth
        ) { // NOLINT(readability-identifier-naming)
            for (auto &child : e->children) {
                child = LowerNotOverArith(std::move(child), bitwidth);
            }

            if (e->kind == Expr::Kind::kNot && e->children.size() == 1
                && IsPurelyArithmetic(*e->children[0]))
            {
                const uint64_t mask = (bitwidth == 64) ? UINT64_MAX : ((1ULL << bitwidth) - 1);
                return Expr::Add(Expr::Negate(std::move(e->children[0])), Expr::Constant(mask));
            }

            return e;
        }

        PassOutcome TrySemilinearPipeline(
            const Expr &ast, const std::vector< std::string > &vars, const Options &opts
        ) {
            const auto kNumVars = static_cast< uint32_t >(vars.size());
            COBRA_TRACE(
                "Simplifier", "TrySemilinearPipeline: vars={} bitwidth={}", vars.size(),
                opts.bitwidth
            );

            if (kNumVars > opts.max_vars) {
                return PassOutcome::Inapplicable(
                    ReasonDetail{
                        .top = { .code    = { ReasonCategory::kGuardFailed,
                                              ReasonDomain::kSemilinear,
                                              semilinear_pass::kTooManyVars },
                                .message = "too many variables for semilinear pipeline" }
                }
                );
            }

            auto ir_result = NormalizeToSemilinear(ast, vars, opts.bitwidth);
            if (!ir_result.has_value()) {
                COBRA_TRACE(
                    "Simplifier", "TrySemilinearPipeline: normalization failed: {}",
                    ir_result.error().message
                );
                return PassOutcome::Blocked(
                    ReasonDetail{
                        .top = { .code    = { ReasonCategory::kRepresentationGap,
                                              ReasonDomain::kSemilinear,
                                              semilinear_pass::kNormalizeFailed },
                                .message = "semilinear normalization failed: "
                                     + ir_result.error().message }
                }
                );
            }
            auto &ir = ir_result.value();

            SimplifyStructure(ir);

            auto plain = ReconstructMaskedAtoms(ir, {});
            auto check = SelfCheckSemilinear(ir, *plain, vars, opts.bitwidth);
            if (!check.passed) {
                COBRA_TRACE(
                    "Simplifier", "TrySemilinearPipeline: self-check failed: {}",
                    check.mismatch_detail
                );
                return PassOutcome::Blocked(
                    ReasonDetail{
                        .top = { .code    = { ReasonCategory::kInternalInvariant,
                                              ReasonDomain::kSemilinear,
                                              semilinear_pass::kSelfCheckFailed },
                                .message = "semilinear self-check failed" }
                }
                );
            }

            if (FlattenComplexAtoms(ir)) { CoalesceTerms(ir); }
            RecoverStructure(ir);
            RefineTerms(ir);
            CoalesceTerms(ir);

            if (opts.evaluator) {
                auto probe_expr = ReconstructMaskedAtoms(ir, {});
                auto probe      = FullWidthCheckEval(
                    opts.evaluator, kNumVars, *probe_expr, opts.bitwidth, 16
                );
                if (!probe.passed) {
                    COBRA_TRACE(
                        "Simplifier", "TrySemilinearPipeline: post-rewrite probe failed"
                    );
                    return PassOutcome::VerifyFailed(
                        std::move(probe_expr), vars,
                        ReasonDetail{
                            .top = { .code    = { ReasonCategory::kVerifyFailed,
                                                  ReasonDomain::kSemilinear,
                                                  semilinear_pass::kPostRewriteProbe },
                                    .message = "post-rewrite probe verification failed" }
                    }
                    );
                }
            }

            CompactAtomTable(ir);
            auto partitions = ComputePartitions(ir);
            auto simplified = ReconstructMaskedAtoms(ir, partitions);
            simplified      = SimplifyXorConstant(std::move(simplified), opts.bitwidth);

            if (opts.evaluator) {
                auto final_check =
                    FullWidthCheckEval(opts.evaluator, kNumVars, *simplified, opts.bitwidth);
                if (!final_check.passed) {
                    COBRA_TRACE(
                        "Simplifier", "TrySemilinearPipeline: final verification failed"
                    );
                    return PassOutcome::VerifyFailed(
                        std::move(simplified), vars,
                        ReasonDetail{
                            .top = { .code    = { ReasonCategory::kVerifyFailed,
                                                  ReasonDomain::kSemilinear,
                                                  semilinear_pass::kFinalVerifyFail },
                                    .message = "final full-width verification failed" }
                    }
                    );
                }
            }

            COBRA_TRACE("Simplifier", "TrySemilinearPipeline: success");
            return PassOutcome::Success(
                std::move(simplified), vars, VerificationState::kVerified
            );
        }

    } // namespace internal

    Result< PassOutcome > RunSupportedPass(
        const std::vector< uint64_t > &sig, const std::vector< std::string > &vars,
        const Options &opts
    ) {
        const auto kNumVars = static_cast< uint32_t >(vars.size());
        COBRA_TRACE(
            "Simplifier", "RunSupportedPass: vars={} bitwidth={} max_vars={}", vars.size(),
            opts.bitwidth, opts.max_vars
        );
        COBRA_TRACE_SIG("Simplifier", "RunSupportedPass input sig", sig);

        // Step 1: Prune constants
        {
            auto pm = MatchPattern(sig, kNumVars, opts.bitwidth);
            if (pm && (*pm)->kind == Expr::Kind::kConstant) {
                COBRA_TRACE(
                    "Simplifier", "RunSupportedPass: constant match val={}", (*pm)->constant_val
                );
                auto outcome =
                    PassOutcome::Success(std::move(*pm), {}, VerificationState::kVerified);
                outcome.SetSigVector(sig);
                return Ok(std::move(outcome));
            }
        }

        // Step 2: Eliminate auxiliary variables
        auto elim                = EliminateAuxVars(sig, vars);
        const auto kRealVarCount = static_cast< uint32_t >(elim.real_vars.size());
        COBRA_TRACE(
            "Simplifier", "RunSupportedPass: after EliminateAuxVars real={} eliminated={}",
            elim.real_vars.size(), elim.spurious_vars.size()
        );

        if (kRealVarCount > opts.max_vars) {
            return Err< PassOutcome >(
                CobraError::kTooManyVariables,
                "Variable count after elimination (" + std::to_string(kRealVarCount)
                    + ") exceeds max_vars (" + std::to_string(opts.max_vars) + ")"
            );
        }

        // Build SignatureContext with mapped evaluator
        SignatureContext ctx; // NOLINT(misc-const-correctness)
        ctx.vars = elim.real_vars;

        ctx.original_indices = BuildVarSupport(vars, elim.real_vars);

        if (opts.evaluator) {
            if (elim.real_vars.size() == vars.size()) {
                ctx.eval = opts.evaluator;
            } else {
                ctx.eval = [eval = opts.evaluator, idx_map = ctx.original_indices,
                            original_vals = std::vector< uint64_t >(vars.size(), 0)](
                               const std::vector< uint64_t > &reduced_vals
                           ) mutable -> uint64_t {
                    for (size_t i = 0; i < idx_map.size(); ++i) {
                        original_vals[idx_map[i]] = reduced_vals[i];
                    }
                    uint64_t result = eval(original_vals);
                    for (size_t i = 0; i < idx_map.size(); ++i) {
                        original_vals[idx_map[i]] = 0;
                    }
                    return result;
                };
            }
        }

        // Delegate to SignatureSimplifier
        auto sub = SimplifyFromSignature(elim.reduced_sig, ctx, opts, 0);
        COBRA_TRACE(
            "Simplifier", "RunSupportedPass: SignatureSimplifier returned succeeded={}",
            sub.Succeeded()
        );

        if (sub.Succeeded()) {
            auto payload      = sub.TakePayload();
            auto verification = payload.verification == VerificationState::kVerified
                ? VerificationState::kVerified
                : VerificationState::kUnverified;
            auto outcome      = PassOutcome::Success(
                std::move(payload.expr), std::move(elim.real_vars), verification
            );
            outcome.SetSigVector(elim.reduced_sig);
            return Ok(std::move(outcome));
        }

        // Translate solver non-success into PassOutcome
        ReasonDetail reason = sub.Reason();
        if (sub.Kind() == OutcomeKind::kInapplicable) {
            return Ok(PassOutcome::Inapplicable(std::move(reason)));
        }
        if (sub.Kind() == OutcomeKind::kVerifyFailed) {
            auto payload = sub.TakePayload();
            return Ok(
                PassOutcome::VerifyFailed(
                    std::move(payload.expr), std::move(elim.real_vars), std::move(reason)
                )
            );
        }
        return Ok(PassOutcome::Blocked(std::move(reason)));
    }

} // namespace cobra
