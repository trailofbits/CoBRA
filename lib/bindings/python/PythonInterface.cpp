#pragma GCC diagnostic push
#pragma GCC diagnostic ignored \
    "-Wsign-conversion" // TODO: fix the bindings to have runtime erros for
#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include "CobraPython.hpp"

namespace nb = nanobind;
using namespace nb::literals;

NB_MODULE(cobra_mba, m) {
    nb::enum_< cobra::Expr::Kind >(m, "ExprKind")
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

    nb::class_< cobra::py::PyExpr >(m, "ExprNode")
        .def_ro("kind", &cobra::py::PyExpr::kind, "The type of the expression node")
        .def_ro(
            "constant_val", &cobra::py::PyExpr::constant_val,
            "If a node is a constant, this will hold its value, undefined otherwise"
        )
        .def_ro(
            "var_index", &cobra::py::PyExpr::var_index,
            "The variables no. true name can be recoved by using it as an index into the var "
            "list, undefined if the node is not a variable"
        )
        .def_ro(
            "children", &cobra::py::PyExpr::children,
            "List of child nodes of the expression node, such as the addends of an addition "
            "node. Will be empty for constant and variable nodes"
        );

    nb::class_<cobra::py::PyExprTree>(m, "Expr")
    .def(nb::init<const cobra::py::PyExpr &, std::vector< std::string >, uint32_t>(),
      "root"_a, "vars"_a, "bitwidth"_a = 64,
      "Create an expression tree from a root node, its variable names and bitwidth")

    .def(nb::init<const std::string &, uint32_t, uint32_t>(),
        "expr"_a, "max_vars"_a = 16, "bitwidth"_a = 64,
        "Parse an expression from a string. Warning increasing max_vars past 16 may cause OOM errors")

    .def_static( // duplicate of the init method but with a nicer name
      "parse",
      [](const std::string &expr, uint32_t max_vars, uint32_t bitwidth) {
        return cobra::py::PyExprTree(expr, max_vars, bitwidth);
      },
      "expr"_a, "max_vars"_a = 16, "bitwidth"_a = 64,
      "Parse an expression from a string. Warning increasing max_vars past 16 may cause OOM errors"
    )

    .def("__str__", &cobra::py::PyExprTree::ToString)
    .def(
        "simplify", &cobra::py::PyExprTree::Simplify, "validate"_a = false,
        "Simplify the expression in place. When validate=true, run full-width checks"
    )
    .def_rw("variables", &cobra::py::PyExprTree::vars, "List of variable names in the expression")
    .def_rw("root", &cobra::py::PyExprTree::root, "Root expression node")
    .def_rw("bitwidth", &cobra::py::PyExprTree::bitwidth, "Bitwidth of the expression");
}

#pragma GCC diagnostic pop
