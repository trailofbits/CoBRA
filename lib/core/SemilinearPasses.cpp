#include "SemilinearPasses.h"
#include "OrchestratorPasses.h"
#include "cobra/core/AtomSimplifier.h"
#include "cobra/core/BitPartitioner.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/MaskedAtomReconstructor.h"
#include "cobra/core/PatternMatcher.h"
#include "cobra/core/Profile.h"
#include "cobra/core/SelfCheck.h"
#include "cobra/core/SemilinearNormalizer.h"
#include "cobra/core/SemilinearSignature.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/StructureRecovery.h"
#include "cobra/core/TermRefiner.h"
#include "cobra/core/Trace.h"

namespace cobra {

    namespace {

        bool MatchAppliedCoefficient(
            const Expr &expr, uint64_t coeff, uint32_t bitwidth, const Expr *&term
        ) {
            if (coeff == 1) {
                term = &expr;
                return true;
            }

            if (coeff == Bitmask(bitwidth) && expr.kind == Expr::Kind::kNeg
                && expr.children.size() == 1)
            {
                term = expr.children[0].get();
                return true;
            }

            if (expr.kind != Expr::Kind::kMul || expr.children.size() != 2) { return false; }

            if (expr.children[0]->kind == Expr::Kind::kConstant
                && expr.children[0]->constant_val == coeff)
            {
                term = expr.children[1].get();
                return true;
            }
            if (expr.children[1]->kind == Expr::Kind::kConstant
                && expr.children[1]->constant_val == coeff)
            {
                term = expr.children[0].get();
                return true;
            }
            return false;
        }

        bool MatchScaledAddTerm(
            const Expr &expr, uint32_t bitwidth, const Expr *&term, uint64_t &coeff
        ) {
            if (expr.kind != Expr::Kind::kAdd || expr.children.size() != 2) { return false; }

            const Expr *const_node = nullptr;
            size_t const_idx       = 0;
            for (size_t i = 0; i < 2; ++i) {
                if (expr.children[i]->kind == Expr::Kind::kConstant) {
                    const_node = expr.children[i].get();
                    const_idx  = i;
                    break;
                }
            }
            if (const_node == nullptr) { return false; }

            coeff = const_node->constant_val;
            return MatchAppliedCoefficient(
                *expr.children[1 - const_idx], coeff, bitwidth, term
            );
        }

    } // namespace

    SemilinearIR CloneSemilinearIR(const SemilinearIR &src) {
        SemilinearIR dst;
        dst.constant = src.constant;
        dst.bitwidth = src.bitwidth;
        dst.terms    = src.terms;
        dst.atom_table.reserve(src.atom_table.size());
        for (const auto &info : src.atom_table) {
            AtomInfo clone;
            clone.atom_id         = info.atom_id;
            clone.key             = info.key;
            clone.structural_hash = info.structural_hash;
            clone.provenance      = info.provenance;
            clone.original_subtree =
                info.original_subtree ? CloneExpr(*info.original_subtree) : nullptr;
            dst.atom_table.push_back(std::move(clone));
        }
        return dst;
    }

