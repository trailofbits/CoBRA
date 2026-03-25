#include "CompetitionGroup.h"
#include "Orchestrator.h"

#include <cassert>

namespace cobra {

    GroupId CreateGroup(
        std::unordered_map< GroupId, CompetitionGroup > &groups, GroupId &next_id,
        std::optional< ExprCost > baseline_cost
    ) {
        GroupId id = next_id++;
        CompetitionGroup group;
        group.open_handles  = 1;
        group.baseline_cost = std::move(baseline_cost);
        groups.emplace(id, std::move(group));
        return id;
    }

    bool SubmitCandidate(
        std::unordered_map< GroupId, CompetitionGroup > &groups, GroupId group_id,
        CandidateRecord record
    ) {
        auto it = groups.find(group_id);
        assert(it != groups.end());
        auto &group = it->second;

        if (group.baseline_cost && !IsBetter(record.cost, *group.baseline_cost)) {
            return false;
        }

        if (!group.best) {
            group.best.emplace(std::move(record));
            return true;
        }

        if (IsBetter(record.cost, group.best->cost)) {
            group.best.emplace(std::move(record));
            return true;
        }

        return false;
    }

    void
    AcquireHandle(std::unordered_map< GroupId, CompetitionGroup > &groups, GroupId group_id) {
        auto it = groups.find(group_id);
        assert(it != groups.end());
        ++it->second.open_handles;
    }

    std::optional< WorkItem >
    ReleaseHandle(std::unordered_map< GroupId, CompetitionGroup > &groups, GroupId group_id) {
        auto it = groups.find(group_id);
        assert(it != groups.end());
        auto &group = it->second;
        assert(group.open_handles > 0);

        --group.open_handles;
        if (group.open_handles > 0) { return std::nullopt; }

        WorkItem resolved;
        resolved.payload = CompetitionResolvedPayload{ .group_id = group_id };
        return resolved;
    }

} // namespace cobra
