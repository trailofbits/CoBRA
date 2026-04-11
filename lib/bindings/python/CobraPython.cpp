#include "CobraPython.hpp"
#include "ExprParser.h"
#include "cobra/core/Classifier.h"

#include <stdexcept>
#include <string>

namespace cobra::py {

    // converts a core Expr into a python facing PyExpr
    // copys the entier tree to avoid lifetime issues due to the mutability of PyExpr
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

    // require given no. subnexpressions
    void PyExpr::RequireArity(const PyExpr &node, size_t expected, const char *label) {
        if (node.children.size() != expected) {
            throw std::runtime_error(
                std::string("PyExpr ") + label + " expects " + std::to_string(expected)
                + " child(ren)"
            );
        }
    }

    // Converts a PyExpr back into a core Expr
    // TODO: Maybe force tailcall to prevent stack overflows on large linear expressions
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
                return Expr::BitwiseAnd(
                    ToExprNode(node.children[0]), ToExprNode(node.children[1])
                );

            case Expr::Kind::kOr:
                RequireArity(node, 2, "Or");
                return Expr::BitwiseOr(
                    ToExprNode(node.children[0]), ToExprNode(node.children[1])
                );

            case Expr::Kind::kXor:
                RequireArity(node, 2, "Xor");
                return Expr::BitwiseXor(
                    ToExprNode(node.children[0]), ToExprNode(node.children[1])
                );
        }
        throw std::runtime_error("PyExpr has unknown kind");
    }

    // Parses an expression from a string. Uses the same parser as the cobra-cli tool.
    // default params declared in header
    PyExprTree::PyExprTree(
        const std::string &s, uint32_t max_vars = 16, uint32_t bitwidth = 64
    ) {
        auto parsed = cobra::ParseToAst(s, bitwidth);
        if (!parsed.has_value()) { throw std::runtime_error(parsed.error().message); }

        auto &ast = parsed.value();
        if (ast.vars.size() > max_vars) {
            throw std::runtime_error(
                "expression has " + std::to_string(ast.vars.size()) + " variables (max "
                + std::to_string(max_vars) + ")"
            );
        }

        this->bitwidth = bitwidth;

        auto folded = cobra::FoldConstantBitwise(std::move(ast.expr), bitwidth);
        this->root  = PyExpr::FromExprNode(*folded);
        this->vars  = std::move(ast.vars);
    }

    std::string PyExprTree::ToString() const {
        auto expr = root.ToExpr();
        return cobra::Render(*expr, vars, bitwidth);
    }
} // namespace cobra::py
