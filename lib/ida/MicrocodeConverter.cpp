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
        ConvertOperand(const mop_t &op, const MBACandidate &candidate) {
            switch (op.t) {
                case mop_d:
                    return BuildExprFromMinsn(*op.d, candidate);
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

        minsn_t *ReconstructImpl(
            const cobra::Expr &expr, const MBACandidate &candidate,
            const std::vector< std::string > &real_vars
        ) {
            int size = static_cast< int >(candidate.bitwidth / 8);
            ea_t ea  = candidate.root->ea;

            switch (expr.kind) {
                case cobra::Expr::Kind::kConstant: {
                    auto *insn   = new minsn_t(ea);
                    insn->opcode = m_mov;
                    insn->l.make_number(expr.constant_val, size);
                    insn->d.size = size;
                    return insn;
                }
                case cobra::Expr::Kind::kVariable: {
                    int leaf_idx = MapVarToLeaf(expr.var_index, candidate, real_vars);
                    if (leaf_idx < 0 || leaf_idx >= static_cast< int >(candidate.leaves.size()))
                    {
                        auto *insn   = new minsn_t(ea);
                        insn->opcode = m_mov;
                        insn->l.make_number(0, size);
                        insn->d.size = size;
                        return insn;
                    }
                    auto *insn   = new minsn_t(ea);
                    insn->opcode = m_mov;
                    insn->l      = *candidate.leaves[leaf_idx];
                    insn->d.size = size;
                    return insn;
                }
                case cobra::Expr::Kind::kAdd:
                    return MakeBinop(
                        m_add, ReconstructImpl(*expr.children[0], candidate, real_vars),
                        ReconstructImpl(*expr.children[1], candidate, real_vars), size, ea
                    );
                case cobra::Expr::Kind::kMul:
                    return MakeBinop(
                        m_mul, ReconstructImpl(*expr.children[0], candidate, real_vars),
                        ReconstructImpl(*expr.children[1], candidate, real_vars), size, ea
                    );
                case cobra::Expr::Kind::kAnd:
                    return MakeBinop(
                        m_and, ReconstructImpl(*expr.children[0], candidate, real_vars),
                        ReconstructImpl(*expr.children[1], candidate, real_vars), size, ea
                    );
                case cobra::Expr::Kind::kOr:
                    return MakeBinop(
                        m_or, ReconstructImpl(*expr.children[0], candidate, real_vars),
                        ReconstructImpl(*expr.children[1], candidate, real_vars), size, ea
                    );
                case cobra::Expr::Kind::kXor:
                    return MakeBinop(
                        m_xor, ReconstructImpl(*expr.children[0], candidate, real_vars),
                        ReconstructImpl(*expr.children[1], candidate, real_vars), size, ea
                    );
                case cobra::Expr::Kind::kNot:
                    return MakeUnop(
                        m_bnot, ReconstructImpl(*expr.children[0], candidate, real_vars), size,
                        ea
                    );
                case cobra::Expr::Kind::kNeg:
                    return MakeUnop(
                        m_neg, ReconstructImpl(*expr.children[0], candidate, real_vars), size,
                        ea
                    );
                case cobra::Expr::Kind::kShr: {
                    auto *insn   = new minsn_t(ea);
                    insn->opcode = m_shr;
                    insn->l.t    = mop_d;
                    insn->l.d    = ReconstructImpl(*expr.children[0], candidate, real_vars);
                    insn->l.size = size;
                    insn->r.make_number(expr.constant_val, size);
                    insn->d.size = size;
                    return insn;
                }
            }
            // Unreachable
            auto *insn   = new minsn_t(ea);
            insn->opcode = m_mov;
            insn->l.make_number(0, size);
            insn->d.size = size;
            return insn;
        }

    } // anonymous namespace

    std::unique_ptr< cobra::Expr >
    BuildExprFromMinsn(const minsn_t &insn, const MBACandidate &candidate) {
        auto l = [&]() { return ConvertOperand(insn.l, candidate); };
        auto r = [&]() { return ConvertOperand(insn.r, candidate); };

        switch (insn.opcode) {
            case m_add:
                return cobra::Expr::Add(l(), r());
            case m_sub:
                return cobra::Expr::Add(l(), cobra::Expr::Negate(r()));
            case m_mul:
                return cobra::Expr::Mul(l(), r());
            case m_and:
                return cobra::Expr::BitwiseAnd(l(), r());
            case m_or:
                return cobra::Expr::BitwiseOr(l(), r());
            case m_xor:
                return cobra::Expr::BitwiseXor(l(), r());
            case m_bnot:
                return cobra::Expr::BitwiseNot(l());
            case m_neg:
                return cobra::Expr::Negate(l());
            default:
                return cobra::Expr::Constant(0);
        }
    }

    minsn_t *ReconstructMinsn(
        const cobra::Expr &expr, const MBACandidate &candidate,
        const std::vector< std::string > &real_vars
    ) {
        return ReconstructImpl(expr, candidate, real_vars);
    }

} // namespace ida_cobra
