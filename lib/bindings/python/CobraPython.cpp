#include "CobraPython.hpp"
#include "ExprParser.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/Simplifier.h"

#include <stdexcept>
#include <string>

namespace {
std::vector<uint64_t> EvaluateToSignature(const cobra::Expr &ast,
                                          uint32_t num_vars,
                                          uint32_t bitwidth) {
  const size_t kLen = size_t{1} << num_vars;
  std::vector<uint64_t> sig(kLen);
  for (size_t i = 0; i < kLen; ++i) {
    std::vector<uint64_t> var_values(num_vars);
    for (uint32_t v = 0; v < num_vars; ++v)
      var_values[v] = (i >> v) & 1;
    sig[i] = cobra::EvalExpr(ast, var_values, bitwidth);
  }
  return sig;
}
} // namespace

namespace cobra::py {

// converts a core Expr into a python facing PyExpr
// copys the entier tree to avoid lifetime issues due to the mutability of
// PyExpr
PyExpr PyExpr::FromExprNode(const Expr &expr) {
  PyExpr out;
  out.kind = expr.kind;
  out.constant_val = expr.constant_val;
  out.var_index = expr.var_index;
  out.children.reserve(expr.children.size());
  for (const auto &child : expr.children)
    out.children.push_back(FromExprNode(*child));
  return out;
}

// require given no. subnexpressions
void PyExpr::RequireArity(const PyExpr &node, size_t expected,
                          const char *label) {
  if (node.children.size() != expected) {
    throw std::runtime_error(std::string("PyExpr ") + label + " expects " +
                             std::to_string(expected) + " child(ren)");
  }
}

// Converts a PyExpr back into a core Expr
// TODO: Maybe force tailcall to prevent stack overflows on large linear
// expressions
std::unique_ptr<Expr> PyExpr::ToExprNode(const PyExpr &node) {
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
    return Expr::Add(ToExprNode(node.children[0]),
                     ToExprNode(node.children[1]));

  case Expr::Kind::kMul:
    RequireArity(node, 2, "Mul");
    return Expr::Mul(ToExprNode(node.children[0]),
                     ToExprNode(node.children[1]));

  case Expr::Kind::kAnd:
    RequireArity(node, 2, "And");
    return Expr::BitwiseAnd(ToExprNode(node.children[0]),
                            ToExprNode(node.children[1]));

  case Expr::Kind::kOr:
    RequireArity(node, 2, "Or");
    return Expr::BitwiseOr(ToExprNode(node.children[0]),
                           ToExprNode(node.children[1]));

  case Expr::Kind::kXor:
    RequireArity(node, 2, "Xor");
    return Expr::BitwiseXor(ToExprNode(node.children[0]),
                            ToExprNode(node.children[1]));
  }
  throw std::runtime_error("PyExpr has unknown kind");
}

// Parses an expression from a string. Uses the same parser as the cobra-cli
// tool. default params declared in header
PyExprTree::PyExprTree(const std::string &s, uint32_t max_vars = 16,
                       uint32_t bitwidth = 64) {
  auto parsed = cobra::ParseToAst(s, bitwidth);
  if (!parsed.has_value())
    throw std::runtime_error(parsed.error().message);

  auto &ast = parsed.value();
  if (ast.vars.size() > max_vars) {
    throw std::runtime_error(
        "expression has " + std::to_string(ast.vars.size()) +
        " variables (max " + std::to_string(max_vars) + ")");
  }

  this->bitwidth = bitwidth;

  auto folded = cobra::FoldConstantBitwise(std::move(ast.expr), bitwidth);
  this->root = PyExpr::FromExprNode(*folded);
  this->vars = std::move(ast.vars);
}

std::string PyExprTree::ToString() const {
  auto expr = root.ToExpr();
  return cobra::Render(*expr, vars, bitwidth);
}

void PyExprTree::Simplify(bool validate = false) {
  auto expr = root.ToExpr();
  auto num_vars = static_cast<uint32_t>(vars.size());
  auto sig = EvaluateToSignature(*expr, num_vars, bitwidth);

  cobra::Options opts{
      .bitwidth = bitwidth, .max_vars = num_vars, .spot_check = true};
  opts.evaluator = cobra::Evaluator::FromExpr(
      *expr, bitwidth, cobra::EvaluatorTraceKind::kCliOriginalAst);

  auto result = cobra::Simplify(sig, vars, expr.get(), opts);
  if (!result.has_value())
    throw std::runtime_error(result.error().message);

  auto &outcome = result.value();
  if (outcome.kind == cobra::SimplifyOutcome::Kind::kError)
    throw std::runtime_error(outcome.diag.reason);
  if (outcome.kind == cobra::SimplifyOutcome::Kind::kUnchangedUnsupported)
    return;

  if (validate) {
    std::vector<uint32_t> var_map;
    if (outcome.real_vars.size() < vars.size())
      var_map = cobra::BuildVarSupport(vars, outcome.real_vars);
    auto fw = cobra::FullWidthCheck(*expr, num_vars, *outcome.expr, var_map,
                                    bitwidth);
    if (!fw.passed) {
      throw std::runtime_error(
          "CoB result is only correct on {0,1} inputs (polynomial target)");
    }
  }

  this->root = PyExpr::FromExprNode(*outcome.expr);
  this->vars = std::move(outcome.real_vars);
}

} // namespace cobra::py
