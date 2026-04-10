#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <stdexcept>
#include <utility>

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
  nb::class_<cobra::py::PyExpr>(m, "Expr")
    .def_ro("kind", &cobra::py::PyExpr::kind)
    .def_ro("constant_val", &cobra::py::PyExpr::constant_val)
    .def_ro("var_index", &cobra::py::PyExpr::var_index)
    .def_ro("children", &cobra::py::PyExpr::children);
  nb::class_<cobra::py::PyExprTree>(m, "ExprTree")
    .def(nb::init<const std::string &, uint32_t, uint32_t>());
}