    // Rewrite k + k*(c^x) -> (-k)*(~c ^ x), and more generally
    // k + k*x -> (-k)*~x. The XOR-specialized form keeps the recovered
    // atom in its cheaper XOR-with-constant representation.
    std::unique_ptr< Expr >
    CanonicalizeScaledBooleanSum(std::unique_ptr< Expr > expr, uint32_t bitwidth) {
        const Expr *term = nullptr;
        uint64_t coeff   = 0;
        if (!MatchScaledAddTerm(*expr, bitwidth, term, coeff) || coeff == 0) { return expr; }

        if (term->kind == Expr::Kind::kXor && term->children.size() == 2) {
            int xor_const_idx = -1;
            if (term->children[0]->kind == Expr::Kind::kConstant) {
                xor_const_idx = 0;
            } else if (term->children[1]->kind == Expr::Kind::kConstant) {
                xor_const_idx = 1;
            }
            if (xor_const_idx >= 0) {
                const uint64_t kMask = Bitmask(bitwidth);
                const uint64_t kNegK = ModNeg(coeff, bitwidth);
                const uint64_t kC =
                    term->children[static_cast< size_t >(xor_const_idx)]->constant_val;
                const uint64_t kNotC = (~kC) & kMask;

                auto var_child =
                    CloneExpr(*term->children[static_cast< size_t >(1 - xor_const_idx)]);
                auto new_xor = Expr::BitwiseXor(Expr::Constant(kNotC), std::move(var_child));
                return ApplyCoefficient(std::move(new_xor), kNegK, bitwidth);
            }
        }

        auto complemented = SimplifyAtom(Expr::BitwiseNot(CloneExpr(*term)), bitwidth);
        return ApplyCoefficient(std::move(complemented), ModNeg(coeff, bitwidth), bitwidth);
    }

    std::unique_ptr< Expr >
    NormalizeLateCandidateExpr(std::unique_ptr< Expr > expr, uint32_t bitwidth) {
        expr = CanonicalizeScaledBooleanSum(std::move(expr), bitwidth);
        return SimplifyPatternSubtrees(std::move(expr), bitwidth);
    }

