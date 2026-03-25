#include "cobra/core/Classification.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/DecompositionEngine.h"
#include "cobra/core/Expr.h"
#include "cobra/core/PassContract.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/Simplifier.h"
#include <gtest/gtest.h>

using namespace cobra;

namespace {

    DecompositionContext MakeCtx(
        const Options &opts, const std::vector< std::string > &vars,
        const std::vector< uint64_t > &sig, const Expr *expr, const Classification &cls
    ) {
        return DecompositionContext{
            .opts = opts, .vars = vars, .sig = sig, .current_expr = expr, .cls = cls
        };
    }

} // namespace

TEST(DecompositionEngineTest, BuildResidualEvaluator_SubtractsCore) {
    // f(x0, x1) = x0*x1 + x0^x1
    Evaluator original = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] * v[1] + (v[0] ^ v[1]);
    };
    // core = x0*x1
    auto core     = Expr::Mul(Expr::Variable(0), Expr::Variable(1));
    // residual should be x0^x1
    auto residual = BuildResidualEvaluator(original, *core, 64);

    // Check at a few points
    EXPECT_EQ(residual({ 3, 5 }), (3u ^ 5u));
    EXPECT_EQ(residual({ 0, 0 }), 0u);
    EXPECT_EQ(residual({ 7, 11 }), (7u ^ 11u));
}

TEST(DecompositionEngineTest, BuildResidualEvaluator_BitwidthMasking) {
    // f(x0) = 200 + x0 at 8 bits
    Evaluator original = [](const std::vector< uint64_t > &v) -> uint64_t {
        return 200 + v[0];
    };
    // core = 100
    auto core     = Expr::Constant(100);
    auto residual = BuildResidualEvaluator(original, *core, 8);

    // residual(50) = (200+50 - 100) & 0xFF = 150
    EXPECT_EQ(residual({ 50 }), 150u);
    // residual(200) = (200+200 - 100) & 0xFF = 300 & 0xFF = 44
    EXPECT_EQ(residual({ 200 }), 44u);
}

TEST(ExtractProductCoreTest, FindsProductTerms) {
    // expr = x0*x1 + x0^x2
    auto product  = Expr::Mul(Expr::Variable(0), Expr::Variable(1));
    auto bitwise  = Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(2));
    auto combined = Expr::Add(CloneExpr(*product), std::move(bitwise));

    Options opts{ .bitwidth = 64 };
    std::vector< std::string > vars = { "x0", "x1", "x2" };
    std::vector< uint64_t > sig;
    auto cls = ClassifyStructural(*combined);
    auto ctx = MakeCtx(opts, vars, sig, combined.get(), cls);

    auto core = ExtractProductCore(ctx);
    ASSERT_TRUE(core.Succeeded());
    EXPECT_EQ(core.Payload().kind, ExtractorKind::kProductAST);
    // The core should contain the product term
    EXPECT_EQ(core.Payload().expr->kind, Expr::Kind::kMul);
}

TEST(ExtractProductCoreTest, NoProducts_ReturnsBlocked) {
    // expr = x0 ^ x1 (no product terms)
    auto expr = Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1));

    Options opts{ .bitwidth = 64 };
    std::vector< std::string > vars = { "x0", "x1" };
    std::vector< uint64_t > sig;
    auto cls = ClassifyStructural(*expr);
    auto ctx = MakeCtx(opts, vars, sig, expr.get(), cls);

    auto core = ExtractProductCore(ctx);
    EXPECT_FALSE(core.Succeeded());
}

TEST(ExtractProductCoreTest, ConstantMul_NotExtracted) {
    // expr = 3*x0 + x1 (Mul with constant is not a product)
    auto scaled   = Expr::Mul(Expr::Constant(3), Expr::Variable(0));
    auto combined = Expr::Add(std::move(scaled), Expr::Variable(1));

    Options opts{ .bitwidth = 64 };
    std::vector< std::string > vars = { "x0", "x1" };
    std::vector< uint64_t > sig;
    auto cls = ClassifyStructural(*combined);
    auto ctx = MakeCtx(opts, vars, sig, combined.get(), cls);

    auto core = ExtractProductCore(ctx);
    EXPECT_FALSE(core.Succeeded());
}

TEST(ExtractPolyCoreTest, RecoversDegree2) {
    // f(x0, x1) = x0*x1 + 3*x0 + 2*x1 (pure polynomial, degree 2)
    Evaluator eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] * v[1] + 3 * v[0] + 2 * v[1];
    };
    Options opts{ .bitwidth = 64 };
    opts.evaluator                  = eval;
    std::vector< std::string > vars = { "x0", "x1" };
    auto sig                        = EvaluateBooleanSignature(eval, 2, 64);
    auto cls                        = Classification{ .semantic = SemanticClass::kPolynomial,
                                                      .flags    = kSfHasMul,
                                                      .route    = Route::kMixedRewrite };
    auto ctx                        = MakeCtx(opts, vars, sig, nullptr, cls);
    auto core                       = ExtractPolyCore(ctx, 2);
    ASSERT_TRUE(core.Succeeded());
    EXPECT_EQ(core.Payload().kind, ExtractorKind::kPolynomial);
    EXPECT_EQ(core.Payload().degree_used, 2);
}

