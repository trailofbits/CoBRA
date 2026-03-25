#include "DecompositionPassHelpers.h"
#include "Orchestrator.h"
#include "OrchestratorPasses.h"
#include "cobra/core/Expr.h"
#include "cobra/core/SignatureEval.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(DecompositionSignature, UsesInputSigWhenFresh) {
    Options opts;
    opts.bitwidth                     = 64;
    std::vector< std::string > vars   = { "x0", "x1" };
    std::vector< uint64_t > input_sig = { 0, 1, 2, 3 };
    OrchestratorContext ctx{
        .opts          = opts,
        .original_vars = vars,
        .bitwidth      = 64,
        .input_sig     = input_sig,
    };

    AstPayload ast{
        .expr = Expr::Variable(0),
    };

    auto sig = ComputeDecompositionSignature(ast, ctx, 0);
    EXPECT_EQ(sig, ctx.input_sig);
}

TEST(DecompositionSignature, EvaluatesFromAstWhenRewritten) {
    Options opts;
    opts.bitwidth                     = 64;
    std::vector< std::string > vars   = { "x0" };
    std::vector< uint64_t > input_sig = { 0, 1 };
    OrchestratorContext ctx{
        .opts          = opts,
        .original_vars = vars,
        .bitwidth      = 64,
        .input_sig     = input_sig,
    };

    AstPayload ast{
        .expr = Expr::Variable(0),
    };

    // rewrite_gen > 0 forces evaluation from AST
    auto sig      = ComputeDecompositionSignature(ast, ctx, 1);
    auto expected = EvaluateBooleanSignature(*ast.expr, 1, 64);
    EXPECT_EQ(sig, expected);
}

TEST(DecompositionSignature, EvaluatesFromAstWhenLoweringFired) {
    Options opts;
    opts.bitwidth                     = 64;
    std::vector< std::string > vars   = { "x0" };
    std::vector< uint64_t > input_sig = { 0, 1 };
    OrchestratorContext ctx{
        .opts           = opts,
        .original_vars  = vars,
        .bitwidth       = 64,
        .input_sig      = input_sig,
        .lowering_fired = true,
    };

    AstPayload ast{
        .expr = Expr::Variable(0),
    };

    auto sig      = ComputeDecompositionSignature(ast, ctx, 0);
    auto expected = EvaluateBooleanSignature(*ast.expr, 1, 64);
    EXPECT_EQ(sig, expected);
}
