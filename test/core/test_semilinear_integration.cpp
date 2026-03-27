#include "Orchestrator.h"
#include "OrchestratorPasses.h"
#include "SemilinearPasses.h"
#include "cobra/core/AtomSimplifier.h"
#include "cobra/core/BitPartitioner.h"
#include "cobra/core/Classification.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/MaskedAtomReconstructor.h"
#include "cobra/core/SelfCheck.h"
#include "cobra/core/SemilinearNormalizer.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/StructureRecovery.h"
#include "cobra/core/TermRefiner.h"
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
        auto check = SelfCheckSemilinear(ir, *check_expr, vars, bitwidth);
        EXPECT_TRUE(check.passed) << "Self-check failed: " << check.mismatch_detail;
    }

    RecoverStructure(ir);
    RefineTerms(ir);
    CoalesceTerms(ir);

    // Post-rewrite probe check: verify equivalence at random full-width inputs.
    {
        auto probe_expr = ReconstructMaskedAtoms(ir, {});
        EXPECT_NE(probe_expr, nullptr) << "Post-rewrite reconstruction failed";
        if (probe_expr) {
            auto nv    = static_cast< uint32_t >(vars.size());
            auto probe = FullWidthCheck(input, nv, *probe_expr, {}, bitwidth, 16);
            EXPECT_TRUE(probe.passed) << "Post-rewrite probe check failed";
        }
    }

    CompactAtomTable(ir);
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

// Test 3: linear-in-disguise — now simplifies via OR lowering
TEST(SemilinearIntegration, LinearInDisguise) {
    // (x & 0xFF) + (x | 0xFF) = x + 0xFF
    // OR lowering cancels the (x & 0xFF) terms.
    auto input = Expr::Add(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)),
        Expr::BitwiseOr(Expr::Variable(0), Expr::Constant(0xFF))
    );
    EXPECT_EQ(ClassifyStructural(*input).semantic, SemanticClass::kSemilinear);

    auto result = run_semilinear(*input, { "x" });
    ASSERT_NE(result, nullptr);

    // After OR lowering: (x & 0xFF) + x + 0xFF - (x & 0xFF) = x + 0xFF
    auto rendered = Render(*result, { "x" }, 64);
    EXPECT_EQ(rendered.find("&"), std::string::npos)
        << "Expected no AND in simplified output, got: " << rendered;

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

// Test: Issue #8 — XOR constant cancellation
TEST(SemilinearIntegration, XorConstantCancellation) {
    // (x ^ 0x10) + 2*(x & 0x10) = x + 0x10
    auto input = Expr::Add(
        Expr::BitwiseXor(Expr::Variable(0), Expr::Constant(0x10)),
        Expr::Mul(Expr::Constant(2), Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0x10)))
    );
    EXPECT_EQ(ClassifyStructural(*input).semantic, SemanticClass::kSemilinear);

    auto result = run_semilinear(*input, { "x" });
    ASSERT_NE(result, nullptr);

    // XOR lowering enables full cancellation: x + 16
    auto rendered = Render(*result, { "x" }, 64);
    EXPECT_EQ(rendered.find("^"), std::string::npos)
        << "Expected no XOR in simplified output, got: " << rendered;
    EXPECT_EQ(rendered.find("&"), std::string::npos)
        << "Expected no AND in simplified output, got: " << rendered;

#ifdef COBRA_HAS_Z3
    auto verify = Z3VerifyExprs(*input, *result, { "x" }, 64);
    EXPECT_TRUE(verify.equivalent) << "Counterexample: " << verify.counterexample;
#endif
}

