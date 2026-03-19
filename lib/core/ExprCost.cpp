#include "cobra/core/ExprCost.h"
#include "cobra/core/Expr.h"
#include <algorithm>
#include <cstdint>
#include <tuple>

namespace cobra {

    CostInfo ComputeCost(const Expr &expr) {
        switch (expr.kind) {
            case Expr::Kind::kConstant:
                return {
                    .cost = { .weighted_size = 1, .nonlinear_mul_count = 0, .max_depth = 1 },
                    .has_var_dep = false
                };

            case Expr::Kind::kVariable:
                return {
                    .cost = { .weighted_size = 1, .nonlinear_mul_count = 0, .max_depth = 1 },
                    .has_var_dep = true
                };

            case Expr::Kind::kNot:
            case Expr::Kind::kNeg: {
                auto child = ComputeCost(*expr.children[0]);
                return {
                    .cost        = { .weighted_size       = child.cost.weighted_size + 1,
                                    .nonlinear_mul_count = child.cost.nonlinear_mul_count,
                                    .max_depth           = child.cost.max_depth + 1 },
                    .has_var_dep = child.has_var_dep
                };
            }

            case Expr::Kind::kShr: {
                auto child = ComputeCost(*expr.children[0]);
                return {
                    .cost        = { .weighted_size       = child.cost.weighted_size + 1,
                                    .nonlinear_mul_count = child.cost.nonlinear_mul_count,
                                    .max_depth           = child.cost.max_depth + 1 },
                    .has_var_dep = child.has_var_dep
                };
            }

            case Expr::Kind::kAdd:
            case Expr::Kind::kAnd:
            case Expr::Kind::kOr:
            case Expr::Kind::kXor: {
                auto lhs = ComputeCost(*expr.children[0]);
                auto rhs = ComputeCost(*expr.children[1]);
                return {
                    .cost = { .weighted_size =
                                  lhs.cost.weighted_size + rhs.cost.weighted_size + 1,
                             .nonlinear_mul_count =
                                  lhs.cost.nonlinear_mul_count + rhs.cost.nonlinear_mul_count,
                             .max_depth =
                                  std::max(lhs.cost.max_depth, rhs.cost.max_depth) + 1 },
                    .has_var_dep = lhs.has_var_dep || rhs.has_var_dep
                };
            }

            case Expr::Kind::kMul: {
                auto lhs               = ComputeCost(*expr.children[0]);
                auto rhs               = ComputeCost(*expr.children[1]);
                const bool kNonlinear  = lhs.has_var_dep && rhs.has_var_dep;
                const uint32_t kWeight = kNonlinear ? 3 : 1;
                uint32_t nl_count = lhs.cost.nonlinear_mul_count + rhs.cost.nonlinear_mul_count
                    + (kNonlinear ? 1 : 0);
                return {
                    .cost = { .weighted_size =
                                  lhs.cost.weighted_size + rhs.cost.weighted_size + kWeight,
                             .nonlinear_mul_count = nl_count,
                             .max_depth =
                                  std::max(lhs.cost.max_depth, rhs.cost.max_depth) + 1 },
                    .has_var_dep = lhs.has_var_dep || rhs.has_var_dep
                };
            }

            default:
                return {
                    .cost = { .weighted_size = 1, .nonlinear_mul_count = 0, .max_depth = 1 },
                    .has_var_dep = false
                };
        }
    }

    bool IsBetter(const ExprCost &candidate, const ExprCost &baseline) {
        return std::tie(
                   candidate.weighted_size, candidate.nonlinear_mul_count, candidate.max_depth
               )
            < std::tie(
                   baseline.weighted_size, baseline.nonlinear_mul_count, baseline.max_depth
            );
    }

} // namespace cobra
