#include "cobra/core/Classifier.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Classification.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/Trace.h"
#include <algorithm>
#include <bit>
#include <cstdint>
#include <memory>
#include <utility>

namespace cobra {

    namespace {

        bool IsBitwise(Expr::Kind kind) {
            return kind == Expr::Kind::kAnd || kind == Expr::Kind::kOr
                || kind == Expr::Kind::kXor || kind == Expr::Kind::kNot;
        }

        bool IsArithmetic(Expr::Kind kind) {
            return kind == Expr::Kind::kAdd || kind == Expr::Kind::kMul
                || kind == Expr::Kind::kNeg;
        }

        std::unique_ptr< Expr > TryFoldBinaryBitwise(
            Expr::Kind kind, std::unique_ptr< Expr > lhs, std::unique_ptr< Expr > rhs,
            uint32_t bitwidth
        ) {
            const uint64_t kMask = Bitmask(bitwidth);

            const bool kLhsConst = (lhs->kind == Expr::Kind::kConstant);
            const bool kRhsConst = (rhs->kind == Expr::Kind::kConstant);

            if (kind == Expr::Kind::kAnd) {
                if (kRhsConst && rhs->constant_val == kMask) {
                    COBRA_TRACE("Classifier", "FoldBinaryBitwise: AND identity — rhs all-ones");
                    return lhs;
                }
                if (kLhsConst && lhs->constant_val == kMask) {
                    COBRA_TRACE("Classifier", "FoldBinaryBitwise: AND identity — lhs all-ones");
                    return rhs;
                }
                if (kRhsConst && rhs->constant_val == 0) {
                    COBRA_TRACE("Classifier", "FoldBinaryBitwise: AND absorb — rhs zero");
                    return Expr::Constant(0);
                }
                if (kLhsConst && lhs->constant_val == 0) {
                    COBRA_TRACE("Classifier", "FoldBinaryBitwise: AND absorb — lhs zero");
                    return Expr::Constant(0);
                }
            }

            if (kind == Expr::Kind::kOr) {
                if (kRhsConst && rhs->constant_val == 0) {
                    COBRA_TRACE("Classifier", "FoldBinaryBitwise: OR identity — rhs zero");
                    return lhs;
                }
                if (kLhsConst && lhs->constant_val == 0) {
                    COBRA_TRACE("Classifier", "FoldBinaryBitwise: OR identity — lhs zero");
                    return rhs;
                }
                if (kRhsConst && rhs->constant_val == kMask) {
                    COBRA_TRACE("Classifier", "FoldBinaryBitwise: OR absorb — rhs all-ones");
                    return Expr::Constant(kMask);
                }
                if (kLhsConst && lhs->constant_val == kMask) {
                    COBRA_TRACE("Classifier", "FoldBinaryBitwise: OR absorb — lhs all-ones");
                    return Expr::Constant(kMask);
                }
            }

            if (kind == Expr::Kind::kXor) {
                if (kRhsConst && rhs->constant_val == 0) {
                    COBRA_TRACE("Classifier", "FoldBinaryBitwise: XOR identity — rhs zero");
                    return lhs;
                }
                if (kLhsConst && lhs->constant_val == 0) {
                    COBRA_TRACE("Classifier", "FoldBinaryBitwise: XOR identity — lhs zero");
                    return rhs;
                }
            }

            auto result  = std::make_unique< Expr >();
            result->kind = kind;
            result->children.reserve(2);
            result->children.push_back(std::move(lhs));
            result->children.push_back(std::move(rhs));
            return result;
        }

    } // namespace

