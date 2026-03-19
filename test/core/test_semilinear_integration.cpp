#include "cobra/core/AtomSimplifier.h"
#include "cobra/core/BitPartitioner.h"
#include "cobra/core/Classification.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/MaskedAtomReconstructor.h"
#include "cobra/core/SelfCheck.h"
#include "cobra/core/SemilinearNormalizer.h"
#include <gtest/gtest.h>

#ifdef COBRA_HAS_Z3
    #include "cobra/verify/Z3Verifier.h"
#endif

using namespace cobra;

// Helper: run full semilinear pipeline (including self-check)
static std::unique_ptr< Expr > run_semilinear(
    const Expr &input, const std::vector< std::string > &vars, uint32_t bitwidth = 64
) {
    auto ir_result = NormalizeToSemilinear(input, vars, bitwidth);
    if (!ir_result.has_value()) { return nullptr; }
    auto &ir = ir_result.value();
    SimplifyStructure(ir);

    // Structural self-check (spec Section 7): verify round-trip before
    // applying the OR-assembly rewrite, which merges atoms and would
    // cause a false term-count mismatch in the atom-table comparison.
    auto check_expr = ReconstructMaskedAtoms(ir, {});
    EXPECT_NE(check_expr, nullptr) << "Plain reconstruction failed unexpectedly";
    if (check_expr) {
        auto check = SelfChecSemilinear(ir, *check_expr, vars, bitwidth);
        EXPECT_TRUE(check.passed) << "Self-check failed: " << check.mismatch_detail;
    }

    auto partitions = ComputePartitions(ir);
    auto result     = ReconstructMaskedAtoms(ir, partitions);
    return result;
}

// Test 1: carry erasure
TEST(SemilinearIntegration, CarryErasure) {
    // (x & 0xFF) + 1
    auto input =
        Expr::Add(Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)), Expr::Constant(1));
    EXPECT_EQ(ClassifyStructural(*input).semantic, SemanticClass::kSemilinear);

    auto result = run_semilinear(*input, { "x" });
    ASSERT_NE(result, nullptr);

#ifdef COBRA_HAS_Z3
    auto verify = Z3VerifyExprs(*input, *result, { "x" }, 64);
    EXPECT_TRUE(verify.equivalent) << "Counterexample: " << verify.counterexample;
#endif
}

// Test 2: spurious variable on Boolean inputs
TEST(SemilinearIntegration, SpuriousVarOnBoolean) {
    // (x & 0xFF) + (y & 0xFF00)
    auto input = Expr::Add(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)),
        Expr::BitwiseAnd(Expr::Variable(1), Expr::Constant(0xFF00))
    );
    EXPECT_EQ(ClassifyStructural(*input).semantic, SemanticClass::kSemilinear);

    auto ir = NormalizeToSemilinear(*input, { "x", "y" }, 64);
    ASSERT_TRUE(ir.has_value());
    // Both atoms must be preserved (y must NOT be eliminated)
    EXPECT_EQ(ir.value().terms.size(), 2);
    EXPECT_EQ(ir.value().atom_table.size(), 2);

    auto result = run_semilinear(*input, { "x", "y" });
    ASSERT_NE(result, nullptr);

#ifdef COBRA_HAS_Z3
    auto verify = Z3VerifyExprs(*input, *result, { "x", "y" }, 64);
    EXPECT_TRUE(verify.equivalent);
#endif
}

// Test 3: linear-in-disguise
TEST(SemilinearIntegration, LinearInDisguise) {
    // (x & 0xFF) + (x | 0xFF)
    auto input = Expr::Add(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)),
        Expr::BitwiseOr(Expr::Variable(0), Expr::Constant(0xFF))
    );
    EXPECT_EQ(ClassifyStructural(*input).semantic, SemanticClass::kSemilinear);

    auto result = run_semilinear(*input, { "x" });
    ASSERT_NE(result, nullptr);

    // v1: not expected to simplify to x + 0xFF
    // but result must be equivalent
#ifdef COBRA_HAS_Z3
    auto verify = Z3VerifyExprs(*input, *result, { "x" }, 64);
    EXPECT_TRUE(verify.equivalent);
#endif
}

