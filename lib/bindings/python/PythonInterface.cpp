#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include "CobraPython.hpp"

namespace nb = nanobind;
using namespace nb::literals;

NB_MODULE(cobra_mba, m) {
  nb::enum_<cobra::Expr::Kind>(m, "ExprKind")
    .value("Constant", cobra::Expr::Kind::kConstant)
    .value("Variable", cobra::Expr::Kind::kVariable)
    .value("Add", cobra::Expr::Kind::kAdd)
    .value("Mul", cobra::Expr::Kind::kMul)
    .value("And", cobra::Expr::Kind::kAnd)
    .value("Or", cobra::Expr::Kind::kOr)
    .value("Xor", cobra::Expr::Kind::kXor)
    .value("Not", cobra::Expr::Kind::kNot)
    .value("Neg", cobra::Expr::Kind::kNeg)
    .value("Shr", cobra::Expr::Kind::kShr);
  nb::class_<cobra::py::PyExpr>(m, "ExprNode")
    .def_ro("kind", &cobra::py::PyExpr::kind)
    .def_ro("constant_val", &cobra::py::PyExpr::constant_val)
    .def_ro("var_index", &cobra::py::PyExpr::var_index)
    .def_ro("children", &cobra::py::PyExpr::children);
  nb::class_<cobra::py::PyExprTree>(m, "Expr")
    .def(nb::init<const std::string &, uint32_t, uint32_t>(),
         "expr"_a, "max_vars"_a = 16, "bitwidth"_a = 64)
    .def(nb::init<const cobra::py::PyExpr &, std::vector< std::string >, uint32_t>(),
         "root"_a, "vars"_a, "bitwidth"_a = 64)
    .def_static(
      "parse",
      [](const std::string &expr, uint32_t max_vars, uint32_t bitwidth) {
        return cobra::py::PyExprTree(expr, max_vars, bitwidth);
      },
      "expr"_a, "max_vars"_a = 16, "bitwidth"_a = 64
    )
    .def("__str__", &cobra::py::PyExprTree::ToString)
    .def_rw("variables", &cobra::py::PyExprTree::vars)
    .def_rw("root", &cobra::py::PyExprTree::root);
}
