#include "MicrocodeConverter.h"

namespace ida_cobra {
    namespace {

        int FindLeafIndex(const mop_t &op, const MBACandidate &candidate) {
            for (size_t i = 0; i < candidate.leaves.size(); ++i) {
                if (*candidate.leaves[i] == op) { return static_cast< int >(i); }
            }
            return -1;
        }

        std::unique_ptr< cobra::Expr >
        ResolveLeafExpr(const mop_t &op, const MBACandidate &candidate) {
            switch (op.t) {
                case mop_n:
                    return cobra::Expr::Constant(static_cast< uint64_t >(op.nnn->value));
                case mop_r:
                case mop_l:
                case mop_S:
                case mop_v: {
                    int idx = FindLeafIndex(op, candidate);
                    if (idx >= 0) {
                        return cobra::Expr::Variable(static_cast< uint32_t >(idx));
                    }
                    return cobra::Expr::Constant(0);
                }
                default:
                    return cobra::Expr::Constant(0);
            }
        }

        std::unique_ptr< cobra::Expr > CombineExpr(
            mcode_t opcode, std::unique_ptr< cobra::Expr > l,
            std::unique_ptr< cobra::Expr > r
        ) {
            if (!l) { return nullptr; }
            switch (opcode) {
                case m_bnot: return cobra::Expr::BitwiseNot(std::move(l));
                case m_neg:  return cobra::Expr::Negate(std::move(l));
                default:     break;
            }
            if (!r) { return nullptr; }
            switch (opcode) {
                case m_add: return cobra::Expr::Add(std::move(l), std::move(r));
                case m_sub: return cobra::Expr::Add(std::move(l), cobra::Expr::Negate(std::move(r)));
                case m_mul: return cobra::Expr::Mul(std::move(l), std::move(r));
                case m_and: return cobra::Expr::BitwiseAnd(std::move(l), std::move(r));
                case m_or:  return cobra::Expr::BitwiseOr(std::move(l), std::move(r));
                case m_xor: return cobra::Expr::BitwiseXor(std::move(l), std::move(r));
                default:    return nullptr;
            }
        }

        int MapVarToLeaf(
            uint32_t var_index, const MBACandidate &candidate,
            const std::vector< std::string > &real_vars
        ) {
            if (var_index >= real_vars.size()) { return -1; }

            const std::string &name = real_vars[var_index];
            for (size_t i = 0; i < candidate.var_names.size(); ++i) {
                if (candidate.var_names[i] == name) { return static_cast< int >(i); }
            }
            return -1;
        }

        minsn_t *MakeBinop(mcode_t opcode, minsn_t *left, minsn_t *right, int size, ea_t ea) {
            auto *insn   = new minsn_t(ea);
            insn->opcode = opcode;
            insn->l.t    = mop_d;
            insn->l.d    = left;
            insn->l.size = size;
            insn->r.t    = mop_d;
            insn->r.d    = right;
            insn->r.size = size;
            insn->d.size = size;
            return insn;
        }

        minsn_t *MakeUnop(mcode_t opcode, minsn_t *operand, int size, ea_t ea) {
            auto *insn   = new minsn_t(ea);
            insn->opcode = opcode;
            insn->l.t    = mop_d;
            insn->l.d    = operand;
            insn->l.size = size;
            insn->d.size = size;
            return insn;
        }

        minsn_t *MakeLeafInsn(const cobra::Expr &expr, const MBACandidate &candidate,
                              const std::vector< std::string > &real_vars, int size, ea_t ea) {
            if (expr.kind == cobra::Expr::Kind::kConstant) {
                auto *insn   = new minsn_t(ea);
                insn->opcode = m_mov;
                insn->l.make_number(expr.constant_val, size);
                insn->d.size = size;
                return insn;
            }
            // kVariable
            int leaf_idx = MapVarToLeaf(expr.var_index, candidate, real_vars);
            auto *insn   = new minsn_t(ea);
            insn->opcode = m_mov;
            if (leaf_idx >= 0 && leaf_idx < static_cast< int >(candidate.leaves.size())) {
                insn->l = *candidate.leaves[leaf_idx];
            } else {
                insn->l.make_number(0, size);
            }
            insn->d.size = size;
            return insn;
        }

