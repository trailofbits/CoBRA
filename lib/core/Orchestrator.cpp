#include "Orchestrator.h"

namespace cobra {

    StateKind GetStateKind(const StateData &data) {
        return std::visit(
            []< typename T >(const T &) -> StateKind {
                if constexpr (std::is_same_v< T, AstPayload >) {
                    return StateKind::kFoldedAst;
                } else if constexpr (std::is_same_v< T, SignatureStatePayload >) {
                    return StateKind::kSignatureState;
                } else {
                    return StateKind::kCandidateExpr;
                }
            },
            data
        );
    }

    StateFingerprint ComputeFingerprint(
        const WorkItem & /*item*/, uint32_t /*bitwidth*/, bool /*normalize_stage_cursor*/
    ) {
        // TODO: implement structural hashing
        return {};
    }

    bool UnsupportedRankBetter(
        const UnsupportedCandidate & /*a*/, const UnsupportedCandidate & /*b*/
    ) {
        // TODO: implement deterministic ordering
        return false;
    }

} // namespace cobra
