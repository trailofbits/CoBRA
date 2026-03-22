#include "IRReconstructor.h"
#include "MBADetector.h"
#include "cobra/core/Expr.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Value.h"

#include <cstdint>
#include <vector>

namespace cobra {

    llvm::Value *ReconstructIr(
        const Expr &expr, const MBACandidate &candidate, llvm::IRBuilder<> &builder,
        const std::vector< uint32_t > &var_map
    ) {
        auto *int_ty = builder.getIntNTy(candidate.bitwidth);

        switch (expr.kind) {
            case Expr::Kind::kConstant:
                return llvm::ConstantInt::get(int_ty, expr.constant_val);

            case Expr::Kind::kVariable: {
                uint32_t leaf_idx = expr.var_index;
                if (!var_map.empty()) { leaf_idx = var_map[expr.var_index]; }
                return candidate.leaf_values[leaf_idx];
            }

            case Expr::Kind::kAdd: {
                auto *lhs = ReconstructIr(*expr.children[0], candidate, builder, var_map);
                auto *rhs = ReconstructIr(*expr.children[1], candidate, builder, var_map);
                return builder.CreateAdd(lhs, rhs, "cobra.add");
            }
            case Expr::Kind::kMul: {
                auto *lhs = ReconstructIr(*expr.children[0], candidate, builder, var_map);
                auto *rhs = ReconstructIr(*expr.children[1], candidate, builder, var_map);
                return builder.CreateMul(lhs, rhs, "cobra.mul");
            }
            case Expr::Kind::kAnd: {
                auto *lhs = ReconstructIr(*expr.children[0], candidate, builder, var_map);
                auto *rhs = ReconstructIr(*expr.children[1], candidate, builder, var_map);
                return builder.CreateAnd(lhs, rhs, "cobra.and");
            }
            case Expr::Kind::kOr: {
                auto *lhs = ReconstructIr(*expr.children[0], candidate, builder, var_map);
                auto *rhs = ReconstructIr(*expr.children[1], candidate, builder, var_map);
                return builder.CreateOr(lhs, rhs, "cobra.or");
            }
            case Expr::Kind::kXor: {
                auto *lhs = ReconstructIr(*expr.children[0], candidate, builder, var_map);
                auto *rhs = ReconstructIr(*expr.children[1], candidate, builder, var_map);
                return builder.CreateXor(lhs, rhs, "cobra.xor");
            }
            case Expr::Kind::kNot: {
                auto *operand = ReconstructIr(*expr.children[0], candidate, builder, var_map);
                auto *neg_one = llvm::ConstantInt::getAllOnesValue(int_ty);
                return builder.CreateXor(operand, neg_one, "cobra.not");
            }
            case Expr::Kind::kNeg: {
                auto *operand = ReconstructIr(*expr.children[0], candidate, builder, var_map);
                return builder.CreateNeg(operand, "cobra.neg");
            }
            case Expr::Kind::kShr: {
                auto *operand = ReconstructIr(*expr.children[0], candidate, builder, var_map);
                auto *amount  = llvm::ConstantInt::get(int_ty, expr.constant_val);
                return builder.CreateLShr(operand, amount, "cobra.shr");
            }
        }
        return nullptr; // unreachable
    }

} // namespace cobra
