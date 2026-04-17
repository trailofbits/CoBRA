#include "cobra/core/Expr.h"

namespace cobra::py {
// Python-facing copy of Expr. Owns a value tree so it can be edited safely.
// We will only have users interact with this if they want to modify the
// expression tree
struct PyExpr {
  cobra::Expr::Kind kind = Expr::Kind::kConstant;
  uint64_t constant_val = 0;
  uint32_t var_index = 0;
  std::vector<PyExpr> children;

  std::unique_ptr<Expr> ToExpr() const { return ToExprNode(*this); }

  static PyExpr FromExprNode(const Expr &expr);

private:
  static void RequireArity(const PyExpr &node, size_t expected,
                           const char *label);

  static std::unique_ptr<Expr> ToExprNode(const PyExpr &node);
};

// This is the struct the python bindings will mostly interact with as it will
// keep track of the variable name and bitwidth information. IMO best solution
// as it lets the libary user ignore the fact that the vars are internally
// represted as indices not strings (unless they want to modify it)
struct PyExprTree {
  PyExpr root;
  std::vector<std::string> vars;
  uint32_t bitwidth = 64;

  explicit PyExprTree(const PyExpr &expr, std::vector<std::string> vars,
                      uint32_t bitwidth = 64)
      : root(expr), vars(std::move(vars)), bitwidth(bitwidth) {}

  explicit PyExprTree(const std::string &s, uint32_t max_vars,
                      uint32_t bitwidth);
  std::string ToString() const;

  std::unique_ptr<Expr> ToExpr() const { return root.ToExpr(); }

  void UpdateExpr(const Expr &expr) { root = PyExpr::FromExprNode(expr); }

  void Simplify(bool validate);
};

} // namespace cobra::py
