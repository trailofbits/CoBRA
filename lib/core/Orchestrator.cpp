#include "Orchestrator.h"
#include "OrchestratorPasses.h"

#include <algorithm>

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

    namespace {
        int BandOf(const WorkItem &item) {
            return GetStateKind(item.payload) == StateKind::kCandidateExpr ? 0 : 1;
        }

        bool IsBetterPriority(const WorkItem &a, const WorkItem &b) {
            int band_a = BandOf(a);
            int band_b = BandOf(b);
            if (band_a != band_b) { return band_a < band_b; }
            if (a.depth != b.depth) { return a.depth < b.depth; }
            if (a.features.provenance != b.features.provenance) {
                return a.features.provenance < b.features.provenance;
            }
            if (a.history.size() != b.history.size()) {
                return a.history.size() < b.history.size();
            }
            return false;
        }
    } // namespace

    size_t StateFingerprintHash::operator()(const StateFingerprint &fp) const {
        size_t h = std::hash< uint64_t >{}(fp.payload_hash);
        h ^= std::hash< int >{}(static_cast< int >(fp.kind)) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash< uint32_t >{}(fp.bitwidth) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash< int >{}(static_cast< int >(fp.provenance)) + 0x9e3779b9 + (h << 6)
            + (h >> 2);
        h ^= std::hash< uint32_t >{}(fp.stage_cursor) + 0x9e3779b9 + (h << 6) + (h >> 2);
        for (const auto &v : fp.vars) {
            h ^= std::hash< std::string >{}(v) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        return h;
    }

    void PassAttemptCache::Record(const StateFingerprint &fp, PassId pass) {
        cache_[fp].push_back(pass);
    }

    bool PassAttemptCache::HasAttempted(const StateFingerprint &fp, PassId pass) const {
        auto it = cache_.find(fp);
        if (it == cache_.end()) { return false; }
        const auto &passes = it->second;
        return std::find(passes.begin(), passes.end(), pass) != passes.end();
    }

    void Worklist::Push(WorkItem item) {
        items_.push_back(std::move(item));
        high_water_ = std::max(high_water_, items_.size());
    }

    WorkItem Worklist::Pop() {
        size_t best = 0;
        for (size_t i = 1; i < items_.size(); ++i) {
            if (IsBetterPriority(items_[i], items_[best])) { best = i; }
        }
        WorkItem result = std::move(items_[best]);
        items_.erase(items_.begin() + static_cast< ptrdiff_t >(best));
        return result;
    }

    bool Worklist::Empty() const { return items_.empty(); }

    size_t Worklist::Size() const { return items_.size(); }

    size_t Worklist::HighWaterMark() const { return high_water_; }

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