    Result< PassResult >
    RunSemilinearNormalize(const WorkItem &item, OrchestratorContext &ctx) {
        COBRA_ZONE_N("RunSemilinearNormalize");
        if (!std::holds_alternative< AstPayload >(item.payload)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }
        const auto &ast = std::get< AstPayload >(item.payload);

        if (ast.provenance == Provenance::kLowered) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNotApplicable,
                    .disposition = ItemDisposition::kConsumeCurrent,
                }
            );
        }
        if (!ast.classification || ast.classification->semantic != SemanticClass::kSemilinear) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNotApplicable,
                    .disposition = ItemDisposition::kConsumeCurrent,
                }
            );
        }

        const auto &active_vars = ActiveAstVars(item, ctx);
        const auto &active_eval = ActiveAstEvaluator(item, ctx);
        const auto kNumVars     = static_cast< uint32_t >(active_vars.size());

        if (IsLinearShortcut(*ast.expr, kNumVars, ctx.bitwidth)) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNotApplicable,
                    .disposition = ItemDisposition::kConsumeCurrent,
                }
            );
        }

        if (kNumVars > ctx.opts.max_vars) {
            return Ok(
                PassResult{
                    .decision    = PassDecision::kNotApplicable,
                    .disposition = ItemDisposition::kConsumeCurrent,
                    .reason =
                        ReasonDetail{
                                     .top = { .code    = { ReasonCategory::kGuardFailed,
                                                  ReasonDomain::kSemilinear,
                                                  semilinear_pass::kTooManyVars },
                                     .message = "too many variables for semilinear" },
                                     },
            }
            );
        }

        auto ir_result = NormalizeToSemilinear(*ast.expr, active_vars, ctx.bitwidth);
        if (!ir_result.has_value()) {
            ReasonDetail reason{
                .top = { .code = { ReasonCategory::kRepresentationGap,
                                   ReasonDomain::kSemilinear,
                                   semilinear_pass::kNormalizeFailed },
                        .message =
                             "semilinear normalization failed: " + ir_result.error().message },
            };
            ctx.run_metadata.semilinear_failure = reason;
            return Ok(
                PassResult{
                    .decision    = PassDecision::kBlocked,
                    .disposition = ItemDisposition::kConsumeCurrent,
                    .reason      = std::move(reason),
                }
            );
        }

        WorkItem next;
        next.payload = NormalizedSemilinearPayload{
            .ctx =
                SemilinearContext{
                                  .ir        = std::move(ir_result.value()),
                                  .vars      = active_vars,
                                  .evaluator = active_eval,
                                  },
        };
        next.features       = item.features;
        next.metadata       = item.metadata;
        next.depth          = item.depth;
        next.rewrite_gen    = item.rewrite_gen;
        next.attempted_mask = item.attempted_mask;
        next.history        = item.history;
        next.group_id       = item.group_id;

        PassResult result;
        result.decision    = PassDecision::kAdvance;
        result.disposition = ItemDisposition::kConsumeCurrent;
        result.next.push_back(std::move(next));
        return Ok(std::move(result));
    }

    Result< PassResult > RunSemilinearCheck(const WorkItem &item, OrchestratorContext &ctx) {
        COBRA_ZONE_N("RunSemilinearCheck");
        if (!std::holds_alternative< NormalizedSemilinearPayload >(item.payload)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }
        const auto &payload = std::get< NormalizedSemilinearPayload >(item.payload);
        auto ir             = CloneSemilinearIR(payload.ctx.ir);
        const auto &vars    = payload.ctx.vars.empty() ? ctx.original_vars : payload.ctx.vars;

        SimplifyStructure(ir);

        auto plain = ReconstructMaskedAtoms(ir, {});
        auto check = SelfCheckSemilinear(ir, *plain, vars, ctx.bitwidth);
        if (!check.passed) {
            COBRA_TRACE(
                "Simplifier", "RunSemilinearCheck: self-check failed: {}", check.mismatch_detail
            );
            ReasonDetail reason{
                .top = { .code    = { ReasonCategory::kInternalInvariant,
                                      ReasonDomain::kSemilinear,
                                      semilinear_pass::kSelfCheckFailed },
                        .message = "semilinear self-check failed" },
            };
            ctx.run_metadata.semilinear_failure = reason;
            return Ok(
                PassResult{
                    .decision    = PassDecision::kBlocked,
                    .disposition = ItemDisposition::kConsumeCurrent,
                    .reason      = std::move(reason),
                }
            );
        }

        WorkItem next;
        next.payload = CheckedSemilinearPayload{
            .ctx =
                SemilinearContext{
                                  .ir        = std::move(ir),
                                  .vars      = vars,
                                  .evaluator = payload.ctx.evaluator,
                                  },
        };
        next.features       = item.features;
        next.metadata       = item.metadata;
        next.depth          = item.depth;
        next.rewrite_gen    = item.rewrite_gen;
        next.attempted_mask = item.attempted_mask;
        next.history        = item.history;
        next.group_id       = item.group_id;

        PassResult result;
        result.decision    = PassDecision::kAdvance;
        result.disposition = ItemDisposition::kConsumeCurrent;
        result.next.push_back(std::move(next));
        return Ok(std::move(result));
    }

    Result< PassResult > RunSemilinearRewrite(const WorkItem &item, OrchestratorContext &ctx) {
        COBRA_ZONE_N("RunSemilinearRewrite");
        if (!std::holds_alternative< CheckedSemilinearPayload >(item.payload)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }
        const auto &payload = std::get< CheckedSemilinearPayload >(item.payload);
        auto ir             = CloneSemilinearIR(payload.ctx.ir);
        const auto &vars    = payload.ctx.vars.empty() ? ctx.original_vars : payload.ctx.vars;
        const auto &local_eval =
            payload.ctx.evaluator.has_value() ? payload.ctx.evaluator : ctx.evaluator;

        if (FlattenComplexAtoms(ir)) { CoalesceTerms(ir); }
        RecoverStructure(ir);
        RefineTerms(ir);
        CoalesceTerms(ir);

        if (local_eval) {
            const auto kNumVars = static_cast< uint32_t >(vars.size());
            auto probe_expr     = ReconstructMaskedAtoms(ir, {});
            auto probe =
                FullWidthCheckEval(*local_eval, kNumVars, *probe_expr, ctx.bitwidth, 16);
            if (!probe.passed) {
                COBRA_TRACE("Simplifier", "RunSemilinearRewrite: post-rewrite probe failed");
                ReasonDetail reason{
                    .top = { .code = { ReasonCategory::kVerifyFailed, ReasonDomain::kSemilinear,
                                       semilinear_pass::kPostRewriteProbe },
                            .message = "post-rewrite probe verification failed" },
                };
                ctx.run_metadata.semilinear_failure = reason;
                return Ok(
                    PassResult{
                        .decision    = PassDecision::kBlocked,
                        .disposition = ItemDisposition::kConsumeCurrent,
                        .reason      = std::move(reason),
                    }
                );
            }
        }

        WorkItem next;
        next.payload = RewrittenSemilinearPayload{
            .ctx =
                SemilinearContext{
                                  .ir        = std::move(ir),
                                  .vars      = vars,
                                  .evaluator = local_eval,
                                  },
        };
        next.features       = item.features;
        next.metadata       = item.metadata;
        next.depth          = item.depth;
        next.rewrite_gen    = item.rewrite_gen;
        next.attempted_mask = item.attempted_mask;
        next.history        = item.history;
        next.group_id       = item.group_id;

        PassResult result;
        result.decision    = PassDecision::kAdvance;
        result.disposition = ItemDisposition::kConsumeCurrent;
        result.next.push_back(std::move(next));
        return Ok(std::move(result));
    }

    Result< PassResult >
    RunSemilinearReconstruct(const WorkItem &item, OrchestratorContext &ctx) {
        COBRA_ZONE_N("RunSemilinearReconstruct");
        if (!std::holds_alternative< RewrittenSemilinearPayload >(item.payload)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }
        const auto &payload = std::get< RewrittenSemilinearPayload >(item.payload);
        auto ir             = CloneSemilinearIR(payload.ctx.ir);
        const auto &vars    = payload.ctx.vars.empty() ? ctx.original_vars : payload.ctx.vars;
        const auto &local_eval =
            payload.ctx.evaluator.has_value() ? payload.ctx.evaluator : ctx.evaluator;

        CompactAtomTable(ir);
        auto partitions = ComputePartitions(ir);
        auto simplified = ReconstructMaskedAtoms(ir, partitions);
        simplified      = NormalizeLateCandidateExpr(std::move(simplified), ctx.bitwidth);

        const auto kNumVars = static_cast< uint32_t >(vars.size());
        auto verification   = VerificationState::kUnverified;

        if (local_eval) {
            auto final_check =
                FullWidthCheckEval(*local_eval, kNumVars, *simplified, ctx.bitwidth);
            if (!final_check.passed) {
                COBRA_TRACE(
                    "Simplifier", "RunSemilinearReconstruct: final verification failed"
                );
                ReasonDetail reason{
                    .top = { .code = { ReasonCategory::kVerifyFailed, ReasonDomain::kSemilinear,
                                       semilinear_pass::kFinalVerifyFail },
                            .message = "final full-width verification failed" },
                };
                ctx.run_metadata.semilinear_failure = reason;
                return Ok(
                    PassResult{
                        .decision    = PassDecision::kBlocked,
                        .disposition = ItemDisposition::kConsumeCurrent,
                        .reason      = std::move(reason),
                    }
                );
            }
            verification = VerificationState::kVerified;
        }

        auto cost_info = ComputeCost(*simplified);

        WorkItem cand_item;
        cand_item.payload = CandidatePayload{
            .expr                              = std::move(simplified),
            .real_vars                         = vars,
            .cost                              = cost_info.cost,
            .producing_pass                    = PassId::kSemilinearReconstruct,
            .needs_original_space_verification = false,
        };
        cand_item.features              = item.features;
        cand_item.metadata              = item.metadata;
        cand_item.metadata.verification = verification;
        cand_item.depth                 = item.depth;
        cand_item.rewrite_gen           = item.rewrite_gen;
        cand_item.attempted_mask        = item.attempted_mask;
        cand_item.history               = item.history;
        cand_item.group_id              = item.group_id;

        PassResult result;
        result.decision    = PassDecision::kSolvedCandidate;
        result.disposition = ItemDisposition::kConsumeCurrent;
        result.next.push_back(std::move(cand_item));
        return Ok(std::move(result));
    }

} // namespace cobra