    std::unique_ptr< Expr > FoldConstantBitwise(
        std::unique_ptr< Expr > expr, uint32_t bitwidth
    ) { // NOLINT(readability-identifier-naming)
        if (expr->kind == Expr::Kind::kConstant || expr->kind == Expr::Kind::kVariable) {
            return expr;
        }

        // Recurse into all children first (bottom-up)
        for (auto &child : expr->children) {
            child = FoldConstantBitwise(std::move(child), bitwidth);
        }

        // Arithmetic nodes: fold to constant if all children are constants,
        // otherwise return after child recursion without bitwise folds.
        // This ensures e.g. Neg(Constant(C)) → Constant(-C mod 2^bw) so that
        // And(Neg(Constant(C)), Variable(x)) becomes purely bitwise.
        if (IsArithmetic(expr->kind) || expr->kind == Expr::Kind::kShr) {
            if (IsConstantSubtree(*expr)) {
                return Expr::Constant(EvalConstantExpr(*expr, bitwidth));
            }
            return expr;
        }

        // Constant-only bitwise subtree: evaluate to single constant
        if (IsBitwise(expr->kind) && IsConstantSubtree(*expr)) {
            const uint64_t val =
                EvalConstantExpr(*expr, bitwidth); // NOLINT(readability-identifier-naming)
            COBRA_TRACE(
                "Classifier", "FoldConstantBitwise: folded constant subtree to val={}", val
            );
            return Expr::Constant(val);
        }

        // Unary Not: no identity folds apply
        if (expr->kind == Expr::Kind::kNot) { return expr; }

        // Binary bitwise: try identity folds
        return TryFoldBinaryBitwise(
            expr->kind, std::move(expr->children[0]), std::move(expr->children[1]), bitwidth
        );
    }

    namespace {

        struct NodeInfo
        {
            bool has_var_dep          = false;
            bool is_polynomial        = false;
            bool has_const_in_bitwise = false;
            bool has_arith_var_dep    = false;
            StructuralFlag flags      = kSfNone;
            uint64_t var_mask         = 0;
            uint8_t max_var_degree    = 0;
            bool has_non_leaf_bitwise = false;
        };