// Test: OR constant cancellation
TEST(SemilinearIntegration, OrConstantCancellation) {
    // (x | 0x10) + (x & 0x10) = x + 0x10
    auto input = Expr::Add(
        Expr::BitwiseOr(Expr::Variable(0), Expr::Constant(0x10)),
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0x10))
    );
    EXPECT_EQ(ClassifyStructural(*input).semantic, SemanticClass::kSemilinear);

    auto result = run_semilinear(*input, { "x" });
    ASSERT_NE(result, nullptr);

    auto rendered = Render(*result, { "x" }, 64);
    EXPECT_EQ(rendered.find("|"), std::string::npos)
        << "Expected no OR in simplified output, got: " << rendered;
    EXPECT_EQ(rendered.find("&"), std::string::npos)
        << "Expected no AND in simplified output, got: " << rendered;

#ifdef COBRA_HAS_Z3
    auto verify = Z3VerifyExprs(*input, *result, { "x" }, 64);
    EXPECT_TRUE(verify.equivalent) << "Counterexample: " << verify.counterexample;
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

// Phase 3: XOR recovery — complement pair with negated coefficients
TEST(SemilinearIntegration, XorRecoveryZ3) {
    // (-10)*(98 & x) + 10*(~98 & x) = 10*(98 ^ x) - 980
    auto input = Expr::Add(
        Expr::Mul(
            Expr::Constant(static_cast< uint64_t >(-10LL)),
            Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(98))
        ),
        Expr::Mul(
            Expr::Constant(10), Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(~98ULL))
        )
    );

    auto result = run_semilinear(*input, { "x" });
    ASSERT_NE(result, nullptr);

#ifdef COBRA_HAS_Z3
    auto verify = Z3VerifyExprs(*input, *result, { "x" }, 64);
    EXPECT_TRUE(verify.equivalent) << "Counterexample: " << verify.counterexample;
#endif
}

// Phase 3: XOR recovery in 8-bit
TEST(SemilinearIntegration, XorRecovery8bitZ3) {
    // 1*(0x0F & x) + (-1)*(0xF0 & x) in 8-bit
    auto input = Expr::Add(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0x0F)),
        Expr::Mul(
            Expr::Constant(0xFF), // -1 in 8-bit
            Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xF0))
        )
    );

    auto result = run_semilinear(*input, { "x" }, 8);
    ASSERT_NE(result, nullptr);

#ifdef COBRA_HAS_Z3
    auto verify = Z3VerifyExprs(*input, *result, { "x" }, 8);
    EXPECT_TRUE(verify.equivalent) << "Counterexample: " << verify.counterexample;
#endif
}

// Phase 3: mask elimination — complement pair with different coefficients
TEST(SemilinearIntegration, MaskEliminationZ3) {
    // 5*(0x0F & x) + 3*(0xF0 & x) in 8-bit
    auto input = Expr::Add(
        Expr::Mul(Expr::Constant(5), Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0x0F))),
        Expr::Mul(Expr::Constant(3), Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xF0)))
    );

    auto result = run_semilinear(*input, { "x" }, 8);
    ASSERT_NE(result, nullptr);

#ifdef COBRA_HAS_Z3
    auto verify = Z3VerifyExprs(*input, *result, { "x" }, 8);
    EXPECT_TRUE(verify.equivalent) << "Counterexample: " << verify.counterexample;
#endif
}

// Phase 3: re-invocation merges same-coefficient partitions
TEST(SemilinearIntegration, CoalesceTermsZ3) {
    // 3*(0x55 & x) + 3*(0xAA & x) = 3*x in 8-bit
    auto input = Expr::Add(
        Expr::Mul(Expr::Constant(3), Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0x55))),
        Expr::Mul(Expr::Constant(3), Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xAA)))
    );

    auto result = run_semilinear(*input, { "x" }, 8);
    ASSERT_NE(result, nullptr);

#ifdef COBRA_HAS_Z3
    auto verify = Z3VerifyExprs(*input, *result, { "x" }, 8);
    EXPECT_TRUE(verify.equivalent) << "Counterexample: " << verify.counterexample;
#endif
}

