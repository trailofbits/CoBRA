#include "OrchestratorPasses.h"
#include "SimplifierInternal.h"

namespace cobra {

    Result< PassResult > RunVerifyCandidate(const WorkItem &item, OrchestratorContext &ctx) {
        if (!std::holds_alternative< CandidatePayload >(item.payload)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        const auto &cand = std::get< CandidatePayload >(item.payload);
        if (!cand.needs_original_space_verification) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        if (!ctx.evaluator) {
            return Ok(
                PassResult{
                    .decision = PassDecision::kBlocked,
                    .reason =
                        ReasonDetail{
                                     .top = { .code    = { ReasonCategory::kGuardFailed,
                                                  ReasonDomain::kVerifier },
                                     .message = "Verification requires evaluator" },
                                     },
            }
            );
        }

        auto check = internal::VerifyInOriginalSpace(
            *ctx.evaluator, ctx.original_vars, cand.real_vars, *cand.expr, ctx.bitwidth
        );

        if (check.passed) {
            WorkItem verified_item;
            verified_item.payload = CandidatePayload{
                .expr                              = CloneExpr(*cand.expr),
                .real_vars                         = cand.real_vars,
                .cost                              = cand.cost,
                .producing_pass                    = cand.producing_pass,
                .needs_original_space_verification = false,
            };
            verified_item.features              = item.features;
            verified_item.metadata              = item.metadata;
            verified_item.metadata.verification = VerificationState::kVerified;
            verified_item.depth                 = item.depth;
            verified_item.rewrite_gen           = item.rewrite_gen;
            verified_item.attempted_mask        = item.attempted_mask;
            verified_item.history               = item.history;

            PassResult result;
            result.decision    = PassDecision::kAdvance;
            result.disposition = ItemDisposition::kConsumeCurrent;
            result.next.push_back(std::move(verified_item));
            return Ok(std::move(result));
        }

        PassResult result;
        result.decision    = PassDecision::kNoProgress;
        result.disposition = ItemDisposition::kRetainCurrent;
        result.reason      = ReasonDetail{
            .top = { .code    = { ReasonCategory::kVerifyFailed, ReasonDomain::kOrchestrator },
                    .message = "Full-width verification failed" },
        };

        return Ok(std::move(result));
    }

} // namespace cobra
