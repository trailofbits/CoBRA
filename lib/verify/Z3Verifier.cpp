#include "cobra/verify/Z3Verifier.h"
#include "cobra/core/Expr.h"
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>
#include <z3.h>

namespace cobra {

    namespace {

        Z3_ast BuildZ3Expr(
            Z3_context ctx, const Expr &expr, const std::vector< Z3_ast > &var_asts,
            uint32_t bitwidth
        ) {
            switch (expr.kind) {
                case Expr::Kind::kConstant:
                    return Z3_mk_unsigned_int64(
                        ctx, expr.constant_val, Z3_mk_bv_sort(ctx, bitwidth)
                    );
                case Expr::Kind::kVariable:
                    return var_asts[expr.var_index];
                case Expr::Kind::kAdd: {
                    auto *l = BuildZ3Expr(ctx, *expr.children[0], var_asts, bitwidth);
                    auto *r = BuildZ3Expr(ctx, *expr.children[1], var_asts, bitwidth);
                    return Z3_mk_bvadd(ctx, l, r);
                }
                case Expr::Kind::kMul: {
                    auto *l = BuildZ3Expr(ctx, *expr.children[0], var_asts, bitwidth);
                    auto *r = BuildZ3Expr(ctx, *expr.children[1], var_asts, bitwidth);
                    return Z3_mk_bvmul(ctx, l, r);
                }
                case Expr::Kind::kAnd: {
                    auto *l = BuildZ3Expr(ctx, *expr.children[0], var_asts, bitwidth);
                    auto *r = BuildZ3Expr(ctx, *expr.children[1], var_asts, bitwidth);
                    return Z3_mk_bvand(ctx, l, r);
                }
                case Expr::Kind::kOr: {
                    auto *l = BuildZ3Expr(ctx, *expr.children[0], var_asts, bitwidth);
                    auto *r = BuildZ3Expr(ctx, *expr.children[1], var_asts, bitwidth);
                    return Z3_mk_bvor(ctx, l, r);
                }
                case Expr::Kind::kXor: {
                    auto *l = BuildZ3Expr(ctx, *expr.children[0], var_asts, bitwidth);
                    auto *r = BuildZ3Expr(ctx, *expr.children[1], var_asts, bitwidth);
                    return Z3_mk_bvxor(ctx, l, r);
                }
                case Expr::Kind::kNot: {
                    auto *o = BuildZ3Expr(ctx, *expr.children[0], var_asts, bitwidth);
                    return Z3_mk_bvnot(ctx, o);
                }
                case Expr::Kind::kNeg: {
                    auto *o = BuildZ3Expr(ctx, *expr.children[0], var_asts, bitwidth);
                    return Z3_mk_bvneg(ctx, o);
                }
                case Expr::Kind::kShr: {
                    auto *o = BuildZ3Expr(ctx, *expr.children[0], var_asts, bitwidth);
                    auto *k = Z3_mk_unsigned_int64(
                        ctx, expr.constant_val, Z3_mk_bv_sort(ctx, bitwidth)
                    );
                    return Z3_mk_bvlshr(ctx, o, k);
                }
            }
            return nullptr; // unreachable
        }

        // Build the expression from CoB coefficients: sum over all subsets S of
        // variables of coeffs[S] * AND(vars in S). Index i encodes subset S as a
        // bitmask, so popcount(i)==0 is the constant, popcount(i)==1 is a single
        // variable, and popcount(i)>=2 is an AND-product of multiple variables.
        Z3_ast BuildOriginalFromCoeffs(
            Z3_context ctx, const std::vector< uint64_t > &coeffs,
            const std::vector< Z3_ast > &var_asts, uint32_t num_vars, uint32_t bitwidth
        ) {
            Z3_sort bv_sort = Z3_mk_bv_sort(ctx, bitwidth);
            Z3_ast result   = Z3_mk_unsigned_int64(ctx, coeffs[0], bv_sort);

            const size_t kLen = 1ULL << num_vars;
            for (size_t i = 1; i < kLen; ++i) {
                if (coeffs[i] == 0) { continue; }

                // Build AND-product of all variables whose bits are set in i
                Z3_ast product = nullptr;
                for (uint32_t v = 0; v < num_vars; ++v) {
                    if ((i & (1ULL << v)) == 0u) { continue; }
                    if (product == nullptr) {
                        product = var_asts[v];
                    } else {
                        product = Z3_mk_bvand(ctx, product, var_asts[v]);
                    }
                }

                Z3_ast coeff = Z3_mk_unsigned_int64(ctx, coeffs[i], bv_sort);
                Z3_ast term  = Z3_mk_bvmul(ctx, coeff, product);
                result       = Z3_mk_bvadd(ctx, result, term);
            }
            return result;
        }

    } // namespace