// Phase 3: two-variable XOR recovery
TEST(SemilinearIntegration, XorRecoveryTwoVarZ3) {
    // 7*((x&y) & 0x0F) + (-7)*((x&y) & 0xF0) in 8-bit
    auto basis = Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1));
    auto input = Expr::Add(
        Expr::Mul(Expr::Constant(7), Expr::BitwiseAnd(CloneExpr(*basis), Expr::Constant(0x0F))),
        Expr::Mul(
            Expr::Constant(0xF9), // -7 in 8-bit
            Expr::BitwiseAnd(CloneExpr(*basis), Expr::Constant(0xF0))
        )
    );

    auto result = run_semilinear(*input, { "x", "y" }, 8);
    ASSERT_NE(result, nullptr);

#ifdef COBRA_HAS_Z3
    auto verify = Z3VerifyExprs(*input, *result, { "x", "y" }, 8);
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

// Regression: mixed bitwise-over-arithmetic must reject, not silently drop
TEST(SemilinearIntegration, RejectMaskAndArith) {
    // (x + y) & 0x457 — AND over arithmetic, not purely bitwise
    auto input = Expr::BitwiseAnd(
        Expr::Add(Expr::Variable(0), Expr::Variable(1)), Expr::Constant(0x457)
    );
    auto ir = NormalizeToSemilinear(*input, { "x", "y" }, 64);
    EXPECT_FALSE(ir.has_value()) << "Expected normalization to fail for AND-over-arithmetic";
    if (!ir.has_value()) { EXPECT_EQ(ir.error().code, CobraError::kNonLinearInput); }
}

TEST(SemilinearIntegration, RejectNotOverArith) {
    // ~(x + y) — NOT over arithmetic
    auto input = Expr::BitwiseNot(Expr::Add(Expr::Variable(0), Expr::Variable(1)));
    auto ir    = NormalizeToSemilinear(*input, { "x", "y" }, 64);
    EXPECT_FALSE(ir.has_value()) << "Expected normalization to fail for NOT-over-arithmetic";
    if (!ir.has_value()) { EXPECT_EQ(ir.error().code, CobraError::kNonLinearInput); }
}

TEST(SemilinearIntegration, RejectXorOverArith) {
    // (x * 3 + y) ^ 0xFF — XOR over arithmetic with variable-dependent
    // non-pure operand (the XOR/OR constant lowering fires only when
    // IsPurelyBitwise is true, so this reaches the bottom of CollectTerms)
    auto arith = Expr::Add(Expr::Mul(Expr::Constant(3), Expr::Variable(0)), Expr::Variable(1));
    auto input = Expr::BitwiseXor(std::move(arith), Expr::Constant(0xFF));
    auto ir    = NormalizeToSemilinear(*input, { "x", "y" }, 64);
    EXPECT_FALSE(ir.has_value()) << "Expected normalization to fail for XOR-over-arithmetic";
    if (!ir.has_value()) { EXPECT_EQ(ir.error().code, CobraError::kNonLinearInput); }
}

// --- Orchestrator pass-level subproblem propagation tests ---

namespace {

    OrchestratorContext
    MakeSemilinearCtx(const Options &opts, const std::vector< std::string > &vars) {
        return OrchestratorContext{
            .opts          = opts,
            .original_vars = vars,
            .evaluator =
                opts.evaluator ? std::optional< Evaluator >(opts.evaluator) : std::nullopt,
            .bitwidth = opts.bitwidth,
        };
    }

} // namespace