        NodeInfo ClassifyNode(const Expr &expr) {
            switch (expr.kind) {
                case Expr::Kind::kConstant:
                    return {};

                case Expr::Kind::kVariable: {
                    NodeInfo info;
                    info.has_var_dep    = true;
                    info.var_mask       = (expr.var_index < 64) ? (1ULL << expr.var_index) : 0;
                    info.max_var_degree = 1;
                    return info;
                }

                case Expr::Kind::kMul: {
                    auto lhs = ClassifyNode(*expr.children[0]);
                    auto rhs = ClassifyNode(*expr.children[1]);

                    NodeInfo info;
                    info.has_var_dep   = lhs.has_var_dep || rhs.has_var_dep;
                    info.is_polynomial = lhs.is_polynomial || rhs.is_polynomial
                        || (lhs.has_var_dep && rhs.has_var_dep);
                    info.has_const_in_bitwise =
                        lhs.has_const_in_bitwise || rhs.has_const_in_bitwise;
                    info.has_arith_var_dep =
                        info.has_var_dep || lhs.has_arith_var_dep || rhs.has_arith_var_dep;
                    info.flags  = lhs.flags | rhs.flags;
                    info.flags |= kSfHasArithmetic;
                    info.has_non_leaf_bitwise =
                        lhs.has_non_leaf_bitwise || rhs.has_non_leaf_bitwise;

                    // kSfHasMul: only when both sides carry variable dependence
                    if (lhs.has_var_dep && rhs.has_var_dep) { info.flags |= kSfHasMul; }

                    // Var mask merging
                    info.var_mask = lhs.var_mask | rhs.var_mask;

                    // Degree tracking
                    const bool kOverlap = (lhs.var_mask & rhs.var_mask) != 0;
                    info.max_var_degree = std::max(lhs.max_var_degree, rhs.max_var_degree);
                    if (kOverlap) {
                        info.max_var_degree = static_cast< uint8_t >(
                            std::min< int >(info.max_var_degree + 1, 255)
                        );
                    }

                    // Mixed-product detection (independent of product-type)
                    if ((lhs.has_non_leaf_bitwise || rhs.has_non_leaf_bitwise)
                        && lhs.has_var_dep && rhs.has_var_dep)
                    {
                        info.flags |= kSfHasMixedProduct;
                    }

                    // ArithOverBitwise: Mul dominates bitwise children
                    if (lhs.has_non_leaf_bitwise || rhs.has_non_leaf_bitwise) {
                        info.flags |= kSfHasArithOverBitwise;
                    }

                    COBRA_TRACE(
                        "Classifier",
                        "ClassifyNode: Mul var_mask=0x{:x} max_degree={} flags=0x{:x}",
                        info.var_mask, info.max_var_degree, static_cast< uint32_t >(info.flags)
                    );

                    // Product-type classification
                    if (lhs.has_var_dep && rhs.has_var_dep) {
                        // If variable identity was lost through a bitwise node
                        // (var_mask==0 with has_var_dep), treat as multilinear
                        const bool kLhsIndet = (lhs.var_mask == 0);
                        const bool kRhsIndet = (rhs.var_mask == 0);

                        if (kLhsIndet || kRhsIndet) {
                            // Clear any singleton-power flags inherited from children
                            info.flags =
                                info.flags & ~(kSfHasSingletonPower | kSfHasSingletonPowerGt2);
                            info.flags |= kSfHasMultilinearProduct;
                        } else {
                            const int kVarCount = std::popcount(info.var_mask);
                            if (kVarCount >= 2 && info.max_var_degree >= 2) {
                                // Supersedes any singleton-power flags from children
                                info.flags = info.flags
                                    & ~(kSfHasSingletonPower | kSfHasSingletonPowerGt2);
                                info.flags |= kSfHasMultivarHighPower;
                            } else if (kVarCount == 1 && info.max_var_degree >= 2) {
                                info.flags |= kSfHasSingletonPower;
                                if (info.max_var_degree > 2) {
                                    info.flags |= kSfHasSingletonPowerGt2;
                                }
                            } else if (kVarCount >= 2) {
                                info.flags |= kSfHasMultilinearProduct;
                            }
                        }
                    }

                    return info;
                }

                case Expr::Kind::kAdd: {
                    auto lhs = ClassifyNode(*expr.children[0]);
                    auto rhs = ClassifyNode(*expr.children[1]);

                    NodeInfo info;
                    info.has_var_dep   = lhs.has_var_dep || rhs.has_var_dep;
                    info.is_polynomial = lhs.is_polynomial || rhs.is_polynomial;
                    info.has_const_in_bitwise =
                        lhs.has_const_in_bitwise || rhs.has_const_in_bitwise;
                    info.has_arith_var_dep =
                        info.has_var_dep || lhs.has_arith_var_dep || rhs.has_arith_var_dep;
                    info.flags  = lhs.flags | rhs.flags;
                    info.flags |= kSfHasArithmetic;
                    info.has_non_leaf_bitwise =
                        lhs.has_non_leaf_bitwise || rhs.has_non_leaf_bitwise;

                    // ArithOverBitwise
                    if (lhs.has_non_leaf_bitwise || rhs.has_non_leaf_bitwise) {
                        info.flags |= kSfHasArithOverBitwise;
                    }

                    // Add does not participate in Mul chains
                    info.var_mask       = 0;
                    info.max_var_degree = 0;

                    return info;
                }

                case Expr::Kind::kNeg: {
                    auto child               = ClassifyNode(*expr.children[0]);
                    child.has_arith_var_dep  = child.has_var_dep || child.has_arith_var_dep;
                    child.flags             |= kSfHasArithmetic;
                    return child;
                }

                case Expr::Kind::kAnd:
                case Expr::Kind::kOr:
                case Expr::Kind::kXor: {
                    auto lhs = ClassifyNode(*expr.children[0]);
                    auto rhs = ClassifyNode(*expr.children[1]);

                    NodeInfo info;
                    info.has_var_dep        = lhs.has_var_dep || rhs.has_var_dep;
                    info.is_polynomial      = lhs.is_polynomial || rhs.is_polynomial;
                    info.has_arith_var_dep  = lhs.has_arith_var_dep || rhs.has_arith_var_dep;
                    info.flags              = lhs.flags | rhs.flags;
                    info.flags             |= kSfHasBitwise;
                    info.has_non_leaf_bitwise =
                        lhs.has_non_leaf_bitwise || rhs.has_non_leaf_bitwise;

                    // BitwiseOverArith
                    if (lhs.has_arith_var_dep || rhs.has_arith_var_dep) {
                        info.flags |= kSfHasBitwiseOverArith;
                    }

                    // This IS a non-leaf bitwise node if var-dependent
                    if (info.has_var_dep) { info.has_non_leaf_bitwise = true; }

                    // kSemilinear: const in bitwise
                    info.has_const_in_bitwise =
                        lhs.has_const_in_bitwise || rhs.has_const_in_bitwise;
                    if (info.has_var_dep && (!lhs.has_var_dep || !rhs.has_var_dep)) {
                        info.has_const_in_bitwise = true;
                    }

                    // And/Or/Xor break Mul chains
                    info.var_mask       = 0;
                    info.max_var_degree = 0;

                    return info;
                }

                case Expr::Kind::kNot: {
                    auto child = ClassifyNode(*expr.children[0]);

                    NodeInfo info;
                    info.has_var_dep          = child.has_var_dep;
                    info.is_polynomial        = child.is_polynomial;
                    info.has_const_in_bitwise = child.has_const_in_bitwise;
                    info.has_arith_var_dep    = child.has_arith_var_dep;
                    info.flags                = child.flags | kSfHasBitwise;
                    info.has_non_leaf_bitwise = child.has_non_leaf_bitwise;

                    // BitwiseOverArith
                    if (child.has_arith_var_dep) { info.flags |= kSfHasBitwiseOverArith; }

                    // Not is a non-leaf bitwise node if var-dependent
                    if (child.has_var_dep) { info.has_non_leaf_bitwise = true; }

                    // Not breaks Mul chains
                    info.var_mask       = 0;
                    info.max_var_degree = 0;

                    return info;
                }

                case Expr::Kind::kShr: {
                    auto child = ClassifyNode(*expr.children[0]);

                    NodeInfo info;
                    info.has_var_dep          = child.has_var_dep;
                    info.is_polynomial        = child.is_polynomial;
                    info.has_arith_var_dep    = child.has_arith_var_dep;
                    info.flags                = child.flags;
                    info.has_non_leaf_bitwise = child.has_non_leaf_bitwise;

                    // kSemilinear: Shr is transparent to flags
                    const bool kSemilinear = !child.has_arith_var_dep
                        && (child.has_var_dep || child.has_const_in_bitwise);
                    info.has_const_in_bitwise = kSemilinear || child.has_const_in_bitwise;

                    // Shr breaks Mul chains
                    info.var_mask       = 0;
                    info.max_var_degree = 0;

                    return info;
                }

                default:
                    return {};
            }
        }

    } // namespace

    Classification ClassifyStructural(const Expr &expr) {
        auto info = ClassifyNode(expr);

        SemanticClass sem = SemanticClass::kLinear;
        if (info.is_polynomial) {
            if (HasFlag(info.flags, kSfHasMixedProduct)
                || HasFlag(info.flags, kSfHasBitwiseOverArith)
                || HasFlag(info.flags, kSfHasUnknownShape))
            {
                sem = SemanticClass::kNonPolynomial;
            } else {
                sem = SemanticClass::kPolynomial;
            }
        } else if (info.has_const_in_bitwise) {
            sem = SemanticClass::kSemilinear;
        }

        const Route kRoute = DeriveRoute(info.flags);
        COBRA_TRACE(
            "Classifier", "ClassifyStructural: semantic={} route={} flags=0x{:x}",
            static_cast< int >(sem), static_cast< int >(kRoute),
            static_cast< uint32_t >(info.flags)
        );
        return { .semantic = sem, .flags = info.flags, .route = kRoute };
    }

} // namespace cobra