// Test 4: non-overlapping masks
TEST(SemilinearIntegration, NonOverlappingMasks) {
    // (x & 0x00FF) + (y & 0xFF00)
    auto input = Expr::Add(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0x00FF)),
        Expr::BitwiseAnd(Expr::Variable(1), Expr::Constant(0xFF00))
    );
    EXPECT_EQ(ClassifyStructural(*input).semantic, SemanticClass::kSemilinear);

    auto result = run_semilinear(*input, { "x", "y" });
    ASSERT_NE(result, nullptr);

    // Check that OR assembly was used (reconstruction optimization)
    // The rendered form should contain | instead of +
    auto rendered = Render(*result, { "x", "y" }, 64);
    EXPECT_NE(rendered.find("|"), std::string::npos)
        << "Expected OR assembly for non-overlapping masks, got: " << rendered;

#ifdef COBRA_HAS_Z3
    auto verify = Z3VerifyExprs(*input, *result, { "x", "y" }, 64);
    EXPECT_TRUE(verify.equivalent);
#endif
}

// Test 5: coefficient prevents OR rewrite
TEST(SemilinearIntegration, CoefficientPreventsOR) {
    // 3 * (x & 0x0F) + (y & 0xF0)
    auto input = Expr::Add(
        Expr::Mul(Expr::Constant(3), Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0x0F))),
        Expr::BitwiseAnd(Expr::Variable(1), Expr::Constant(0xF0))
    );
    EXPECT_EQ(ClassifyStructural(*input).semantic, SemanticClass::kSemilinear);

    auto result = run_semilinear(*input, { "x", "y" });
    ASSERT_NE(result, nullptr);

    // Result must use + not | because coefficient 3 spreads bits
    auto rendered = Render(*result, { "x", "y" }, 64);
    EXPECT_NE(rendered.find("+"), std::string::npos)
        << "Expected addition (not OR) due to coefficient, got: " << rendered;

#ifdef COBRA_HAS_Z3
    auto verify = Z3VerifyExprs(*input, *result, { "x", "y" }, 64);
    EXPECT_TRUE(verify.equivalent);
#endif
}

TEST(SemilinearIntegration, ShrAtomRoundTrip) {
    auto input = Expr::LogicalShr(Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)), 4);
    EXPECT_EQ(ClassifyStructural(*input).semantic, SemanticClass::kSemilinear);

    auto result = run_semilinear(*input, { "x" });
    ASSERT_NE(result, nullptr);

    auto text = Render(*result, { "x" }, 64);
    EXPECT_NE(text.find(">>"), std::string::npos) << "Expected >> in output, got: " << text;

#ifdef COBRA_HAS_Z3
    auto verify = Z3VerifyExprs(*input, *result, { "x" }, 64);
    EXPECT_TRUE(verify.equivalent) << "Counterexample: " << verify.counterexample;
#endif
}

TEST(SemilinearIntegration, ShrWithCoeffAndAdd) {
    auto shr_atom =
        Expr::LogicalShr(Expr::BitwiseXor(Expr::Variable(0), Expr::Constant(0xAA)), 2);
    auto input =
        Expr::Add(Expr::Mul(Expr::Constant(3), std::move(shr_atom)), Expr::Variable(1));
    EXPECT_EQ(ClassifyStructural(*input).semantic, SemanticClass::kSemilinear);

    auto result = run_semilinear(*input, { "x", "y" });
    ASSERT_NE(result, nullptr);

#ifdef COBRA_HAS_Z3
    auto verify = Z3VerifyExprs(*input, *result, { "x", "y" }, 64);
    EXPECT_TRUE(verify.equivalent) << "Counterexample: " << verify.counterexample;
#endif
}

TEST(SemilinearIntegration, ShrIdentityFold) {
    auto input = Expr::LogicalShr(Expr::BitwiseOr(Expr::Variable(0), Expr::Variable(1)), 0);

    auto result = run_semilinear(*input, { "x", "y" });
    ASSERT_NE(result, nullptr);

    auto text = Render(*result, { "x", "y" }, 64);
    EXPECT_EQ(text.find(">>"), std::string::npos)
        << "Did not expect >> in output, got: " << text;
}