        minsn_t *CombineMinsn(const cobra::Expr &expr, minsn_t *child0, minsn_t *child1,
                              int size, ea_t ea) {
            switch (expr.kind) {
                case cobra::Expr::Kind::kAdd: return MakeBinop(m_add, child0, child1, size, ea);
                case cobra::Expr::Kind::kMul: return MakeBinop(m_mul, child0, child1, size, ea);
                case cobra::Expr::Kind::kAnd: return MakeBinop(m_and, child0, child1, size, ea);
                case cobra::Expr::Kind::kOr:  return MakeBinop(m_or,  child0, child1, size, ea);
                case cobra::Expr::Kind::kXor: return MakeBinop(m_xor, child0, child1, size, ea);
                case cobra::Expr::Kind::kNot: return MakeUnop(m_bnot, child0, size, ea);
                case cobra::Expr::Kind::kNeg: return MakeUnop(m_neg, child0, size, ea);
                case cobra::Expr::Kind::kShr: {
                    auto *insn   = new minsn_t(ea);
                    insn->opcode = m_shr;
                    insn->l.t    = mop_d;
                    insn->l.d    = child0;
                    insn->l.size = size;
                    insn->r.make_number(expr.constant_val, size);
                    insn->d.size = size;
                    return insn;
                }
                default: {
                    auto *insn   = new minsn_t(ea);
                    insn->opcode = m_mov;
                    insn->l.make_number(0, size);
                    insn->d.size = size;
                    return insn;
                }
            }
        }

        minsn_t *ReconstructImpl(
            const cobra::Expr &expr, const MBACandidate &candidate,
            const std::vector< std::string > &real_vars
        ) {
            int size = static_cast< int >(candidate.bitwidth / 8);
            ea_t ea  = candidate.root->ea;

            // Flatten the Expr tree into post-order, then build the
            // minsn tree bottom-up with a value stack.
            std::vector< const cobra::Expr * > post;
            {
                std::vector< const cobra::Expr * > work;
                work.push_back(&expr);
                while (!work.empty()) {
                    const cobra::Expr *n = work.back();
                    work.pop_back();
                    post.push_back(n);
                    for (size_t i = 0; i < n->children.size(); ++i) {
                        if (n->children[i]) { work.push_back(n->children[i].get()); }
                    }
                }
            }

            std::vector< minsn_t * > vals;
            for (auto it = post.rbegin(); it != post.rend(); ++it) {
                const cobra::Expr *n = *it;

                if (n->kind == cobra::Expr::Kind::kConstant ||
                    n->kind == cobra::Expr::Kind::kVariable)
                {
                    vals.push_back(MakeLeafInsn(*n, candidate, real_vars, size, ea));
                    continue;
                }

                minsn_t *child1 = nullptr;
                if (n->children.size() > 1 && n->children[1]) {
                    child1 = vals.back();
                    vals.pop_back();
                }

                minsn_t *child0 = nullptr;
                if (n->children.size() > 0 && n->children[0]) {
                    child0 = vals.back();
                    vals.pop_back();
                }

                vals.push_back(CombineMinsn(*n, child0, child1, size, ea));
            }

            return vals.back();
        }

    } // anonymous namespace

    std::unique_ptr< cobra::Expr >
    BuildExprFromMinsn(const minsn_t &insn, const MBACandidate &candidate) {
        // Flatten the minsn tree into post-order, then build the Expr
        // tree bottom-up with a value stack.
        auto post = MicrocodePostOrder(insn);

        std::vector< std::unique_ptr< cobra::Expr > > vals;
        for (auto it = post.rbegin(); it != post.rend(); ++it) {
            const minsn_t *n = *it;

            std::unique_ptr< cobra::Expr > r;
            if (n->r.t == mop_d) { r = std::move(vals.back()); vals.pop_back(); }
            else                  { r = ResolveLeafExpr(n->r, candidate); }

            std::unique_ptr< cobra::Expr > l;
            if (n->l.t == mop_d) { l = std::move(vals.back()); vals.pop_back(); }
            else                  { l = ResolveLeafExpr(n->l, candidate); }

            vals.push_back(CombineExpr(n->opcode, std::move(l), std::move(r)));
        }

        return std::move(vals.back());
    }

    minsn_t *ReconstructMinsn(
        const cobra::Expr &expr, const MBACandidate &candidate,
        const std::vector< std::string > &real_vars
    ) {
        return ReconstructImpl(expr, candidate, real_vars);
    }

} // namespace ida_cobra
