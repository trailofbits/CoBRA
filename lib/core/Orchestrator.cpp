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

    namespace {
        uint64_t HashExpr(const Expr &e) {
            uint64_t h = 14695981039346656037ULL;
            auto mix   = [&h](uint64_t val) {
                h ^= val;
                h *= 1099511628211ULL;
            };
            mix(static_cast< uint64_t >(e.kind));
            mix(e.constant_val);
            mix(e.var_index);
            for (const auto &child : e.children) { mix(HashExpr(*child)); }
            return h;
        }
    } // namespace

    StateFingerprint
    ComputeFingerprint(const WorkItem &item, uint32_t bitwidth, bool normalize_stage_cursor) {
        StateFingerprint fp;
        fp.kind         = GetStateKind(item.payload);
        fp.bitwidth     = bitwidth;
        fp.provenance   = item.features.provenance;
        fp.stage_cursor = normalize_stage_cursor ? 0 : item.stage_cursor;

        std::visit(
            [&fp](const auto &payload) {
                using T = std::decay_t< decltype(payload) >;
                if constexpr (std::is_same_v< T, AstPayload >) {
                    fp.payload_hash = HashExpr(*payload.expr);
                    fp.vars         = {};
                } else if constexpr (std::is_same_v< T, SignatureStatePayload >) {
                    uint64_t h = 14695981039346656037ULL;
                    for (uint64_t v : payload.sig) {
                        h ^= v;
                        h *= 1099511628211ULL;
                    }
                    fp.payload_hash = h;
                    fp.vars         = payload.real_vars;
                } else {
                    // CandidatePayload
                    uint64_t h  = HashExpr(*payload.expr);
                    h          ^= payload.needs_original_space_verification ? 0x1ULL : 0x0ULL;
                    fp.payload_hash = h;
                    fp.vars         = payload.real_vars;
                }
            },
            item.payload
        );

        return fp;
    }

    bool UnsupportedRankBetter(const UnsupportedCandidate &a, const UnsupportedCandidate &b) {
        // 1. Candidates (verification-failed) rank highest
        if (a.is_candidate_state != b.is_candidate_state) { return a.is_candidate_state; }
        // 2. Deeper depth
        if (a.depth != b.depth) { return a.depth > b.depth; }
        // 3. More rewrites
        if (a.rewrite_gen != b.rewrite_gen) { return a.rewrite_gen > b.rewrite_gen; }
        // 4. More passes attempted
        if (a.history_size != b.history_size) { return a.history_size > b.history_size; }
        // 5. Last PassId enum order
        if (a.last_pass != b.last_pass) { return a.last_pass > b.last_pass; }
        return false;
    }

} // namespace cobra