TEST(SemilinearSubproblem, LocalVarsPreservedThroughPipeline) {
    // ctx.original_vars = {"x", "y"} (2 vars globally),
    // but the subproblem AST has solve_ctx.vars = {"x"} (1 var).
    // The semilinear pipeline should use the 1-var space throughout.
    // Input: (x & 0xFF) + 1 in the 1-var space.
    std::vector< std::string > global_vars = { "x", "y" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeSemilinearCtx(opts, global_vars);

    auto input =
        Expr::Add(Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)), Expr::Constant(1));
    auto cls = ClassifyStructural(*input);
    ASSERT_EQ(cls.semantic, SemanticClass::kSemilinear);

    WorkItem seed;
    seed.payload = AstPayload{
        .expr           = CloneExpr(*input),
        .classification = cls,
        .provenance     = Provenance::kRewritten,
        .solve_ctx      = AstSolveContext{ .vars = { "x" } },
    };
    seed.features.classification = cls;
    seed.features.provenance     = Provenance::kRewritten;

    // Step 1: Normalize
    auto r1 = RunSemilinearNormalize(seed, ctx);
    ASSERT_TRUE(r1.has_value());
    ASSERT_EQ(r1.value().decision, PassDecision::kAdvance);
    ASSERT_EQ(r1.value().next.size(), 1);
    auto &norm_item = r1.value().next[0];
    auto &norm_ctx  = std::get< NormalizedSemilinearPayload >(norm_item.payload).ctx;
    EXPECT_EQ(norm_ctx.vars.size(), 1);
    EXPECT_EQ(norm_ctx.vars[0], "x");

    // Step 2: Check
    auto r2 = RunSemilinearCheck(norm_item, ctx);
    ASSERT_TRUE(r2.has_value());
    ASSERT_EQ(r2.value().decision, PassDecision::kAdvance);
    ASSERT_EQ(r2.value().next.size(), 1);
    auto &checked_item = r2.value().next[0];
    auto &checked_ctx  = std::get< CheckedSemilinearPayload >(checked_item.payload).ctx;
    EXPECT_EQ(checked_ctx.vars.size(), 1);
    EXPECT_EQ(checked_ctx.vars[0], "x");

    // Step 3: Rewrite
    auto r3 = RunSemilinearRewrite(checked_item, ctx);
    ASSERT_TRUE(r3.has_value());
    ASSERT_EQ(r3.value().decision, PassDecision::kAdvance);
    ASSERT_EQ(r3.value().next.size(), 1);
    auto &rewritten_item = r3.value().next[0];
    auto &rewritten_ctx  = std::get< RewrittenSemilinearPayload >(rewritten_item.payload).ctx;
    EXPECT_EQ(rewritten_ctx.vars.size(), 1);
    EXPECT_EQ(rewritten_ctx.vars[0], "x");

    // Step 4: Reconstruct
    auto r4 = RunSemilinearReconstruct(rewritten_item, ctx);
    ASSERT_TRUE(r4.has_value());
    ASSERT_EQ(r4.value().decision, PassDecision::kSolvedCandidate);
    ASSERT_EQ(r4.value().next.size(), 1);
    auto &cand = std::get< CandidatePayload >(r4.value().next[0].payload);
    // Final candidate real_vars should come from the subproblem
    EXPECT_EQ(cand.real_vars.size(), 1);
    EXPECT_EQ(cand.real_vars[0], "x");
}

TEST(SemilinearSubproblem, GroupIdPreservedThroughPipeline) {
    std::vector< std::string > global_vars = { "x", "y" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeSemilinearCtx(opts, global_vars);

    auto input =
        Expr::Add(Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)), Expr::Constant(1));
    auto cls = ClassifyStructural(*input);

    WorkItem seed;
    seed.payload = AstPayload{
        .expr           = CloneExpr(*input),
        .classification = cls,
        .provenance     = Provenance::kRewritten,
        .solve_ctx      = AstSolveContext{ .vars = { "x" } },
    };
    seed.features.classification = cls;
    seed.features.provenance     = Provenance::kRewritten;
    seed.group_id                = 42;

    auto r1 = RunSemilinearNormalize(seed, ctx);
    ASSERT_TRUE(r1.has_value());
    ASSERT_EQ(r1.value().next.size(), 1);
    EXPECT_EQ(r1.value().next[0].group_id, 42);

    auto r2 = RunSemilinearCheck(r1.value().next[0], ctx);
    ASSERT_TRUE(r2.has_value());
    ASSERT_EQ(r2.value().next.size(), 1);
    EXPECT_EQ(r2.value().next[0].group_id, 42);

    auto r3 = RunSemilinearRewrite(r2.value().next[0], ctx);
    ASSERT_TRUE(r3.has_value());
    ASSERT_EQ(r3.value().next.size(), 1);
    EXPECT_EQ(r3.value().next[0].group_id, 42);

    auto r4 = RunSemilinearReconstruct(r3.value().next[0], ctx);
    ASSERT_TRUE(r4.has_value());
    ASSERT_EQ(r4.value().next.size(), 1);
    EXPECT_EQ(r4.value().next[0].group_id, 42);
}

