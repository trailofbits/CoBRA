#pragma once

#include "ContinuationTypes.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/PassContract.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace cobra {

    using GroupId = uint32_t;

    // Forward declare — full definition in OrchestratorPasses.h
    enum class PassId : uint8_t;

    struct CandidateRecord
    {
        std::unique_ptr< Expr > expr;
        ExprCost cost;
        VerificationState verification = VerificationState::kUnverified;
        std::vector< std::string > real_vars;
        PassId source_pass;
        bool needs_original_space_verification = true;
        std::vector< uint64_t > sig_vector;
    };

    struct CompetitionGroup
    {
        uint32_t open_handles = 0;
        std::optional< CandidateRecord > best;
        std::optional< ExprCost > baseline_cost;
        std::optional< ContinuationData > continuation;
        std::vector< ReasonDetail > technique_failures;
    };

    // ---------------------------------------------------------------
    // Competition group lifecycle helpers
    // ---------------------------------------------------------------

    // Allocates a new group with open_handles = 1 and returns its id.
    GroupId CreateGroup(
        std::unordered_map< GroupId, CompetitionGroup > &groups, GroupId &next_id,
        std::optional< ExprCost > baseline_cost = std::nullopt
    );

    // Submits a candidate to a group. Accepted only if it beats
    // the baseline and any current best.
    bool SubmitCandidate(
        std::unordered_map< GroupId, CompetitionGroup > &groups, GroupId group_id,
        CandidateRecord record
    );

    // Increments a group's open_handles.
    void
    AcquireHandle(std::unordered_map< GroupId, CompetitionGroup > &groups, GroupId group_id);

} // namespace cobra
