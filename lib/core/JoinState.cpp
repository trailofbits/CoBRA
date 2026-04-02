#include "JoinState.h"

#include <functional>
#include <utility>

namespace cobra {

    JoinId CreateJoin(
        absl::flat_hash_map< JoinId, JoinState > &joins, JoinId &next_id, JoinState state
    ) {
        JoinId id = next_id++;
        joins.emplace(id, std::move(state));
        return id;
    }

    std::unique_ptr< Expr > ReplaceByHash(
        std::unique_ptr< Expr > root, size_t target_hash, std::unique_ptr< Expr > &replacement,
        bool &replaced
    ) {
        if (replaced) { return root; }

        size_t h = std::hash< Expr >{}(*root);
        if (h == target_hash) {
            replaced = true;
            return std::move(replacement);
        }

        for (auto &child : root->children) {
            child = ReplaceByHash(std::move(child), target_hash, replacement, replaced);
            if (replaced) { return root; }
        }

        return root;
    }

} // namespace cobra