// --- Reconstruct preserves group_id ---

TEST(SemilinearSubproblem, ReconstructPreservesGroupId) {
    // Run a grouped semilinear subproblem through reconstruct
    // and verify the emitted CandidatePayload inherits group_id.
    std::vector< std::string > global_vars = { "x", "y" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeSemilinearCtx(opts, global_vars);

    auto input =
        Expr::Add(Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)), Expr::Constant(1));
    auto cls = ClassifyStructural(*input);

    WorkItem seed;
    seed.payload = AstPayload{
        .expr           = CloneExpr(*input),
        .classification = cls,
        .provenance     = Provenance::kRewritten,
        .solve_ctx      = AstSolveContext{ .vars = { "x" } },
    };
    seed.features.classification = cls;
    seed.features.provenance     = Provenance::kRewritten;
    seed.group_id                = 99;

    auto r1 = RunSemilinearNormalize(seed, ctx);
    ASSERT_TRUE(r1.has_value());
    ASSERT_EQ(r1.value().next.size(), 1);
    auto r2 = RunSemilinearCheck(r1.value().next[0], ctx);
    ASSERT_TRUE(r2.has_value());
    ASSERT_EQ(r2.value().next.size(), 1);
    auto r3 = RunSemilinearRewrite(r2.value().next[0], ctx);
    ASSERT_TRUE(r3.has_value());
    ASSERT_EQ(r3.value().next.size(), 1);
    auto r4 = RunSemilinearReconstruct(r3.value().next[0], ctx);
    ASSERT_TRUE(r4.has_value());
    ASSERT_EQ(r4.value().decision, PassDecision::kSolvedCandidate);
    ASSERT_EQ(r4.value().next.size(), 1);

    // The candidate must inherit the group_id.
    ASSERT_TRUE(r4.value().next[0].group_id.has_value());
    EXPECT_EQ(*r4.value().next[0].group_id, 99);
}

// Issue #7: per-partition coefficient elision via NOT-AND lowering
TEST(SemilinearIntegration, Issue7NotAndElision) {
    // (x | 0x10) - (~x & 0x10) - (x & 0xEF) = (x & 0x10) in 8-bit
    auto input = Expr::Add(
        Expr::Add(
            Expr::BitwiseOr(Expr::Variable(0), Expr::Constant(0x10)),
            Expr::Negate(
                Expr::BitwiseAnd(Expr::BitwiseNot(Expr::Variable(0)), Expr::Constant(0x10))
            )
        ),
        Expr::Negate(Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xEF)))
    );
    EXPECT_EQ(ClassifyStructural(*input).semantic, SemanticClass::kSemilinear);

    auto result = run_semilinear(*input, { "x" }, 8);
    ASSERT_NE(result, nullptr);

    auto rendered = Render(*result, { "x" }, 8);
    EXPECT_EQ(rendered.find("~"), std::string::npos)
        << "Expected no NOT in simplified output, got: " << rendered;
    EXPECT_EQ(rendered.find("|"), std::string::npos)
        << "Expected no OR in simplified output, got: " << rendered;

#ifdef COBRA_HAS_Z3
    auto verify = Z3VerifyExprs(*input, *result, { "x" }, 8);
    EXPECT_TRUE(verify.equivalent) << "Counterexample: " << verify.counterexample;
#endif
}
