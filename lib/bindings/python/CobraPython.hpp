#include "cobra/core/Expr.h"

namespace cobra::py {
    // Python-facing copy of Expr. Owns a value tree so it can be edited safely.
    struct PyExpr
    {
        cobra::Expr::Kind kind = Expr::Kind::kConstant;
        uint64_t constant_val  = 0;
        uint32_t var_index     = 0;
        std::vector< PyExpr > children;

        std::unique_ptr< Expr > ToExpr() const { return ToExprNode(*this); }

        static PyExpr FromExprNode(const Expr &expr);

      private:
        static void RequireArity(const PyExpr &node, size_t expected, const char *label);

        static std::unique_ptr< Expr > ToExprNode(const PyExpr &node);
    };

    struct PyExprTree
    {
        PyExpr root;
        std::vector< std::string > vars;

        explicit PyExprTree(const std::string &s, uint32_t max_vars = 16, uint32_t bitwidth = 64);
    };
} // namespace cobra::py
