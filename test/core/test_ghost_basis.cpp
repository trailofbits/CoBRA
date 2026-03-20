#include "cobra/core/GhostBasis.h"
#include "cobra/core/SignatureChecker.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(GhostBasisTest, AllPrimitivesZeroOnBooleanInputs) {
    for (const auto &prim : GetGhostBasis()) {
        for (uint32_t bw : { 8u, 32u, 64u }) {
            uint32_t combos = 1u << prim.arity;
            for (uint32_t mask = 0; mask < combos; ++mask) {
                std::vector< uint64_t > args(prim.arity);
                for (uint8_t a = 0; a < prim.arity; ++a) { args[a] = (mask >> a) & 1u; }
                uint64_t val = prim.eval(args, bw);
                EXPECT_EQ(val, 0u) << prim.name << " bw=" << bw << " mask=" << mask;
            }
        }
    }
}

TEST(GhostBasisTest, AllPrimitivesNonzeroOnNonBooleanInput) {
    for (const auto &prim : GetGhostBasis()) {
        std::vector< uint64_t > args(prim.arity, 3);
        uint64_t val = prim.eval(args, 64);
        EXPECT_NE(val, 0u) << prim.name << " should be nonzero at (3,3,...)";
    }
}

TEST(GhostBasisTest, BuilderMatchesEval) {
    for (const auto &prim : GetGhostBasis()) {
        std::vector< uint32_t > var_indices(prim.arity);
        for (uint8_t i = 0; i < prim.arity; ++i) { var_indices[i] = i; }
        auto expr = prim.build(var_indices);
        ASSERT_NE(expr, nullptr) << prim.name;

        for (uint64_t a = 2; a < 8; ++a) {
            for (uint64_t b = 2; b < 8; ++b) {
                std::vector< uint64_t > args;
                args.push_back(a);
                args.push_back(b);
                if (prim.arity == 3) { args.push_back(a + b); }

                uint64_t from_eval = prim.eval(args, 64);
                uint64_t from_expr = EvalExpr(*expr, args, 64);
                EXPECT_EQ(from_eval, from_expr) << prim.name << " a=" << a << " b=" << b;
            }
        }
    }
}

TEST(GhostBasisTest, SymmetricPrimitivesCommute) {
    for (const auto &prim : GetGhostBasis()) {
        if (!prim.symmetric) { continue; }
        if (prim.arity == 2) {
            std::vector< uint64_t > fwd = { 5, 11 };
            std::vector< uint64_t > rev = { 11, 5 };
            EXPECT_EQ(prim.eval(fwd, 64), prim.eval(rev, 64)) << prim.name;
        } else if (prim.arity == 3) {
            std::vector< uint64_t > abc = { 3, 7, 13 };
            std::vector< uint64_t > bca = { 7, 13, 3 };
            std::vector< uint64_t > cab = { 13, 3, 7 };
            uint64_t v1                 = prim.eval(abc, 64);
            EXPECT_EQ(v1, prim.eval(bca, 64)) << prim.name;
            EXPECT_EQ(v1, prim.eval(cab, 64)) << prim.name;
        }
    }
}

TEST(GhostBasisTest, BuilderProducesDeterministicStructure) {
    for (const auto &prim : GetGhostBasis()) {
        std::vector< uint32_t > vars(prim.arity);
        for (uint8_t i = 0; i < prim.arity; ++i) { vars[i] = i; }
        auto expr1 = prim.build(vars);
        auto expr2 = prim.build(vars);
        ASSERT_EQ(expr1->kind, Expr::Kind::kAdd) << prim.name;
        ASSERT_EQ(expr2->kind, Expr::Kind::kAdd) << prim.name;
        ASSERT_EQ(expr1->children.size(), expr2->children.size()) << prim.name;
        EXPECT_EQ(expr1->children[0]->kind, Expr::Kind::kMul) << prim.name;
        EXPECT_EQ(expr1->children[1]->kind, Expr::Kind::kNeg) << prim.name;
    }
}
