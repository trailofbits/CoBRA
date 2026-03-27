#include "SemilinearPasses.h"
#include "OrchestratorPasses.h"
#include "cobra/core/AtomSimplifier.h"
#include "cobra/core/BitPartitioner.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/MaskedAtomReconstructor.h"
#include "cobra/core/SelfCheck.h"
#include "cobra/core/SemilinearNormalizer.h"
#include "cobra/core/SemilinearSignature.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/StructureRecovery.h"
#include "cobra/core/TermRefiner.h"
#include "cobra/core/Trace.h"

namespace cobra {

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

    // Rewrite k + k*(c^x) -> (-k)*(~c ^ x).
    // The semilinear XOR recovery produces a negated coefficient
    // and an additive constant that cancel via this identity.
    std::unique_ptr< Expr >
    SimplifyXorConstant(std::unique_ptr< Expr > expr, uint32_t bitwidth) {
        if (expr->kind != Expr::Kind::kAdd) { return expr; }
        if (expr->children.size() != 2) { return expr; }

        // Match Add(Constant(k), Mul(Constant(k), XOR(Constant(c), ...)))
        // or   Add(Mul(Constant(k), XOR(Constant(c), ...)), Constant(k))
        const Expr *const_node = nullptr;
        const Expr *mul_node   = nullptr;
        int const_idx          = -1;

        for (int i = 0; i < 2; ++i) {
            if (expr->children[i]->kind == Expr::Kind::kConstant
                && expr->children[1 - i]->kind == Expr::Kind::kMul)
            {
                const_node = expr->children[i].get();
                mul_node   = expr->children[1 - i].get();
                const_idx  = i;
                break;
            }
        }
        (void) const_idx;
        if (const_node == nullptr) { return expr; }

        // Mul must have Constant(k) as first child and XOR as second.
        if (mul_node->children.size() != 2) { return expr; }
        const auto &mul_lhs = *mul_node->children[0];
        const auto &mul_rhs = *mul_node->children[1];
        if (mul_lhs.kind != Expr::Kind::kConstant) { return expr; }
        if (mul_rhs.kind != Expr::Kind::kXor) { return expr; }

        // Constants must match: k == k
        if (const_node->constant_val != mul_lhs.constant_val) { return expr; }

        // XOR must have a constant child.
        const auto &xor_node = mul_rhs;
        if (xor_node.children.size() != 2) { return expr; }

        int xor_const_idx = -1;
        if (xor_node.children[0]->kind == Expr::Kind::kConstant) {
            xor_const_idx = 0;
        } else if (xor_node.children[1]->kind == Expr::Kind::kConstant) {
            xor_const_idx = 1;
        }
        if (xor_const_idx < 0) { return expr; }

        // k + k*(c^x) = (-k)*(~c^x)
        const uint64_t kMask = Bitmask(bitwidth);
        const uint64_t kK    = const_node->constant_val;
        const uint64_t kNegK = ModNeg(kK, bitwidth);
        const uint64_t kC    = xor_node.children[xor_const_idx]->constant_val;
        const uint64_t kNotC = (~kC) & kMask;

        auto var_child = CloneExpr(*xor_node.children[1 - xor_const_idx]);
        auto new_xor   = Expr::BitwiseXor(Expr::Constant(kNotC), std::move(var_child));
        return ApplyCoefficient(std::move(new_xor), kNegK, bitwidth);
    }

    Result< PassResult >
    RunSemilinearNormalize(const WorkItem &item, OrchestratorContext &ctx) {
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
        simplified      = SimplifyXorConstant(std::move(simplified), ctx.bitwidth);

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
