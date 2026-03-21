#include "cobra/core/SemilinearNormalizer.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Expr.h"
#include "cobra/core/Result.h"
#include "cobra/core/SemilinearIR.h"
#include "cobra/core/Trace.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cobra {

    namespace {

        bool IsPurelyBitwise(const Expr &expr) {
            switch (expr.kind) {
                case Expr::Kind::kConstant:
                case Expr::Kind::kVariable:
                    return true;
                case Expr::Kind::kAnd:
                case Expr::Kind::kOr:
                case Expr::Kind::kXor:
                    return IsPurelyBitwise(*expr.children[0])
                        && IsPurelyBitwise(*expr.children[1]);
                case Expr::Kind::kNot:
                    return IsPurelyBitwise(*expr.children[0]);
                case Expr::Kind::kShr:
                    return IsPurelyBitwise(*expr.children[0]);
                case Expr::Kind::kAdd:
                case Expr::Kind::kMul:
                case Expr::Kind::kNeg:
                    return false;
            }
            return false;
        }

        bool HasVariable(const Expr &expr) {
            if (expr.kind == Expr::Kind::kVariable) { return true; }
            return std::ranges::any_of(expr.children, [](const auto &child) {
                return HasVariable(*child);
            });
        }

        uint64_t EvalConstantBitwise(const Expr &expr, uint64_t mask) {
            switch (expr.kind) {
                case Expr::Kind::kConstant:
                    return expr.constant_val & mask;
                case Expr::Kind::kAnd:
                    return EvalConstantBitwise(*expr.children[0], mask)
                        & EvalConstantBitwise(*expr.children[1], mask);
                case Expr::Kind::kOr:
                    return EvalConstantBitwise(*expr.children[0], mask)
                        | EvalConstantBitwise(*expr.children[1], mask);
                case Expr::Kind::kXor:
                    return EvalConstantBitwise(*expr.children[0], mask)
                        ^ EvalConstantBitwise(*expr.children[1], mask);
                case Expr::Kind::kNot:
                    return (~EvalConstantBitwise(*expr.children[0], mask)) & mask;
                case Expr::Kind::kShr: {
                    const uint64_t kVal = EvalConstantBitwise(*expr.children[0], mask);
                    return ModShr(kVal, expr.constant_val, 64) & mask;
                }
                case Expr::Kind::kVariable:
                case Expr::Kind::kAdd:
                case Expr::Kind::kMul:
                case Expr::Kind::kNeg:
                    break;
            }
            return 0;
        }

        bool ContainsShr(const Expr &expr) {
            if (expr.kind == Expr::Kind::kShr) { return true; }
            return std::ranges::any_of(expr.children, [](const auto &child) {
                return ContainsShr(*child);
            });
        }

        uint64_t EvalConstantArith(const Expr &expr, uint64_t mask, uint32_t bitwidth) {
            switch (expr.kind) {
                case Expr::Kind::kConstant:
                    return expr.constant_val & mask;
                case Expr::Kind::kNeg: {
                    const uint64_t kVal = EvalConstantArith(*expr.children[0], mask, bitwidth);
                    return ModNeg(kVal, bitwidth);
                }
                case Expr::Kind::kAdd: {
                    const uint64_t kLhs = EvalConstantArith(*expr.children[0], mask, bitwidth);
                    const uint64_t kRhs = EvalConstantArith(*expr.children[1], mask, bitwidth);
                    return (kLhs + kRhs) & mask;
                }
                case Expr::Kind::kMul: {
                    const uint64_t kLhs = EvalConstantArith(*expr.children[0], mask, bitwidth);
                    const uint64_t kRhs = EvalConstantArith(*expr.children[1], mask, bitwidth);
                    return (kLhs * kRhs) & mask;
                }
                case Expr::Kind::kShr: {
                    const uint64_t kVal = EvalConstantArith(*expr.children[0], mask, bitwidth);
                    return (kVal >> expr.constant_val) & mask;
                }
                case Expr::Kind::kAnd:
                case Expr::Kind::kOr:
                case Expr::Kind::kXor:
                case Expr::Kind::kNot:
                    return EvalConstantBitwise(expr, mask);
                case Expr::Kind::kVariable:
                    break;
            }
            return 0;
        }

        void CollectVariables(const Expr &expr, std::vector< GlobalVarIdx > &out) {
            if (expr.kind == Expr::Kind::kVariable) {
                out.push_back(expr.var_index);
                return;
            }
            for (const auto &child : expr.children) { CollectVariables(*child, out); }
        }

        bool HasConstant(const Expr &expr) {
            if (expr.kind == Expr::Kind::kConstant) { return true; }
            return std::ranges::any_of(expr.children, [](const auto &child) {
                return HasConstant(*child);
            });
        }

        OperatorFamily DetectProvenance(const Expr &expr) {
            switch (expr.kind) {
                case Expr::Kind::kConstant:
                case Expr::Kind::kVariable:
                    return OperatorFamily::kMixed;
                case Expr::Kind::kNot:
                    return OperatorFamily::kNot;
                case Expr::Kind::kAnd: {
                    auto lp = DetectProvenance(*expr.children[0]);
                    auto rp = DetectProvenance(*expr.children[1]);
                    if ((lp == OperatorFamily::kAnd || lp == OperatorFamily::kMixed)
                        && (rp == OperatorFamily::kAnd || rp == OperatorFamily::kMixed))
                    {
                        return OperatorFamily::kAnd;
                    }
                    return OperatorFamily::kMixed;
                }
                case Expr::Kind::kOr: {
                    auto lp = DetectProvenance(*expr.children[0]);
                    auto rp = DetectProvenance(*expr.children[1]);
                    if ((lp == OperatorFamily::kOr || lp == OperatorFamily::kMixed)
                        && (rp == OperatorFamily::kOr || rp == OperatorFamily::kMixed))
                    {
                        return OperatorFamily::kOr;
                    }
                    return OperatorFamily::kMixed;
                }
                case Expr::Kind::kXor: {
                    auto lp = DetectProvenance(*expr.children[0]);
                    auto rp = DetectProvenance(*expr.children[1]);
                    if ((lp == OperatorFamily::kXor || lp == OperatorFamily::kMixed)
                        && (rp == OperatorFamily::kXor || rp == OperatorFamily::kMixed))
                    {
                        return OperatorFamily::kXor;
                    }
                    return OperatorFamily::kMixed;
                }
                case Expr::Kind::kAdd:
                case Expr::Kind::kMul:
                case Expr::Kind::kNeg:
                case Expr::Kind::kShr:
                    return OperatorFamily::kMixed;
            }
            return OperatorFamily::kMixed;
        }

        struct CollectCtx
        {
            uint32_t bitwidth{};
            uint64_t mask{};
            std::unordered_map< AtomKey, AtomId, AtomKeyHash > atom_map;
            std::vector< AtomInfo > atom_table;
            std::unordered_map< uint64_t, AtomId > hash_cache;
            bool encountered_nonlinear = false;
        };

        struct CollectResult
        {
            uint64_t constant;
            std::vector< WeightedAtom > terms;
        };

        AtomId RegisterAtom(CollectCtx &ctx, const Expr &expr) {
            std::vector< GlobalVarIdx > support;
            CollectVariables(expr, support);
            std::sort(support.begin(), support.end());
            support.erase(std::unique(support.begin(), support.end()), support.end());

            const uint64_t kStructHash = StructuralHash(expr);

            // Fast path: if structural hash matches an existing atom with the same
            // support, structurally identical subtrees are semantically identical —
            // skip truth-table computation entirely.
            auto hash_it = ctx.hash_cache.find(kStructHash);
            if (hash_it != ctx.hash_cache.end()) {
                const auto &existing = ctx.atom_table[hash_it->second];
                if (existing.key.support == support) { return hash_it->second; }
            }

            auto tt = ComputeAtomTruthTable(expr, support, ctx.bitwidth);
            const AtomKey kKey{ .support = std::move(support), .truth_table = std::move(tt) };

            // Truth-table dedup is only safe for pure-variable atoms.
            // kSemilinear atoms (containing constants like x & 0xFF) can have
            // identical Boolean truth tables but differ on full-width inputs
            // because variables evaluate to {0,1} while constants keep their
            // full value.
            const bool kPure = !HasConstant(expr) && !ContainsShr(expr);
            if (kPure && !kKey.truth_table.empty()) {
                auto it = ctx.atom_map.find(kKey);
                if (it != ctx.atom_map.end()) { return it->second; }
            }

            auto atom_id = static_cast< AtomId >(ctx.atom_table.size());
            if (kPure && !kKey.truth_table.empty()) { ctx.atom_map.emplace(kKey, atom_id); }
            ctx.hash_cache.emplace(kStructHash, atom_id);

            AtomInfo info;
            info.atom_id          = atom_id;
            info.key              = kKey;
            info.structural_hash  = kStructHash;
            info.original_subtree = CloneExpr(expr);
            info.provenance       = DetectProvenance(expr);
            ctx.atom_table.push_back(std::move(info));
            return atom_id;
        }

        CollectResult CollectTerms(CollectCtx &ctx, const Expr &expr, uint64_t coeff) {
            coeff &= ctx.mask;
            if (coeff == 0) { return { .constant = 0, .terms = {} }; }

            // Lower XOR/OR with a constant operand to AND-basis:
            //   a ^ c  =  a + c - 2*(a & c)
            //   a | c  =  a + c - (a & c)
            if ((expr.kind == Expr::Kind::kXor || expr.kind == Expr::Kind::kOr)
                && IsPurelyBitwise(expr) && HasVariable(expr))
            {
                const bool kLhsConst = !HasVariable(*expr.children[0]);
                const bool kRhsConst = !HasVariable(*expr.children[1]);
                if (kLhsConst || kRhsConst) {
                    const auto &const_child = kLhsConst ? *expr.children[0] : *expr.children[1];
                    const auto &var_child   = kLhsConst ? *expr.children[1] : *expr.children[0];

                    const uint64_t kC = EvalConstantBitwise(const_child, ctx.mask);

                    // +a with current coefficient
                    auto var_result = CollectTerms(ctx, var_child, coeff);

                    // +c * coeff
                    var_result.constant = (var_result.constant + (coeff * kC)) & ctx.mask;

                    // -(n * coeff) * (a & c), where n=2 for XOR, n=1 for OR
                    auto and_expr  = Expr::BitwiseAnd(CloneExpr(var_child), Expr::Constant(kC));
                    AtomId and_aid = RegisterAtom(ctx, *and_expr);
                    uint64_t and_coeff = (expr.kind == Expr::Kind::kXor)
                        ? ModNeg((2 * coeff) & ctx.mask, ctx.bitwidth)
                        : ModNeg(coeff, ctx.bitwidth);
                    var_result.terms.push_back({ .coeff = and_coeff, .atom_id = and_aid });

                    return var_result;
                }
            }

            // Purely bitwise with variables: treat as atom
            if (IsPurelyBitwise(expr) && HasVariable(expr)) {
                AtomId aid = RegisterAtom(ctx, expr);
                return { .constant = 0, .terms = { { .coeff = coeff, .atom_id = aid } } };
            }

            // No variables: fold to constant
            if (!HasVariable(expr)) {
                const uint64_t kVal = EvalConstantArith(expr, ctx.mask, ctx.bitwidth);
                return { .constant = (coeff * kVal) & ctx.mask, .terms = {} };
            }

            switch (expr.kind) {
                case Expr::Kind::kAdd: {
                    auto left         = CollectTerms(ctx, *expr.children[0], coeff);
                    auto right        = CollectTerms(ctx, *expr.children[1], coeff);
                    const uint64_t kC = (left.constant + right.constant) & ctx.mask;
                    auto &terms       = left.terms;
                    terms.insert(terms.end(), right.terms.begin(), right.terms.end());
                    return { .constant = kC, .terms = std::move(terms) };
                }

                case Expr::Kind::kNeg: {
                    const uint64_t kNegCoeff = ModNeg(coeff, ctx.bitwidth);
                    return CollectTerms(ctx, *expr.children[0], kNegCoeff);
                }

                case Expr::Kind::kMul: {
                    const bool kLhsConst = !HasVariable(*expr.children[0]);
                    const bool kRhsConst = !HasVariable(*expr.children[1]);

                    if (kLhsConst) {
                        const uint64_t kC =
                            EvalConstantArith(*expr.children[0], ctx.mask, ctx.bitwidth);
                        const uint64_t kNewCoeff = (coeff * kC) & ctx.mask;
                        return CollectTerms(ctx, *expr.children[1], kNewCoeff);
                    }
                    if (kRhsConst) {
                        const uint64_t kC =
                            EvalConstantArith(*expr.children[1], ctx.mask, ctx.bitwidth);
                        const uint64_t kNewCoeff = (coeff * kC) & ctx.mask;
                        return CollectTerms(ctx, *expr.children[0], kNewCoeff);
                    }
                    // Both sides have variables — nonlinear
                    ctx.encountered_nonlinear = true;
                    return { .constant = 0, .terms = {} };
                }

                case Expr::Kind::kShr: {
                    if (IsPurelyBitwise(expr) && HasVariable(expr)) {
                        AtomId aid = RegisterAtom(ctx, expr);
                        return { .constant = 0,
                                 .terms    = { { .coeff = coeff, .atom_id = aid } } };
                    }
                    if (!HasVariable(expr)) {
                        const uint64_t kVal = EvalConstantArith(expr, ctx.mask, ctx.bitwidth);
                        return { .constant = (coeff * kVal) & ctx.mask, .terms = {} };
                    }
                    ctx.encountered_nonlinear = true;
                    return { .constant = 0, .terms = {} };
                }

                case Expr::Kind::kConstant:
                case Expr::Kind::kVariable:
                case Expr::Kind::kAnd:
                case Expr::Kind::kOr:
                case Expr::Kind::kXor:
                case Expr::Kind::kNot:
                    break;
            }

            return { .constant = 0, .terms = {} };
        }

    } // namespace

    Result< SemilinearIR > NormalizeToSemilinear(
        const Expr &expr, const std::vector< std::string > &vars, uint32_t bitwidth
    ) {
        COBRA_TRACE(
            "Semilinear", "NormalizeToSemilinear: vars={} bitwidth={}", vars.size(), bitwidth
        );
        CollectCtx ctx; // NOLINT(misc-const-correctness)
        ctx.bitwidth = bitwidth;
        ctx.mask     = Bitmask(bitwidth);

        auto result = CollectTerms(ctx, expr, 1);

        if (ctx.encountered_nonlinear) {
            return Err< SemilinearIR >(
                CobraError::kNonLinearInput,
                "expression is not semilinear (variable*variable "
                "multiplication or shift of non-bitwise operand)"
            );
        }

        // Group terms by AtomId, summing coefficients mod 2^w
        std::unordered_map< AtomId, uint64_t > coeff_map;
        for (const auto &t : result.terms) {
            coeff_map[t.atom_id] = (coeff_map[t.atom_id] + t.coeff) & ctx.mask;
        }

        SemilinearIR ir; // NOLINT(misc-const-correctness)
        ir.constant   = result.constant & ctx.mask;
        ir.bitwidth   = bitwidth;
        ir.atom_table = std::move(ctx.atom_table);

        COBRA_TRACE(
            "Semilinear", "NormalizeToSemilinear: terms={} atoms={}", result.terms.size(),
            ir.atom_table.size()
        );

        for (const auto &[atom_id, coeff] : coeff_map) {
            if (coeff != 0) { ir.terms.push_back({ .coeff = coeff, .atom_id = atom_id }); }
        }

        // Sort terms by atom_id for deterministic output
        std::sort(
            ir.terms.begin(), ir.terms.end(),
            [](const WeightedAtom &a, const WeightedAtom &b) { return a.atom_id < b.atom_id; }
        );

        return Ok(std::move(ir));
    }

} // namespace cobra