    Z3VerifyResult Z3Verify(
        const std::vector< uint64_t > &cob_coeffs, const Expr &simplified,
        const std::vector< std::string > &var_names, uint32_t num_vars, uint32_t bitwidth,
        uint32_t timeout_ms
    ) {
        Z3_config cfg = Z3_mk_config();
        Z3_set_param_value(cfg, "timeout", std::to_string(timeout_ms).c_str());
        Z3_context ctx = Z3_mk_context(cfg);
        Z3_del_config(cfg);

        Z3_sort bv_sort = Z3_mk_bv_sort(ctx, bitwidth);

        std::vector< Z3_ast > var_asts;
        for (uint32_t v = 0; v < num_vars; ++v) {
            Z3_symbol sym = Z3_mk_string_symbol(ctx, var_names[v].c_str());
            var_asts.push_back(Z3_mk_const(ctx, sym, bv_sort));
        }

        Z3_ast original =
            BuildOriginalFromCoeffs(ctx, cob_coeffs, var_asts, num_vars, bitwidth);
        Z3_ast simpl = BuildZ3Expr(ctx, simplified, var_asts, bitwidth);

        // Assert original != simplified (if unsat => equivalent)
        Z3_ast neq = Z3_mk_not(ctx, Z3_mk_eq(ctx, original, simpl));

        Z3_solver solver = Z3_mk_solver(ctx);
        Z3_solver_inc_ref(ctx, solver);
        Z3_solver_assert(ctx, solver, neq);

        Z3VerifyResult result;
        const Z3_lbool kStatus = Z3_solver_check(ctx, solver);

        if (kStatus == Z3_L_FALSE) {
            result.equivalent = true;
        } else if (kStatus == Z3_L_TRUE) {
            result.equivalent = false;
            Z3_model model    = Z3_solver_get_model(ctx, solver);
            std::ostringstream ss;
            ss << Z3_model_to_string(ctx, model);
            result.counterexample = ss.str();
        } else {
            result.equivalent     = false;
            result.counterexample = "Z3 returned unknown (possible timeout)";
        }

        Z3_solver_dec_ref(ctx, solver);
        Z3_del_context(ctx);

        return result;
    }

    Z3VerifyResult Z3VerifyExprs(
        const Expr &original, const Expr &simplified,
        const std::vector< std::string > &var_names, uint32_t bitwidth, uint32_t timeout_ms
    ) {
        Z3_config cfg = Z3_mk_config();
        Z3_set_param_value(cfg, "timeout", std::to_string(timeout_ms).c_str());
        Z3_context ctx = Z3_mk_context(cfg);
        Z3_del_config(cfg);

        Z3_sort bv_sort = Z3_mk_bv_sort(ctx, bitwidth);

        std::vector< Z3_ast > var_asts;
        for (const auto &var_name : var_names) {
            Z3_symbol sym = Z3_mk_string_symbol(ctx, var_name.c_str());
            var_asts.push_back(Z3_mk_const(ctx, sym, bv_sort));
        }

        Z3_ast lhs = BuildZ3Expr(ctx, original, var_asts, bitwidth);
        Z3_ast rhs = BuildZ3Expr(ctx, simplified, var_asts, bitwidth);

        // Assert original != simplified (if unsat => equivalent)
        Z3_ast neq = Z3_mk_not(ctx, Z3_mk_eq(ctx, lhs, rhs));

        Z3_solver solver = Z3_mk_solver(ctx);
        Z3_solver_inc_ref(ctx, solver);
        Z3_solver_assert(ctx, solver, neq);

        Z3VerifyResult result;
        const Z3_lbool kStatus = Z3_solver_check(ctx, solver);

        if (kStatus == Z3_L_FALSE) {
            result.equivalent = true;
        } else if (kStatus == Z3_L_TRUE) {
            result.equivalent = false;
            Z3_model model    = Z3_solver_get_model(ctx, solver);
            std::ostringstream ss;
            ss << Z3_model_to_string(ctx, model);
            result.counterexample = ss.str();
        } else {
            result.equivalent     = false;
            result.counterexample = "Z3 returned unknown (possible timeout)";
        }

        Z3_solver_dec_ref(ctx, solver);
        Z3_del_context(ctx);

        return result;
    }

} // namespace cobra