TEST(ExtractPolyCoreTest, TooManyVars_ReturnsInapplicable) {
    // 7 live variables exceed the 6-var support cap
    Evaluator eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] * v[1] + v[2] * v[3] + v[4] * v[5] + v[6];
    };
    Options opts{ .bitwidth = 64 };
    opts.evaluator                  = eval;
    std::vector< std::string > vars = { "x0", "x1", "x2", "x3", "x4", "x5", "x6" };
    auto sig                        = EvaluateBooleanSignature(eval, 7, 64);
    auto cls                        = Classification{ .semantic = SemanticClass::kPolynomial,
                                                      .flags    = kSfHasMul,
                                                      .route    = Route::kMixedRewrite };
    auto ctx                        = MakeCtx(opts, vars, sig, nullptr, cls);
    auto core                       = ExtractPolyCore(ctx, 2);
    EXPECT_FALSE(core.Succeeded());
}

TEST(AcceptCoreTest, RejectsZeroCore) {
    auto zero_expr = Expr::Constant(0);
    Evaluator eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] + v[1]; };
    Options opts{ .bitwidth = 64 };
    opts.evaluator                  = eval;
    std::vector< std::string > vars = { "x0", "x1" };
    auto sig                        = EvaluateBooleanSignature(eval, 2, 64);
    auto cls                        = Classification{ .semantic = SemanticClass::kLinear,
                                                      .flags    = kSfNone,
                                                      .route    = Route::kMixedRewrite };
    auto ctx                        = MakeCtx(opts, vars, sig, nullptr, cls);

    CoreCandidate core;
    core.expr = std::move(zero_expr);
    core.kind = ExtractorKind::kPolynomial;
    EXPECT_FALSE(AcceptCore(ctx, core));
}

TEST(AcceptCoreTest, AcceptsNonTrivialCore) {
    // f(x0, x1) = x0*x1 + (x0 ^ x1)
    // Core = x0*x1 — non-trivial (changes function, non-zero residual).
    // AcceptCore only rejects trivially useless cores; residual solvers
    // decide whether the decomposition actually works.
    Evaluator eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] * v[1] + (v[0] ^ v[1]);
    };
    Options opts{ .bitwidth = 64, .spot_check = true };
    opts.evaluator                  = eval;
    std::vector< std::string > vars = { "x0", "x1" };
    auto sig                        = EvaluateBooleanSignature(eval, 2, 64);
    auto cls                        = Classification{ .semantic = SemanticClass::kPolynomial,
                                                      .flags    = kSfHasMul | kSfHasBitwise,
                                                      .route    = Route::kMixedRewrite };
    auto ctx                        = MakeCtx(opts, vars, sig, nullptr, cls);

    auto core_expr = Expr::Mul(Expr::Variable(0), Expr::Variable(1));
    CoreCandidate core;
    core.expr        = std::move(core_expr);
    core.kind        = ExtractorKind::kPolynomial;
    core.degree_used = 2;
    EXPECT_TRUE(AcceptCore(ctx, core));
}

TEST(AcceptCoreTest, AcceptsPolyCoreSameSupportNonRoutable) {
    // f(x0, x1) = x0*x1 + x0*x1*x1 (poly D=2 core + poly D=3 residual)
    // Previously rejected: same support, not boolean-valued, residual
    // not routable by supported pipeline. Now accepted so SolveViaPoly
    // can attempt polynomial recovery on the residual.
    Evaluator eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] * v[1] + v[0] * v[1] * v[1];
    };
    Options opts{ .bitwidth = 64 };
    opts.evaluator                  = eval;
    std::vector< std::string > vars = { "x0", "x1" };
    auto sig                        = EvaluateBooleanSignature(eval, 2, 64);
    auto cls                        = Classification{ .semantic = SemanticClass::kPolynomial,
                                                      .flags    = kSfHasMul,
                                                      .route    = Route::kMixedRewrite };
    auto ctx                        = MakeCtx(opts, vars, sig, nullptr, cls);

    auto core_expr = Expr::Mul(Expr::Variable(0), Expr::Variable(1));
    CoreCandidate core;
    core.expr        = std::move(core_expr);
    core.kind        = ExtractorKind::kPolynomial;
    core.degree_used = 2;
    EXPECT_TRUE(AcceptCore(ctx, core));
}

TEST(AcceptCoreTest, RejectsConstantCore) {
    auto const_expr = Expr::Constant(42);
    Evaluator eval  = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] + v[1]; };
    Options opts{ .bitwidth = 64 };
    opts.evaluator                  = eval;
    std::vector< std::string > vars = { "x0", "x1" };
    auto sig                        = EvaluateBooleanSignature(eval, 2, 64);
    auto cls                        = Classification{ .semantic = SemanticClass::kLinear,
                                                      .flags    = kSfNone,
                                                      .route    = Route::kMixedRewrite };
    auto ctx                        = MakeCtx(opts, vars, sig, nullptr, cls);

    CoreCandidate core;
    core.expr = std::move(const_expr);
    core.kind = ExtractorKind::kPolynomial;
    EXPECT_FALSE(AcceptCore(ctx, core));
}

TEST(ExtractTemplateCoreTest, FindsTemplateForSimpleComposition) {
    // f(x0, x1) = x0 + x1 — template should find this as G(A,B)
    Evaluator eval = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] + v[1]; };
    Options opts{ .bitwidth = 64 };
    opts.evaluator                  = eval;
    std::vector< std::string > vars = { "x0", "x1" };
    auto sig                        = EvaluateBooleanSignature(eval, 2, 64);
    auto cls                        = Classification{ .semantic = SemanticClass::kLinear,
                                                      .flags    = kSfNone,
                                                      .route    = Route::kMixedRewrite };
    auto ctx                        = MakeCtx(opts, vars, sig, nullptr, cls);
    auto core                       = ExtractTemplateCore(ctx);
    ASSERT_TRUE(core.Succeeded());
    EXPECT_EQ(core.Payload().kind, ExtractorKind::kTemplate);
}
