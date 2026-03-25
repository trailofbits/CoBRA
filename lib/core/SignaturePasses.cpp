#include "SignaturePasses.h"
#include "CompetitionGroup.h"
#include "ContinuationTypes.h"
#include "OrchestratorPasses.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"

#include <cassert>

namespace cobra {

    Result< PassResult > RunResolveCompetition(const WorkItem &item, OrchestratorContext &ctx) {
        if (!std::holds_alternative< CompetitionResolvedPayload >(item.payload)) {
            return Ok(PassResult{ .decision = PassDecision::kNotApplicable });
        }

        const auto &resolved = std::get< CompetitionResolvedPayload >(item.payload);
        auto group_it        = ctx.competition_groups.find(resolved.group_id);
        assert(group_it != ctx.competition_groups.end());
        auto &group = group_it->second;

        // Determine which continuation variant is active.
        // Default to monostate when no continuation is set.
        static const ContinuationData kDefaultCont{ std::monostate{} };
        const ContinuationData &cont =
            group.continuation.has_value() ? *group.continuation : kDefaultCont;

        PassResult result = std::visit(
            [&](const auto &c) -> PassResult {
                using T = std::decay_t< decltype(c) >;
                if constexpr (std::is_same_v< T, std::monostate >) {
                    // Top-level resolution: emit winner or blocked.
                    if (group.best.has_value()) {
                        auto &winner = *group.best;
                        WorkItem cand_item;
                        cand_item.payload = CandidatePayload{
                            .expr                              = CloneExpr(*winner.expr),
                            .real_vars                         = winner.real_vars,
                            .cost                              = winner.cost,
                            .producing_pass                    = winner.source_pass,
                            .needs_original_space_verification = true,
                        };
                        cand_item.features              = item.features;
                        cand_item.metadata              = item.metadata;
                        cand_item.metadata.verification = winner.verification;
                        cand_item.depth                 = item.depth;
                        cand_item.rewrite_gen           = item.rewrite_gen;
                        cand_item.attempted_mask        = item.attempted_mask;
                        cand_item.history               = item.history;

                        PassResult pr;
                        pr.decision    = PassDecision::kSolvedCandidate;
                        pr.disposition = ItemDisposition::kConsumeCurrent;
                        pr.next.push_back(std::move(cand_item));
                        return pr;
                    }

                    // No winner: blocked with accumulated failures.
                    ReasonDetail reason;
                    if (!group.technique_failures.empty()) {
                        reason = group.technique_failures.front();
                        for (size_t i = 1; i < group.technique_failures.size(); ++i) {
                            reason.causes.push_back(group.technique_failures[i].top);
                        }
                    } else {
                        reason.top.code = {
                            ReasonCategory::kSearchExhausted,
                            ReasonDomain::kOrchestrator,
                        };
                        reason.top.message = "Competition group resolved with no winner";
                    }
                    return PassResult{
                        .decision    = PassDecision::kBlocked,
                        .disposition = ItemDisposition::kConsumeCurrent,
                        .reason      = std::move(reason),
                    };
                } else {
                    // Non-monostate continuations are stubs for now.
                    return PassResult{
                        .decision    = PassDecision::kBlocked,
                        .disposition = ItemDisposition::kConsumeCurrent,
                        .reason =
                            ReasonDetail{
                                .top = {
                                    .code = {
                                        ReasonCategory::kInapplicable,
                                        ReasonDomain::kOrchestrator,
                                    },
                                    .message =
                                        "Continuation type not yet "
                                        "implemented",
                                },
                            },
                    };
                }
            },
            cont
        );

        // Clean up group from registry after resolution.
        ctx.competition_groups.erase(group_it);

        return Ok(std::move(result));
    }

} // namespace cobra
