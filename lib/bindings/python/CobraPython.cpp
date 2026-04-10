#include "CobraPython.hpp"

#include <stdexcept>
#include <string>

namespace cobra::py {

PyExpr PyExpr::FromExprNode(const Expr &expr) {
    PyExpr out;
    out.kind         = expr.kind;
    out.constant_val = expr.constant_val;
    out.var_index    = expr.var_index;
    out.children.reserve(expr.children.size());
    for (const auto &child : expr.children) {
        out.children.push_back(FromExprNode(*child));
    }
    return out;
}

void PyExpr::RequireArity(const PyExpr &node, size_t expected, const char *label) {
    if (node.children.size() != expected) {
        throw std::runtime_error(
            std::string("PyExpr ") + label + " expects " + std::to_string(expected)
            + " child(ren)" 
        );
    }
}

std::unique_ptr< Expr > PyExpr::ToExprNode(const PyExpr &node) {
    switch (node.kind) {
        case Expr::Kind::kConstant:
            return Expr::Constant(node.constant_val);
        case Expr::Kind::kVariable:
            return Expr::Variable(node.var_index);
        case Expr::Kind::kNot:
            RequireArity(node, 1, "Not");
            return Expr::BitwiseNot(ToExprNode(node.children[0]));
        case Expr::Kind::kNeg:
            RequireArity(node, 1, "Neg");
            return Expr::Negate(ToExprNode(node.children[0]));
        case Expr::Kind::kShr:
            RequireArity(node, 1, "Shr");
            return Expr::LogicalShr(ToExprNode(node.children[0]), node.constant_val);
        case Expr::Kind::kAdd:
            RequireArity(node, 2, "Add");
            return Expr::Add(ToExprNode(node.children[0]), ToExprNode(node.children[1]));
        case Expr::Kind::kMul:
            RequireArity(node, 2, "Mul");
            return Expr::Mul(ToExprNode(node.children[0]), ToExprNode(node.children[1]));
        case Expr::Kind::kAnd:
            RequireArity(node, 2, "And");
            return Expr::BitwiseAnd(ToExprNode(node.children[0]), ToExprNode(node.children[1]));
        case Expr::Kind::kOr:
            RequireArity(node, 2, "Or");
            return Expr::BitwiseOr(ToExprNode(node.children[0]), ToExprNode(node.children[1]));
        case Expr::Kind::kXor:
            RequireArity(node, 2, "Xor");
            return Expr::BitwiseXor(ToExprNode(node.children[0]), ToExprNode(node.children[1]));
    }
    throw std::runtime_error("PyExpr has unknown kind");
}

PyExprTree::PyExprTree(const std::string &s, uint32_t max_vars, uint32_t bitwidth) {
    (void)s;
    (void)max_vars;
    (void)bitwidth;
    throw std::runtime_error("PyExprTree is not implemented yet");
}

} // namespace cobra::py
