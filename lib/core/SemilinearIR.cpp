#include "cobra/core/SemilinearIR.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace cobra {

    size_t AtomKeyHash::operator()(const AtomKey &k) const {
        size_t h = std::hash< size_t >{}(k.support.size());
        for (auto v : k.support) {
            h ^= std::hash< uint32_t >{}(v) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        for (auto t : k.truth_table) {
            h ^= std::hash< uint64_t >{}(t) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        return h;
    }

    namespace {

        uint64_t EvalExprBool(
            const Expr &e, const std::vector< GlobalVarIdx > &support, uint64_t assignment,
            uint64_t mask
        ) {
            switch (e.kind) {
                case Expr::Kind::kConstant:
                    return e.constant_val & mask;
                case Expr::Kind::kVariable: {
                    for (size_t i = 0; i < support.size(); ++i) {
                        if (support[i] == e.var_index) {
                            return (assignment >> i) & 1;
                        }
                    }
                    return 0;
                }
                case Expr::Kind::kAnd:
                    return EvalExprBool(*e.children[0], support, assignment, mask)
                        & EvalExprBool(*e.children[1], support, assignment, mask);
                case Expr::Kind::kOr:
                    return EvalExprBool(*e.children[0], support, assignment, mask)
                        | EvalExprBool(*e.children[1], support, assignment, mask);
                case Expr::Kind::kXor:
                    return EvalExprBool(*e.children[0], support, assignment, mask)
                        ^ EvalExprBool(*e.children[1], support, assignment, mask);
                case Expr::Kind::kNot:
                    return (~EvalExprBool(*e.children[0], support, assignment, mask)) & mask;
                case Expr::Kind::kShr: {
                    const uint64_t val =
                        EvalExprBool(*e.children[0], support, assignment, mask);
                    return (val >> e.constant_val) & mask;
                }
                case Expr::Kind::kAdd:
                case Expr::Kind::kMul:
                case Expr::Kind::kNeg:
                    std::unreachable();
            }
            std::unreachable();
        }

    } // namespace

    std::vector< uint64_t > ComputeAtomTruthTable(
        const Expr &atom, const std::vector< GlobalVarIdx > &support, uint32_t bitwidth
    ) {
        const size_t n = support.size();
        if (n > 5) {
            return {};
        }
        const size_t len    = size_t{ 1 } << n;
        const uint64_t mask = Bitmask(bitwidth);
        std::vector< uint64_t > tt(len);
        for (size_t i = 0; i < len; ++i) {
            tt[i] = EvalExprBool(atom, support, i, mask);
        }
        return tt;
    }

} // namespace cobra
