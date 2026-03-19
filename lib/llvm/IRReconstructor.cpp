#include "IRReconstructor.h"
#include "MBADetector.h"
#include "cobra/core/Expr.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Value.h"

namespace cobra {

    llvm::Value *
    ReconstructIr(const Expr &expr, const MBACandidate &candidate, llvm::IRBuilder<> &builder) {
        auto *int_ty = builder.getIntNTy(candidate.bitwidth);

        switch (expr.kind) {
            case Expr::Kind::kConstant:
                return llvm::ConstantInt::get(int_ty, expr.constant_val);

            case Expr::Kind::kVariable:
                return candidate.leaf_values[expr.var_index];

            case Expr::Kind::kAdd: {
                auto *lhs = ReconstructIr(*expr.children[0], candidate, builder);
                auto *rhs = ReconstructIr(*expr.children[1], candidate, builder);
                return builder.CreateAdd(lhs, rhs, "cobra.add");
            }
            case Expr::Kind::kMul: {
                auto *lhs = ReconstructIr(*expr.children[0], candidate, builder);
                auto *rhs = ReconstructIr(*expr.children[1], candidate, builder);
                return builder.CreateMul(lhs, rhs, "cobra.mul");
            }
            case Expr::Kind::kAnd: {
                auto *lhs = ReconstructIr(*expr.children[0], candidate, builder);
                auto *rhs = ReconstructIr(*expr.children[1], candidate, builder);
                return builder.CreateAnd(lhs, rhs, "cobra.and");
            }
            case Expr::Kind::kOr: {
                auto *lhs = ReconstructIr(*expr.children[0], candidate, builder);
                auto *rhs = ReconstructIr(*expr.children[1], candidate, builder);
                return builder.CreateOr(lhs, rhs, "cobra.or");
            }
            case Expr::Kind::kXor: {
                auto *lhs = ReconstructIr(*expr.children[0], candidate, builder);
                auto *rhs = ReconstructIr(*expr.children[1], candidate, builder);
                return builder.CreateXor(lhs, rhs, "cobra.xor");
            }
            case Expr::Kind::kNot: {
                auto *operand = ReconstructIr(*expr.children[0], candidate, builder);
                auto *neg_one = llvm::ConstantInt::getAllOnesValue(int_ty);
                return builder.CreateXor(operand, neg_one, "cobra.not");
            }
            case Expr::Kind::kNeg: {
                auto *operand = ReconstructIr(*expr.children[0], candidate, builder);
                return builder.CreateNeg(operand, "cobra.neg");
            }
            case Expr::Kind::kShr: {
                auto *operand = ReconstructIr(*expr.children[0], candidate, builder);
                auto *amount  = llvm::ConstantInt::get(int_ty, expr.constant_val);
                return builder.CreateLShr(operand, amount, "cobra.shr");
            }
        }
        return nullptr; // unreachable
    }

} // namespace cobra
