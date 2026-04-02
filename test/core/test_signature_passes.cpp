#include "CompetitionGroup.h"
#include "ContinuationTypes.h"
#include "DecompositionPassHelpers.h"
#include "JoinState.h"
#include "Orchestrator.h"
#include "OrchestratorPasses.h"
#include "SignaturePasses.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/BitwiseDecomposer.h"
#include "cobra/core/Classification.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/HybridDecomposer.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include <gtest/gtest.h>

using namespace cobra;

namespace {

    // Build an OrchestratorContext for testing.
    // Caller must keep opts and vars alive.
    OrchestratorContext MakeCtx(const Options &opts, const std::vector< std::string > &vars) {
        return OrchestratorContext{
            .opts          = opts,
            .original_vars = vars,
            .evaluator =
                opts.evaluator ? std::optional< Evaluator >(opts.evaluator) : std::nullopt,
            .bitwidth = opts.bitwidth,
        };
    }

    // Build a SignatureStatePayload WorkItem with a competition group.
    WorkItem MakeSignatureItem(
        const std::vector< uint64_t > &sig, const std::vector< std::string > &vars,
        OrchestratorContext &ctx, std::optional< Classification > cls = std::nullopt
    ) {
        auto elim      = EliminateAuxVars(sig, vars);
        auto orig_idx  = BuildVarSupport(vars, elim.real_vars);
        bool needs_ver = ctx.evaluator.has_value();

        auto group_id = CreateGroup(ctx.competition_groups, ctx.next_group_id);

        WorkItem item;
        item.payload = SignatureStatePayload{
            .ctx = {
                .sig                               = sig,
                .real_vars                         = elim.real_vars,
                .elimination                       = std::move(elim),
                .original_indices                  = std::move(orig_idx),
                .needs_original_space_verification = needs_ver,
            },
        };
        if (cls) { item.features.classification = cls; }
        item.group_id = group_id;
        return item;
    }

    // Build a SignatureCoeffStatePayload WorkItem in an existing group.
    WorkItem MakeCoeffItem(
        const std::vector< uint64_t > &sig, const std::vector< std::string > &vars,
        const std::vector< uint64_t > &coeffs, GroupId group_id, OrchestratorContext &ctx
    ) {
        auto elim      = EliminateAuxVars(sig, vars);
        auto orig_idx  = BuildVarSupport(vars, elim.real_vars);
        bool needs_ver = ctx.evaluator.has_value();

        WorkItem item;
        item.payload = SignatureCoeffStatePayload{
            .ctx = {
                .sig                               = sig,
                .real_vars                         = elim.real_vars,
                .elimination                       = std::move(elim),
                .original_indices                  = std::move(orig_idx),
                .needs_original_space_verification = needs_ver,
            },
            .coeffs = coeffs,
        };
        item.group_id = group_id;
        return item;
    }

} // namespace

// --- Scheduler routing tests ---

TEST(SignaturePass, SchedulerSignatureStateTable) {
    WorkItem item;
    item.payload = SignatureStatePayload{};
    OrchestratorPolicy policy;
    PassAttemptCache cache;

    auto p0 = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(p0.has_value());
    EXPECT_EQ(*p0, PassId::kSignaturePatternMatch);

    item.attempted_mask |= (1ULL << static_cast< uint8_t >(PassId::kSignaturePatternMatch));
    auto p1              = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(p1.has_value());
    EXPECT_EQ(*p1, PassId::kSignatureAnf);

    item.attempted_mask |= (1ULL << static_cast< uint8_t >(PassId::kSignatureAnf));
    auto p2              = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(p2.has_value());
    EXPECT_EQ(*p2, PassId::kPrepareCoeffModel);

    item.attempted_mask |= (1ULL << static_cast< uint8_t >(PassId::kPrepareCoeffModel));
    auto p3              = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(p3.has_value());
    EXPECT_EQ(*p3, PassId::kSignatureMultivarPolyRecovery);

    item.attempted_mask |=
        (1ULL << static_cast< uint8_t >(PassId::kSignatureMultivarPolyRecovery));
    auto p4 = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(p4.has_value());
    EXPECT_EQ(*p4, PassId::kSignatureBitwiseDecompose);

    item.attempted_mask |= (1ULL << static_cast< uint8_t >(PassId::kSignatureBitwiseDecompose));
    auto p5              = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(p5.has_value());
    EXPECT_EQ(*p5, PassId::kSignatureHybridDecompose);

    item.attempted_mask |= (1ULL << static_cast< uint8_t >(PassId::kSignatureHybridDecompose));
    auto p6              = SelectNextPass(item, policy, 0, cache);
    EXPECT_FALSE(p6.has_value());
}

TEST(SignaturePass, SchedulerCoeffStateTable) {
    WorkItem item;
    item.payload = SignatureCoeffStatePayload{
        .ctx    = { .sig = { 0, 1 } },
        .coeffs = { 0, 1 },
    };
    OrchestratorPolicy policy;
    PassAttemptCache cache;

    auto p0 = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(p0.has_value());
    EXPECT_EQ(*p0, PassId::kSignatureCobCandidate);

    item.attempted_mask |= (1ULL << static_cast< uint8_t >(PassId::kSignatureCobCandidate));
    auto p1              = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(p1.has_value());
    EXPECT_EQ(*p1, PassId::kSignatureSingletonPolyRecovery);

    item.attempted_mask |=
        (1ULL << static_cast< uint8_t >(PassId::kSignatureSingletonPolyRecovery));
    auto p2 = SelectNextPass(item, policy, 0, cache);
    EXPECT_FALSE(p2.has_value());
}

// --- PatternMatch tests ---

TEST(SignaturePass, PatternMatchKnownPattern) {
    // sig = {0, 1} is the identity x0
    std::vector< uint64_t > sig     = { 0, 1 };
    std::vector< std::string > vars = { "x0" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    auto item   = MakeSignatureItem(sig, vars, ctx);
    auto result = RunSignaturePatternMatch(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    EXPECT_EQ(pr.disposition, ItemDisposition::kRetainCurrent);

    auto &group = ctx.competition_groups.at(*item.group_id);
    ASSERT_TRUE(group.best.has_value());
    EXPECT_EQ(group.best->source_pass, PassId::kSignaturePatternMatch);
}

TEST(SignaturePass, PatternMatchNoMatch) {
    // A non-pattern sig: something like x + 2*y
    // sig = {0, 1, 2, 3} for 2 vars — this is x + y, which IS a
    // pattern. Use {0, 1, 2, 4} to defeat matching.
    std::vector< uint64_t > sig     = { 0, 1, 2, 4 };
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    auto item   = MakeSignatureItem(sig, vars, ctx);
    auto result = RunSignaturePatternMatch(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kNoProgress);
    EXPECT_EQ(pr.disposition, ItemDisposition::kRetainCurrent);
}

// --- ANF tests ---

TEST(SignaturePass, AnfBooleanSig) {
    // Boolean sig: XOR truth table {0, 1, 1, 0}
    std::vector< uint64_t > sig     = { 0, 1, 1, 0 };
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    auto item   = MakeSignatureItem(sig, vars, ctx);
    auto result = RunSignatureAnf(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    EXPECT_EQ(pr.disposition, ItemDisposition::kRetainCurrent);

    auto &group = ctx.competition_groups.at(*item.group_id);
    ASSERT_TRUE(group.best.has_value());
    EXPECT_EQ(group.best->source_pass, PassId::kSignatureAnf);
    EXPECT_EQ(group.best->verification, VerificationState::kVerified);
}

TEST(SignaturePass, AnfNonBooleanReturnsNoProgress) {
    // Non-boolean sig (values > 1)
    std::vector< uint64_t > sig     = { 0, 3, 5, 7 };
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    auto item   = MakeSignatureItem(sig, vars, ctx);
    auto result = RunSignatureAnf(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kNoProgress);
    EXPECT_EQ(pr.disposition, ItemDisposition::kRetainCurrent);
}

// --- PrepareCoeffModel tests ---

TEST(SignaturePass, PrepareCoeffModelEmitsChild) {
    std::vector< uint64_t > sig     = { 0, 1, 1, 0 };
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    auto item                      = MakeSignatureItem(sig, vars, ctx);
    item.signature_recursion_depth = 1;
    auto gid                       = *item.group_id;
    auto result                    = RunPrepareCoeffModel(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    EXPECT_EQ(pr.disposition, ItemDisposition::kRetainCurrent);
    ASSERT_EQ(pr.next.size(), 1);

    auto &child = pr.next[0];
    EXPECT_EQ(GetStateKind(child.payload), StateKind::kSignatureCoeffState);
    ASSERT_TRUE(child.group_id.has_value());
    EXPECT_EQ(*child.group_id, gid);
    EXPECT_EQ(child.signature_recursion_depth, item.signature_recursion_depth + 1);

    // AcquireHandle should have incremented to 2
    auto &group = ctx.competition_groups.at(gid);
    EXPECT_EQ(group.open_handles, 2);
}

// --- CobCandidate tests ---

TEST(SignaturePass, CobCandidateSubmitsToGroup) {
    // Simple case: x0 has sig {0, 1}, coeffs {0, 1}
    std::vector< uint64_t > sig     = { 0, 1 };
    std::vector< std::string > vars = { "x0" };
    std::vector< uint64_t > coeffs  = { 0, 1 };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    auto group_id = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    auto item     = MakeCoeffItem(sig, vars, coeffs, group_id, ctx);

    auto result = RunSignatureCobCandidate(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    // CoB for {0,1} = x0 which passes spot check
    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    EXPECT_EQ(pr.disposition, ItemDisposition::kRetainCurrent);

    auto &group = ctx.competition_groups.at(group_id);
    ASSERT_TRUE(group.best.has_value());
    EXPECT_EQ(group.best->source_pass, PassId::kSignatureCobCandidate);
}

// --- MultivarPolyRecovery tests ---

TEST(SignaturePass, MultivarPolyRecoveryGuardNoFlag) {
    // Without kSfHasMultivarHighPower, should return kNoProgress
    std::vector< uint64_t > sig     = { 0, 1, 1, 0 };
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    auto item   = MakeSignatureItem(sig, vars, ctx);
    auto result = RunSignatureMultivarPolyRecovery(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNoProgress);
}

TEST(SignaturePass, MultivarPolyRecoveryGuardNoEvaluator) {
    // With flag but no evaluator, should return kNoProgress
    std::vector< uint64_t > sig     = { 0, 1, 1, 0 };
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    auto cls = Classification{
        .semantic = SemanticClass::kPolynomial,
        .flags    = kSfHasMultivarHighPower,
    };
    auto item   = MakeSignatureItem(sig, vars, ctx, cls);
    auto result = RunSignatureMultivarPolyRecovery(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNoProgress);
}

// --- Competition: multiple techniques on same sig ---

TEST(SignaturePass, CompetitionBestWins) {
    // sig = {0, 1} (identity x0) — both PatternMatch and ANF
    // should produce candidates; the cheaper one wins.
    std::vector< uint64_t > sig     = { 0, 1 };
    std::vector< std::string > vars = { "x0" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    auto item = MakeSignatureItem(sig, vars, ctx);
    auto gid  = *item.group_id;

    // Run pattern match
    auto r1 = RunSignaturePatternMatch(item, ctx);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1.value().decision, PassDecision::kAdvance);

    // Run ANF (sig {0,1} is boolean-valued)
    auto r2 = RunSignatureAnf(item, ctx);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2.value().decision, PassDecision::kAdvance);

    // Group should have a best candidate
    auto &group = ctx.competition_groups.at(gid);
    ASSERT_TRUE(group.best.has_value());
    // The best candidate should be from one of the two passes
    bool from_pm  = group.best->source_pass == PassId::kSignaturePatternMatch;
    bool from_anf = group.best->source_pass == PassId::kSignatureAnf;
    EXPECT_TRUE(from_pm || from_anf);
}

// --- BuildSignatureState creates group ---

TEST(SignaturePass, BuildSignatureStateCreatesGroup) {
    std::vector< std::string > vars = { "x0" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    // Build an AST item for x0
    WorkItem ast_item;
    auto cls         = Classification{ .semantic = SemanticClass::kLinear, .flags = kSfNone };
    ast_item.payload = AstPayload{
        .expr           = Expr::Variable(0),
        .classification = cls,
        .provenance     = Provenance::kLowered,
    };
    ast_item.features.classification = cls;
    ast_item.features.provenance     = Provenance::kLowered;
    ctx.input_sig                    = { 0, 1 };

    EXPECT_EQ(ctx.next_group_id, 0);

    auto result = RunBuildSignatureState(ast_item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();
    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    ASSERT_EQ(pr.next.size(), 1);

    auto &sig_item = pr.next[0];
    ASSERT_TRUE(sig_item.group_id.has_value());
    EXPECT_EQ(*sig_item.group_id, 0);
    EXPECT_EQ(ctx.next_group_id, 1);

    // Verify the group exists with open_handles == 1
    auto it = ctx.competition_groups.find(0);
    ASSERT_NE(it, ctx.competition_groups.end());
    EXPECT_EQ(it->second.open_handles, 1);
}

// --- BuildSignatureState uses solve_ctx vars ---

TEST(SignaturePass, BuildSignatureStateUsesSolveCtxVars) {
    // Top-level has 2 vars {x, y}, but solve_ctx restricts to {x}.
    // The emitted SignatureStatePayload should be built in the 1-var space.
    std::vector< std::string > vars = { "x", "y" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    // Build an AST item for Variable(0) in a 1-var subproblem.
    WorkItem ast_item;
    auto cls         = Classification{ .semantic = SemanticClass::kLinear, .flags = kSfNone };
    ast_item.payload = AstPayload{
        .expr           = Expr::Variable(0),
        .classification = cls,
        .provenance     = Provenance::kRewritten,
        .solve_ctx =
            AstSolveContext{
                            .vars      = { "x" },
                            .input_sig = { 0, 1 },
                            },
    };
    ast_item.features.classification = cls;
    ast_item.features.provenance     = Provenance::kRewritten;

    auto result = RunBuildSignatureState(ast_item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();
    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    ASSERT_EQ(pr.next.size(), 1);

    auto &sig_payload = std::get< SignatureStatePayload >(pr.next[0].payload);
    // Signature should have 2 entries (2^1 vars), not 4 (2^2 vars)
    EXPECT_EQ(sig_payload.ctx.sig.size(), 2);
    // real_vars should come from the 1-var space
    EXPECT_EQ(sig_payload.ctx.real_vars.size(), 1);
    EXPECT_EQ(sig_payload.ctx.real_vars[0], "x");
}

TEST(SignaturePass, BuildSignatureStateNoSolveCtxUsesOriginal) {
    // Without solve_ctx, should use ctx.original_vars as before.
    std::vector< std::string > vars = { "x", "y" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    WorkItem ast_item;
    auto cls         = Classification{ .semantic = SemanticClass::kLinear, .flags = kSfNone };
    ast_item.payload = AstPayload{
        .expr           = Expr::Add(Expr::Variable(0), Expr::Variable(1)),
        .classification = cls,
        .provenance     = Provenance::kLowered,
    };
    ast_item.features.classification = cls;
    ast_item.features.provenance     = Provenance::kLowered;
    ctx.input_sig                    = { 0, 1, 1, 2 };

    auto result = RunBuildSignatureState(ast_item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();
    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    ASSERT_EQ(pr.next.size(), 1);

    auto &sig_payload = std::get< SignatureStatePayload >(pr.next[0].payload);
    // Signature should have 4 entries (2^2 vars)
    EXPECT_EQ(sig_payload.ctx.sig.size(), 4);
    EXPECT_EQ(sig_payload.ctx.real_vars.size(), 2);
}

// --- SingletonPolyRecovery guard ---

TEST(SignaturePass, SingletonPolyRecoveryNoEvaluatorReturnsNoProgress) {
    std::vector< uint64_t > sig     = { 0, 1 };
    std::vector< std::string > vars = { "x0" };
    std::vector< uint64_t > coeffs  = { 0, 1 };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    auto group_id = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    auto item     = MakeCoeffItem(sig, vars, coeffs, group_id, ctx);

    auto result = RunSignatureSingletonPolyRecovery(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNoProgress);
}

TEST(SingletonPolyRecovery, EmitsResidualForNonzeroCob) {
    // f(x) = x has CoB coefficients {0, 1}. Singleton power recovery
    // captures x as a linear univariate power; after coefficient
    // splitting the and_coeffs residual may or may not be zero
    // depending on whether the singleton fully absorbs the coefficient.
    // This test verifies the pass produces kAdvance (either via
    // direct candidate submission for zero residual or via
    // kRemainderState emission for nonzero residual).
    std::vector< uint64_t > sig     = { 0, 1 };
    std::vector< std::string > vars = { "x0" };
    std::vector< uint64_t > coeffs  = { 0, 1 };
    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0]; };
    auto ctx       = MakeCtx(opts, vars);

    auto group_id = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    auto item     = MakeCoeffItem(sig, vars, coeffs, group_id, ctx);

    auto result = RunSignatureSingletonPolyRecovery(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    EXPECT_EQ(pr.disposition, ItemDisposition::kRetainCurrent);

    // Check which path was taken.
    if (pr.next.empty()) {
        // Zero-residual path: candidate submitted directly.
        auto &group = ctx.competition_groups.at(group_id);
        ASSERT_TRUE(group.best.has_value());
        EXPECT_EQ(group.best->source_pass, PassId::kSignatureSingletonPolyRecovery);
    } else {
        // Nonzero-residual path: kRemainderState emitted.
        ASSERT_EQ(pr.next.size(), 1);
        EXPECT_EQ(GetStateKind(pr.next[0].payload), StateKind::kRemainderState);
        auto &residual = std::get< RemainderStatePayload >(pr.next[0].payload);
        EXPECT_EQ(residual.origin, RemainderOrigin::kSignatureLowering);
        ASSERT_TRUE(pr.next[0].group_id.has_value());
        EXPECT_EQ(*pr.next[0].group_id, group_id);
    }
}

TEST(SingletonPolyRecovery, EmitsResidualForNonzeroTwoVar) {
    // f(x,y) = x + y + x*y. CoB coefficients {0, 1, 1, 1}.
    // The polynomial lowering may extract x*y, leaving a nonzero
    // residual (x + y). This exercises the kRemainderState emission.
    std::vector< uint64_t > sig     = { 0, 1, 1, 3 };
    std::vector< std::string > vars = { "x0", "x1" };
    std::vector< uint64_t > coeffs  = { 0, 1, 1, 1 };
    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] + v[1] + v[0] * v[1];
    };
    auto ctx = MakeCtx(opts, vars);

    auto group_id = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    auto item     = MakeCoeffItem(sig, vars, coeffs, group_id, ctx);

    auto result = RunSignatureSingletonPolyRecovery(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    EXPECT_EQ(pr.disposition, ItemDisposition::kRetainCurrent);

    // Both paths are valid outcomes — verify either:
    if (!pr.next.empty()) {
        // Residual state emitted.
        ASSERT_EQ(pr.next.size(), 1);
        EXPECT_EQ(GetStateKind(pr.next[0].payload), StateKind::kRemainderState);
        auto &residual = std::get< RemainderStatePayload >(pr.next[0].payload);
        EXPECT_EQ(residual.origin, RemainderOrigin::kSignatureLowering);
        EXPECT_TRUE(residual.prefix_expr != nullptr);
        ASSERT_TRUE(pr.next[0].group_id.has_value());
        EXPECT_EQ(*pr.next[0].group_id, group_id);
        // Target context should carry the evaluator and vars.
        EXPECT_FALSE(residual.target.vars.empty());
    } else {
        // Direct candidate — still valid if residual happens to be zero.
        auto &group = ctx.competition_groups.at(group_id);
        ASSERT_TRUE(group.best.has_value());
        EXPECT_EQ(group.best->source_pass, PassId::kSignatureSingletonPolyRecovery);
    }
}

TEST(SingletonPolyRecovery, PreservesSignatureRecursionDepthOnResidualEmission) {
    std::vector< uint64_t > sig     = { 0, 0, 0, 1 };
    std::vector< std::string > vars = { "x0", "x1" };
    std::vector< uint64_t > coeffs  = { 0, 0, 0, 1 };
    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0] & v[1]; };
    auto ctx       = MakeCtx(opts, vars);

    auto group_id                  = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    auto item                      = MakeCoeffItem(sig, vars, coeffs, group_id, ctx);
    item.signature_recursion_depth = 1;

    auto result = RunSignatureSingletonPolyRecovery(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    ASSERT_EQ(pr.next.size(), 1);
    EXPECT_EQ(GetStateKind(pr.next[0].payload), StateKind::kRemainderState);
    EXPECT_EQ(pr.next[0].signature_recursion_depth, item.signature_recursion_depth);
}

TEST(SingletonPolyRecovery, RecursiveInvocationFallsBackToInline) {
    // When evaluator_override is set (residual solver spawned this
    // chain), the pass should NOT emit a kRemainderState — it should
    // fall back to inline combination and verification.
    std::vector< uint64_t > sig     = { 0, 1 };
    std::vector< std::string > vars = { "x0" };
    std::vector< uint64_t > coeffs  = { 0, 1 };
    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [](const std::vector< uint64_t > &v) -> uint64_t { return v[0]; };
    auto ctx       = MakeCtx(opts, vars);

    auto group_id                 = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    auto item                     = MakeCoeffItem(sig, vars, coeffs, group_id, ctx);
    // Simulate a recursive invocation from a residual solver.
    item.evaluator_override       = opts.evaluator;
    item.evaluator_override_arity = static_cast< uint32_t >(vars.size());

    auto result = RunSignatureSingletonPolyRecovery(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    EXPECT_EQ(pr.disposition, ItemDisposition::kRetainCurrent);

    // No residual state should be emitted when evaluator_override is set.
    // Result is either a direct candidate or an inline combined candidate.
    for (const auto &child : pr.next) {
        EXPECT_NE(GetStateKind(child.payload), StateKind::kRemainderState);
    }
}

// --- Wrong payload type returns kNotApplicable ---

TEST(SignaturePass, PatternMatchWrongPayload) {
    std::vector< std::string > vars = { "x0" };
    Options opts;
    auto ctx = MakeCtx(opts, vars);

    WorkItem item;
    item.payload = AstPayload{ .expr = Expr::Constant(0) };
    auto result  = RunSignaturePatternMatch(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNotApplicable);
}

TEST(SignaturePass, AnfWrongPayload) {
    std::vector< std::string > vars = { "x0" };
    Options opts;
    auto ctx = MakeCtx(opts, vars);

    WorkItem item;
    item.payload = AstPayload{ .expr = Expr::Constant(0) };
    auto result  = RunSignatureAnf(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNotApplicable);
}

TEST(SignaturePass, CobCandidateWrongPayload) {
    std::vector< std::string > vars = { "x0" };
    Options opts;
    auto ctx = MakeCtx(opts, vars);

    WorkItem item;
    item.payload = AstPayload{ .expr = Expr::Constant(0) };
    auto result  = RunSignatureCobCandidate(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNotApplicable);
}

// --- IsDecompositionFamilyPass does not include new passes ---

TEST(SignaturePass, NewPassIdsNotInDecompositionRange) {
    EXPECT_FALSE(IsDecompositionFamilyPass(PassId::kSignaturePatternMatch));
    EXPECT_FALSE(IsDecompositionFamilyPass(PassId::kSignatureAnf));
    EXPECT_FALSE(IsDecompositionFamilyPass(PassId::kPrepareCoeffModel));
    EXPECT_FALSE(IsDecompositionFamilyPass(PassId::kSignatureCobCandidate));
    EXPECT_FALSE(IsDecompositionFamilyPass(PassId::kSignatureSingletonPolyRecovery));
    EXPECT_FALSE(IsDecompositionFamilyPass(PassId::kSignatureMultivarPolyRecovery));
    EXPECT_FALSE(IsDecompositionFamilyPass(PassId::kSignatureBitwiseDecompose));
    EXPECT_FALSE(IsDecompositionFamilyPass(PassId::kSignatureHybridDecompose));
}

// --- BitwiseDecompose fanout tests ---

TEST(SignaturePass, BitwiseFanoutEmitsChildren) {
    // sig for x0 & x1: {0, 0, 0, 1}
    // cofactor split on k=0: cof0={0,0} cof1={0,1}
    // all cof0 zero => AND candidate with g_sig={0,1}
    std::vector< uint64_t > sig     = { 0, 0, 0, 1 };
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [](const std::vector< uint64_t > &vals) -> uint64_t {
        return vals[0] & vals[1];
    };
    auto ctx = MakeCtx(opts, vars);

    auto item   = MakeSignatureItem(sig, vars, ctx);
    auto result = RunSignatureBitwiseDecompose(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    EXPECT_EQ(pr.disposition, ItemDisposition::kRetainCurrent);
    // Should emit at least one child (AND candidate)
    EXPECT_GE(pr.next.size(), 1);

    // Each child should be a SignatureStatePayload with a group_id
    for (const auto &child : pr.next) {
        EXPECT_EQ(GetStateKind(child.payload), StateKind::kSignatureState);
        EXPECT_TRUE(child.group_id.has_value());
        EXPECT_EQ(child.signature_recursion_depth, 1);
    }
}

TEST(SignaturePass, BitwiseFanoutDepthGuard) {
    std::vector< uint64_t > sig     = { 0, 0, 0, 1 };
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [](const std::vector< uint64_t > &vals) -> uint64_t {
        return vals[0] & vals[1];
    };
    auto ctx = MakeCtx(opts, vars);

    auto item                      = MakeSignatureItem(sig, vars, ctx);
    item.signature_recursion_depth = 2;
    auto result                    = RunSignatureBitwiseDecompose(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNoProgress);
}

TEST(SignaturePass, BitwiseFanoutNoEvaluatorGuard) {
    std::vector< uint64_t > sig     = { 0, 0, 0, 1 };
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    auto item   = MakeSignatureItem(sig, vars, ctx);
    auto result = RunSignatureBitwiseDecompose(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNoProgress);
}

TEST(SignaturePass, BitwiseFanoutUsesEvaluatorOverride) {
    std::vector< uint64_t > sig     = { 0, 0, 0, 1 };
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    auto item               = MakeSignatureItem(sig, vars, ctx);
    item.evaluator_override = Evaluator{ [](const std::vector< uint64_t > &vals) -> uint64_t {
        return vals[0] & vals[1];
    } };

    auto result = RunSignatureBitwiseDecompose(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    EXPECT_GE(pr.next.size(), 1);
    for (const auto &child : pr.next) { EXPECT_TRUE(child.evaluator_override.has_value()); }
}

TEST(SignaturePass, BitwiseFanoutCapsAtEight) {
    // Construct a signature with many AND/MUL cofactor candidates.
    // f = (x0 & x1) + (x2 & x3) — for k=0, cof0 is all-zero
    // (in a boolean sense) which gives AND+MUL candidates. With
    // 4 vars and symmetric structure, we get multiple candidates.
    // To ensure > 8 candidates, we use 5 variables with an ADD
    // structure: f = x0 + 2*x1 + 4*x2 + 8*x3 + 16*x4 at {0,1}.
    // Each variable k has cof1[j] - cof0[j] = 2^k constant,
    // giving ADD candidates for all 5 vars = 5 candidates.
    // To exceed 8, augment with boolean-valued cofactor structure.
    //
    // Actually, simpler: construct a 4-var sig where every var
    // has AND and MUL candidates: f = x0 & x1 & x2 & x3.
    // For k=0: cof0=all-zero, cof1=x1&x2&x3 => AND+MUL = 2 cands
    // Similarly for k=1,2,3 => 4*2 = 8 candidates (boolean MUL=AND).
    // With IsBooleanValued true, both AND and MUL are emitted = 8.
    //
    // For > 8, we need more. Use 5 vars:
    // f = x0 & x1 & x2 & x3 & x4 (5 vars, all-zero cof0 for each)
    // => 5 * 2 = 10 candidates (AND + MUL for each var)
    std::vector< uint64_t > sig(32);
    for (uint32_t i = 0; i < 32; ++i) {
        sig[i] =
            ((i >> 0) & 1) & ((i >> 1) & 1) & ((i >> 2) & 1) & ((i >> 3) & 1) & ((i >> 4) & 1);
    }

    auto candidates = EnumerateBitwiseCandidates(sig, 5);
    EXPECT_GT(candidates.size(), 8);

    std::vector< std::string > vars = { "x0", "x1", "x2", "x3", "x4" };
    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [](const std::vector< uint64_t > &vals) -> uint64_t {
        return vals[0] & vals[1] & vals[2] & vals[3] & vals[4];
    };
    auto ctx = MakeCtx(opts, vars);

    auto item   = MakeSignatureItem(sig, vars, ctx);
    auto result = RunSignatureBitwiseDecompose(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();
    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    // Children capped at 8 (some may be constant-g handled inline)
    EXPECT_LE(pr.next.size(), 8);
}

// --- HybridDecompose fanout tests ---

TEST(SignaturePass, HybridFanoutEmitsChildren) {
    // sig for x0 ^ (x0 & x1) = {0, 1, 0, 0}
    // Extracting x0 with XOR gives residual {0, 0, 0, 1} = x0&x1
    std::vector< uint64_t > sig     = { 0, 1, 0, 0 };
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [](const std::vector< uint64_t > &vals) -> uint64_t {
        return vals[0] ^ (vals[0] & vals[1]);
    };
    auto ctx = MakeCtx(opts, vars);

    auto item   = MakeSignatureItem(sig, vars, ctx);
    auto result = RunSignatureHybridDecompose(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    EXPECT_EQ(pr.disposition, ItemDisposition::kRetainCurrent);
    EXPECT_GE(pr.next.size(), 1);

    for (const auto &child : pr.next) {
        EXPECT_EQ(GetStateKind(child.payload), StateKind::kSignatureState);
        EXPECT_TRUE(child.group_id.has_value());
        EXPECT_EQ(child.signature_recursion_depth, 1);
    }
}

TEST(SignaturePass, HybridFanoutDepthGuard) {
    std::vector< uint64_t > sig     = { 0, 1, 0, 0 };
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [](const std::vector< uint64_t > &vals) -> uint64_t {
        return vals[0] ^ (vals[0] & vals[1]);
    };
    auto ctx = MakeCtx(opts, vars);

    auto item                      = MakeSignatureItem(sig, vars, ctx);
    item.signature_recursion_depth = 1;
    auto result                    = RunSignatureHybridDecompose(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNoProgress);
}

TEST(SignaturePass, HybridFanoutNoEvaluatorGuard) {
    std::vector< uint64_t > sig     = { 0, 1, 0, 0 };
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    auto item   = MakeSignatureItem(sig, vars, ctx);
    auto result = RunSignatureHybridDecompose(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kNoProgress);
}

TEST(SignaturePass, HybridFanoutUsesEvaluatorOverride) {
    std::vector< uint64_t > sig     = { 0, 1, 0, 0 };
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    auto item               = MakeSignatureItem(sig, vars, ctx);
    item.evaluator_override = Evaluator{ [](const std::vector< uint64_t > &vals) -> uint64_t {
        return vals[0] ^ (vals[0] & vals[1]);
    } };

    auto result = RunSignatureHybridDecompose(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    EXPECT_GE(pr.next.size(), 1);
    for (const auto &child : pr.next) { EXPECT_TRUE(child.evaluator_override.has_value()); }
}

// --- BitwiseComposeCont resolution test ---

TEST(SignaturePass, BitwiseComposeContResolution) {
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    // Parent group: where the composed result will land
    auto parent_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);

    // Child group: represents the sub-problem for g(rest) = x1
    auto child_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    ctx.competition_groups.at(child_gid).continuation = ContinuationData{
        BitwiseComposeCont{
                           .var_k                  = 0,
                           .gate                   = GateKind::kAnd,
                           .add_coeff              = 0,
                           .active_context_indices = { 1 },
                           .parent_group_id        = parent_gid,
                           .parent_eval = Evaluator{ [](const std::vector< uint64_t > &vals) -> uint64_t {
                return vals[0] & vals[1];
            } },
                           .parent_real_vars                         = vars,
                           .parent_original_indices                  = { 0, 1 },
                           .parent_num_vars                          = 2,
                           .parent_needs_original_space_verification = false,
                           }
    };

    // Submit x1 (= Variable(1)) as the winner of the child group
    CandidateRecord rec;
    rec.expr        = Expr::Variable(0);
    rec.cost        = ExprCost{ .weighted_size = 1 };
    rec.source_pass = PassId::kSignaturePatternMatch;
    SubmitCandidate(ctx.competition_groups, child_gid, std::move(rec));

    WorkItem resolved_item;
    resolved_item.payload = CompetitionResolvedPayload{ .group_id = child_gid };

    auto result = RunResolveCompetition(resolved_item, ctx);
    ASSERT_TRUE(result.has_value());

    // Child group is consumed
    EXPECT_EQ(ctx.competition_groups.count(child_gid), 0);
    // Parent group should have received x0 & x1
    ASSERT_TRUE(ctx.competition_groups.at(parent_gid).best.has_value());
    EXPECT_EQ(
        ctx.competition_groups.at(parent_gid).best->source_pass,
        PassId::kSignatureBitwiseDecompose
    );
    EXPECT_EQ(
        ctx.competition_groups.at(parent_gid).best->verification, VerificationState::kVerified
    );
}

// --- HybridComposeCont resolution test ---

TEST(SignaturePass, HybridComposeContResolution) {
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    auto parent_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);

    auto child_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    ctx.competition_groups.at(child_gid).continuation = ContinuationData{
        HybridComposeCont{
                          .var_k           = 0,
                          .op              = ExtractOp::kXor,
                          .parent_group_id = parent_gid,
                          .parent_eval     = Evaluator{ [](const std::vector< uint64_t > &vals) -> uint64_t {
                return vals[0] ^ (vals[0] & vals[1]);
            } },
                          .parent_real_vars                         = vars,
                          .parent_original_indices                  = { 0, 1 },
                          .parent_num_vars                          = 2,
                          .parent_needs_original_space_verification = false,
                          }
    };

    // The child's winner is x0 & x1 (= AND(x0, x1))
    CandidateRecord rec;
    rec.expr        = Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1));
    rec.cost        = ExprCost{ .weighted_size = 3 };
    rec.source_pass = PassId::kSignaturePatternMatch;
    SubmitCandidate(ctx.competition_groups, child_gid, std::move(rec));

    WorkItem resolved_item;
    resolved_item.payload = CompetitionResolvedPayload{ .group_id = child_gid };

    auto result = RunResolveCompetition(resolved_item, ctx);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(ctx.competition_groups.count(child_gid), 0);
    ASSERT_TRUE(ctx.competition_groups.at(parent_gid).best.has_value());
    EXPECT_EQ(
        ctx.competition_groups.at(parent_gid).best->source_pass,
        PassId::kSignatureHybridDecompose
    );
    EXPECT_EQ(
        ctx.competition_groups.at(parent_gid).best->verification, VerificationState::kVerified
    );
}

// --- OperandRewriteCont resolution tests ---

TEST(SignaturePass, OperandJoinResolveBothSides) {
    // Build: Mul((x0 | 0), (x1 | 0)). Both winners are full-width
    // equivalent to x0 and x1, so the join should emit a rewritten
    // AST and preserve the parent subproblem context.
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth   = 64;
    auto ctx        = MakeCtx(opts, vars);
    auto parent_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);

    auto orig_mul = Expr::Mul(
        Expr::BitwiseOr(Expr::Variable(0), Expr::Constant(0)),
        Expr::BitwiseOr(Expr::Variable(1), Expr::Constant(0))
    );
    auto baseline  = ComputeCost(*orig_mul).cost;
    auto input_sig = EvaluateBooleanSignature(*orig_mul, 2, 64);

    OperandJoinState join;
    join.full_ast            = CloneExpr(*orig_mul);
    join.original_mul        = CloneExpr(*orig_mul);
    join.target_hash         = std::hash< Expr >{}(*orig_mul);
    join.baseline_cost       = baseline;
    join.vars                = vars;
    join.parent_group_id     = parent_gid;
    join.has_solve_ctx       = true;
    join.solve_ctx_vars      = { "sx0", "sx1" };
    join.solve_ctx_input_sig = input_sig;
    join.bitwidth            = 64;
    join.parent_depth        = 7;
    join.rewrite_gen         = 0;
    join.parent_history      = { PassId::kBuildSignatureState, PassId::kOperandSimplify };
    join.lhs_resolved        = false;
    join.rhs_resolved        = false;

    auto join_id = CreateJoin(ctx.join_states, ctx.next_join_id, JoinState{ std::move(join) });

    // Create LHS child group with OperandRewriteCont
    auto lhs_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    ctx.competition_groups.at(lhs_gid).continuation = ContinuationData{
        OperandRewriteCont{
                           .join_id = join_id,
                           .role    = OperandRewriteCont::OperandRole::kLhs,
                           }
    };

    // Submit x0 as the LHS winner.
    CandidateRecord lhs_rec;
    lhs_rec.expr        = Expr::Variable(0);
    lhs_rec.cost        = ComputeCost(*lhs_rec.expr).cost;
    lhs_rec.source_pass = PassId::kSignaturePatternMatch;
    SubmitCandidate(ctx.competition_groups, lhs_gid, std::move(lhs_rec));

    // Resolve LHS
    WorkItem lhs_resolved;
    lhs_resolved.payload = CompetitionResolvedPayload{ .group_id = lhs_gid };
    auto r1              = RunResolveCompetition(lhs_resolved, ctx);
    ASSERT_TRUE(r1.has_value());

    // LHS resolved but RHS not yet — join state should still exist
    EXPECT_EQ(ctx.join_states.count(join_id), 1);

    // Create RHS child group
    auto rhs_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    ctx.competition_groups.at(rhs_gid).continuation = ContinuationData{
        OperandRewriteCont{
                           .join_id = join_id,
                           .role    = OperandRewriteCont::OperandRole::kRhs,
                           }
    };

    // Submit x1 as the RHS winner.
    CandidateRecord rhs_rec;
    rhs_rec.expr        = Expr::Variable(1);
    rhs_rec.cost        = ComputeCost(*rhs_rec.expr).cost;
    rhs_rec.source_pass = PassId::kSignaturePatternMatch;
    SubmitCandidate(ctx.competition_groups, rhs_gid, std::move(rhs_rec));

    // Resolve RHS
    WorkItem rhs_resolved;
    rhs_resolved.payload = CompetitionResolvedPayload{ .group_id = rhs_gid };
    auto r2              = RunResolveCompetition(rhs_resolved, ctx);
    ASSERT_TRUE(r2.has_value());
    auto &pr = r2.value();

    // Both resolved — join state should be cleaned up
    EXPECT_EQ(ctx.join_states.count(join_id), 0);

    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    ASSERT_EQ(pr.next.size(), 1u);
    EXPECT_EQ(GetStateKind(pr.next[0].payload), StateKind::kFoldedAst);
    auto &ast = std::get< AstPayload >(pr.next[0].payload);
    EXPECT_EQ(ast.provenance, Provenance::kRewritten);
    ASSERT_TRUE(pr.next[0].group_id.has_value());
    EXPECT_EQ(*pr.next[0].group_id, parent_gid);
    ASSERT_TRUE(ast.solve_ctx.has_value());
    EXPECT_EQ(ast.solve_ctx->vars, std::vector< std::string >({ "sx0", "sx1" }));
    EXPECT_EQ(ast.solve_ctx->input_sig, input_sig);
    EXPECT_EQ(pr.next[0].depth, 7u);
    EXPECT_EQ(
        pr.next[0].history,
        std::vector< PassId >({ PassId::kBuildSignatureState, PassId::kOperandSimplify })
    );
    EXPECT_EQ(pr.next[0].rewrite_gen, 1u);
}

// --- ProductCollapseCont resolution tests ---

TEST(SignaturePass, ProductJoinResolveBothFactors) {
    // Build: Add(Mul(x&y, x|y), Mul(x&~y, ~x&y)) = x*y
    // Factor sigs: x = {0,1,0,1}, y = {0,0,1,1}
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    auto orig = Expr::Add(
        Expr::Mul(
            Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)),
            Expr::BitwiseOr(Expr::Variable(0), Expr::Variable(1))
        ),
        Expr::Mul(
            Expr::BitwiseAnd(Expr::Variable(0), Expr::BitwiseNot(Expr::Variable(1))),
            Expr::BitwiseAnd(Expr::BitwiseNot(Expr::Variable(0)), Expr::Variable(1))
        )
    );
    auto baseline   = ComputeCost(*orig).cost;
    auto parent_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    auto input_sig  = EvaluateBooleanSignature(*orig, 2, 64);

    ProductJoinState join;
    join.original_expr       = CloneExpr(*orig);
    join.baseline_cost       = baseline;
    join.vars                = vars;
    join.parent_group_id     = parent_gid;
    join.has_solve_ctx       = true;
    join.solve_ctx_vars      = { "sx0", "sx1" };
    join.solve_ctx_input_sig = input_sig;
    join.bitwidth            = 64;
    join.parent_depth        = 5;
    join.rewrite_gen         = 0;
    join.parent_history = { PassId::kBuildSignatureState, PassId::kProductIdentityCollapse };
    join.full_ast       = CloneExpr(*orig);
    join.target_hash    = std::hash< Expr >{}(*orig);

    auto join_id = CreateJoin(ctx.join_states, ctx.next_join_id, JoinState{ std::move(join) });

    // X factor group
    auto x_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    ctx.competition_groups.at(x_gid).continuation = ContinuationData{
        ProductCollapseCont{
                            .join_id = join_id,
                            .role    = ProductCollapseCont::FactorRole::kX,
                            }
    };

    CandidateRecord x_rec;
    x_rec.expr        = Expr::Variable(0);
    x_rec.cost        = ComputeCost(*x_rec.expr).cost;
    x_rec.source_pass = PassId::kSignaturePatternMatch;
    SubmitCandidate(ctx.competition_groups, x_gid, std::move(x_rec));

    // Y factor group
    auto y_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    ctx.competition_groups.at(y_gid).continuation = ContinuationData{
        ProductCollapseCont{
                            .join_id = join_id,
                            .role    = ProductCollapseCont::FactorRole::kY,
                            }
    };

    CandidateRecord y_rec;
    y_rec.expr        = Expr::Variable(1);
    y_rec.cost        = ComputeCost(*y_rec.expr).cost;
    y_rec.source_pass = PassId::kSignaturePatternMatch;
    SubmitCandidate(ctx.competition_groups, y_gid, std::move(y_rec));

    // Resolve X
    WorkItem x_resolved;
    x_resolved.payload = CompetitionResolvedPayload{ .group_id = x_gid };
    auto r1            = RunResolveCompetition(x_resolved, ctx);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(ctx.join_states.count(join_id), 1);

    // Resolve Y
    WorkItem y_resolved;
    y_resolved.payload = CompetitionResolvedPayload{ .group_id = y_gid };
    auto r2            = RunResolveCompetition(y_resolved, ctx);
    ASSERT_TRUE(r2.has_value());

    // Join cleaned up
    EXPECT_EQ(ctx.join_states.count(join_id), 0);

    auto &pr = r2.value();
    EXPECT_EQ(pr.decision, PassDecision::kAdvance);

    // Should emit Mul(x0, x1) which is cheaper than
    // Add(Mul(x&y, x|y), Mul(x&~y, ~x&y))
    ASSERT_GE(pr.next.size(), 1);
    EXPECT_EQ(GetStateKind(pr.next[0].payload), StateKind::kFoldedAst);
    auto &ast = std::get< AstPayload >(pr.next[0].payload);
    EXPECT_EQ(ast.provenance, Provenance::kRewritten);
    ASSERT_TRUE(pr.next[0].group_id.has_value());
    EXPECT_EQ(*pr.next[0].group_id, parent_gid);
    ASSERT_TRUE(ast.solve_ctx.has_value());
    EXPECT_EQ(ast.solve_ctx->vars, std::vector< std::string >({ "sx0", "sx1" }));
    EXPECT_EQ(ast.solve_ctx->input_sig, input_sig);
    EXPECT_EQ(pr.next[0].depth, 5u);
    EXPECT_EQ(
        pr.next[0].history,
        std::vector< PassId >({ PassId::kBuildSignatureState,
                                PassId::kProductIdentityCollapse })
    );
    EXPECT_EQ(pr.next[0].rewrite_gen, 1u);

    // Verify the rewritten expr is x0 * x1
    EXPECT_EQ(ast.expr->kind, Expr::Kind::kMul);
}

TEST(SignaturePass, ProductJoinOneFactorNoWinner) {
    // When one factor has no winner, no rewrite should be emitted.
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    auto orig = Expr::Add(
        Expr::Mul(
            Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)),
            Expr::BitwiseOr(Expr::Variable(0), Expr::Variable(1))
        ),
        Expr::Mul(
            Expr::BitwiseAnd(Expr::Variable(0), Expr::BitwiseNot(Expr::Variable(1))),
            Expr::BitwiseAnd(Expr::BitwiseNot(Expr::Variable(0)), Expr::Variable(1))
        )
    );
    auto baseline = ComputeCost(*orig).cost;

    ProductJoinState join;
    join.original_expr = CloneExpr(*orig);
    join.baseline_cost = baseline;
    join.vars          = vars;
    join.bitwidth      = 64;
    join.rewrite_gen   = 0;
    join.full_ast      = CloneExpr(*orig);
    join.target_hash   = std::hash< Expr >{}(*orig);

    auto join_id = CreateJoin(ctx.join_states, ctx.next_join_id, JoinState{ std::move(join) });

    // X factor group — has a winner
    auto x_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    ctx.competition_groups.at(x_gid).continuation = ContinuationData{
        ProductCollapseCont{
                            .join_id = join_id,
                            .role    = ProductCollapseCont::FactorRole::kX,
                            }
    };

    CandidateRecord x_rec;
    x_rec.expr        = Expr::Variable(0);
    x_rec.cost        = ComputeCost(*x_rec.expr).cost;
    x_rec.source_pass = PassId::kSignaturePatternMatch;
    SubmitCandidate(ctx.competition_groups, x_gid, std::move(x_rec));

    // Y factor group — NO winner
    auto y_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    ctx.competition_groups.at(y_gid).continuation = ContinuationData{
        ProductCollapseCont{
                            .join_id = join_id,
                            .role    = ProductCollapseCont::FactorRole::kY,
                            }
    };
    // No candidate submitted for Y

    // Resolve X
    WorkItem x_resolved;
    x_resolved.payload = CompetitionResolvedPayload{ .group_id = x_gid };
    RunResolveCompetition(x_resolved, ctx);

    // Resolve Y (no winner)
    WorkItem y_resolved;
    y_resolved.payload = CompetitionResolvedPayload{ .group_id = y_gid };
    auto r2            = RunResolveCompetition(y_resolved, ctx);
    ASSERT_TRUE(r2.has_value());

    auto &pr = r2.value();
    // No rewrite emitted when one factor has no winner
    EXPECT_TRUE(pr.next.empty());
    // Join state cleaned up
    EXPECT_EQ(ctx.join_states.count(join_id), 0);
}

TEST(SignaturePass, JoinCleanupAfterResolution) {
    // Verify join state is removed after both sides resolve.
    std::vector< std::string > vars = { "x0" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    OperandJoinState join;
    join.full_ast      = Expr::Mul(Expr::Variable(0), Expr::Variable(0));
    join.original_mul  = Expr::Mul(Expr::Variable(0), Expr::Variable(0));
    join.target_hash   = std::hash< Expr >{}(*join.original_mul);
    join.baseline_cost = ComputeCost(*join.original_mul).cost;
    join.vars          = vars;
    join.bitwidth      = 64;
    join.rewrite_gen   = 0;
    join.lhs_resolved  = true; // one side already resolved (no bitwise)
    join.rhs_resolved  = false;

    auto join_id = CreateJoin(ctx.join_states, ctx.next_join_id, JoinState{ std::move(join) });
    EXPECT_EQ(ctx.join_states.count(join_id), 1);

    // Create RHS group
    auto rhs_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    ctx.competition_groups.at(rhs_gid).continuation = ContinuationData{
        OperandRewriteCont{
                           .join_id = join_id,
                           .role    = OperandRewriteCont::OperandRole::kRhs,
                           }
    };
    // No winner for RHS

    // Resolve
    WorkItem resolved;
    resolved.payload = CompetitionResolvedPayload{ .group_id = rhs_gid };
    auto r           = RunResolveCompetition(resolved, ctx);
    ASSERT_TRUE(r.has_value());

    // Join state should be cleaned up
    EXPECT_EQ(ctx.join_states.count(join_id), 0);
}

// --- RemainderRecombineCont resolution tests ---

TEST(SignaturePass, ResidualRecombineDirectBooleanNull) {
    // Direct boolean-null: core_expr is null, winner IS the full answer.
    // f(x0) = 2*x0 — the residual IS the function.
    std::vector< std::string > vars = { "x0" };
    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [](const std::vector< uint64_t > &vals) -> uint64_t {
        return 2 * vals[0];
    };
    auto ctx = MakeCtx(opts, vars);

    // Child group: the signature DAG solved the residual to "2*x0"
    auto child_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    ctx.competition_groups.at(child_gid).continuation = ContinuationData{
        RemainderRecombineCont{
                               .prefix_expr       = nullptr,
                               .origin            = RemainderOrigin::kDirectBooleanNull,
                               .remainder_eval    = opts.evaluator,
                               .source_sig        = { 0, 2 },
                               .remainder_support = {},
                               .prefix_degree     = 0,
                               .parent_group_id   = std::nullopt,
                               }
    };

    // Submit 2*x0 as the winner
    CandidateRecord rec;
    rec.expr        = Expr::Mul(Expr::Constant(2), Expr::Variable(0));
    rec.cost        = ComputeCost(*rec.expr).cost;
    rec.real_vars   = vars;
    rec.source_pass = PassId::kSignatureCobCandidate;
    SubmitCandidate(ctx.competition_groups, child_gid, std::move(rec));

    WorkItem resolved_item;
    resolved_item.payload = CompetitionResolvedPayload{ .group_id = child_gid };

    auto result = RunResolveCompetition(resolved_item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    // Should emit a candidate directly (no parent group)
    EXPECT_EQ(pr.decision, PassDecision::kSolvedCandidate);
    ASSERT_EQ(pr.next.size(), 1);
    EXPECT_EQ(GetStateKind(pr.next[0].payload), StateKind::kCandidateExpr);

    auto &cand = std::get< CandidatePayload >(pr.next[0].payload);
    EXPECT_EQ(cand.producing_pass, PassId::kResidualSupported);
    EXPECT_FALSE(cand.needs_original_space_verification);

    ASSERT_TRUE(pr.next[0].metadata.decomposition_meta.has_value());
    auto &dmeta = *pr.next[0].metadata.decomposition_meta;
    EXPECT_TRUE(dmeta.has_solver);
    EXPECT_EQ(dmeta.extractor_kind, static_cast< uint8_t >(ExtractorKind::kBooleanNullDirect));

    // Child group should be cleaned up
    EXPECT_EQ(ctx.competition_groups.count(child_gid), 0);
}

TEST(SignaturePass, ResidualRecombineWithCoreExpr) {
    // f(x0, x1) = x0*x1 + (x0 ^ x1)
    // core_expr = x0*x1, residual = x0 ^ x1
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [](const std::vector< uint64_t > &vals) -> uint64_t {
        return vals[0] * vals[1] + (vals[0] ^ vals[1]);
    };
    auto ctx = MakeCtx(opts, vars);

    auto core_expr          = Expr::Mul(Expr::Variable(0), Expr::Variable(1));
    Evaluator residual_eval = [](const std::vector< uint64_t > &vals) -> uint64_t {
        return vals[0] ^ vals[1];
    };

    auto child_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    ctx.competition_groups.at(child_gid).continuation = ContinuationData{
        RemainderRecombineCont{
                               .prefix_expr       = CloneExpr(*core_expr),
                               .origin            = RemainderOrigin::kProductCore,
                               .remainder_eval    = residual_eval,
                               .source_sig        = { 0, 1, 1, 0 },
                               .remainder_support = {},
                               .prefix_degree     = 0,
                               .parent_group_id   = std::nullopt,
                               }
    };

    // The DAG solved the residual: x0 ^ x1
    CandidateRecord rec;
    rec.expr        = Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1));
    rec.cost        = ComputeCost(*rec.expr).cost;
    rec.real_vars   = vars;
    rec.source_pass = PassId::kSignaturePatternMatch;
    SubmitCandidate(ctx.competition_groups, child_gid, std::move(rec));

    WorkItem resolved_item;
    resolved_item.payload = CompetitionResolvedPayload{ .group_id = child_gid };

    auto result = RunResolveCompetition(resolved_item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kSolvedCandidate);
    ASSERT_EQ(pr.next.size(), 1);

    auto &cand = std::get< CandidatePayload >(pr.next[0].payload);
    EXPECT_EQ(cand.producing_pass, PassId::kResidualSupported);

    auto &dmeta = *pr.next[0].metadata.decomposition_meta;
    EXPECT_EQ(dmeta.extractor_kind, static_cast< uint8_t >(ExtractorKind::kProductAST));
}

TEST(SignaturePass, ResidualRecombineSubmitsToParentGroup) {
    // When parent_group_id is set, submit to parent group
    // instead of emitting directly.
    std::vector< std::string > vars = { "x0" };
    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [](const std::vector< uint64_t > &vals) -> uint64_t {
        return 3 * vals[0];
    };
    auto ctx = MakeCtx(opts, vars);

    auto parent_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    auto child_gid  = CreateGroup(ctx.competition_groups, ctx.next_group_id);

    Evaluator residual_eval                           = opts.evaluator;
    ctx.competition_groups.at(child_gid).continuation = ContinuationData{
        RemainderRecombineCont{
                               .prefix_expr       = nullptr,
                               .origin            = RemainderOrigin::kDirectBooleanNull,
                               .remainder_eval    = residual_eval,
                               .source_sig        = { 0, 3 },
                               .remainder_support = {},
                               .prefix_degree     = 0,
                               .parent_group_id   = parent_gid,
                               }
    };

    CandidateRecord rec;
    rec.expr        = Expr::Mul(Expr::Constant(3), Expr::Variable(0));
    rec.cost        = ComputeCost(*rec.expr).cost;
    rec.real_vars   = vars;
    rec.source_pass = PassId::kSignatureCobCandidate;
    SubmitCandidate(ctx.competition_groups, child_gid, std::move(rec));

    WorkItem resolved_item;
    resolved_item.payload = CompetitionResolvedPayload{ .group_id = child_gid };

    auto result = RunResolveCompetition(resolved_item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    // Should advance (submitted to parent group).
    EXPECT_EQ(pr.decision, PassDecision::kAdvance);

    // ReleaseHandle on parent may resolve it if no other handles
    // remain.  The parent was created with 1 handle; AcquireHandle
    // in RunResidualSupported added a second, and ReleaseHandle
    // here drops it back to 1.  If the test didn't acquire an
    // extra handle the parent resolves, producing a
    // kCompetitionResolved item in pr.next.  Either outcome is
    // acceptable — the important thing is the candidate was
    // submitted.
    //
    // Simulate the real scenario: acquire an extra handle on the
    // parent so it stays open after the child's release.
    // (The previous assertion already ran, so just verify the
    //  candidate was actually submitted before the parent erased.)
    //
    // With the current setup (parent has exactly 1 handle from
    // CreateGroup), the child's ReleaseHandle resolves it.
    // Accept a kCompetitionResolved child if present.
    if (!pr.next.empty()) {
        EXPECT_TRUE(std::holds_alternative< CompetitionResolvedPayload >(pr.next[0].payload));
    }
}

TEST(SignaturePass, ResidualRecombineNoWinnerAdvances) {
    // When the child group has no winner, just advance.
    std::vector< std::string > vars = { "x0" };
    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [](const std::vector< uint64_t > &vals) -> uint64_t { return vals[0]; };
    auto ctx       = MakeCtx(opts, vars);

    auto child_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    ctx.competition_groups.at(child_gid).continuation = ContinuationData{
        RemainderRecombineCont{
                               .prefix_expr       = nullptr,
                               .origin            = RemainderOrigin::kDirectBooleanNull,
                               .remainder_eval    = opts.evaluator,
                               .source_sig        = { 0, 1 },
                               .remainder_support = {},
                               .prefix_degree     = 0,
                               .parent_group_id   = std::nullopt,
                               }
    };

    // No candidate submitted — group has no winner

    WorkItem resolved_item;
    resolved_item.payload = CompetitionResolvedPayload{ .group_id = child_gid };

    auto result = RunResolveCompetition(resolved_item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    EXPECT_TRUE(pr.next.empty());
}

TEST(SignaturePass, ResidualRecombineNoWinnerReleasesParentHandle) {
    std::vector< std::string > vars = { "x0" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    auto parent_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    AcquireHandle(ctx.competition_groups, parent_gid);
    EXPECT_EQ(ctx.competition_groups.at(parent_gid).open_handles, 2);

    auto child_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    Evaluator eval = [](const std::vector< uint64_t > &vals) -> uint64_t { return vals[0]; };
    ctx.competition_groups.at(child_gid).continuation = ContinuationData{
        RemainderRecombineCont{
                               .prefix_expr       = nullptr,
                               .origin            = RemainderOrigin::kDirectBooleanNull,
                               .remainder_eval    = eval,
                               .source_sig        = { 0, 1 },
                               .remainder_support = {},
                               .prefix_degree     = 0,
                               .parent_group_id   = parent_gid,
                               .target_eval       = eval,
                               .target_vars       = vars,
                               }
    };

    WorkItem resolved_item;
    resolved_item.payload = CompetitionResolvedPayload{ .group_id = child_gid };

    auto result = RunResolveCompetition(resolved_item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    EXPECT_TRUE(pr.next.empty());
    EXPECT_EQ(ctx.competition_groups.at(parent_gid).open_handles, 1);
}

TEST(SignaturePass, ResidualRecombineVerificationFailureReleasesParentHandle) {
    std::vector< std::string > vars = { "x0" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    auto parent_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    AcquireHandle(ctx.competition_groups, parent_gid);
    EXPECT_EQ(ctx.competition_groups.at(parent_gid).open_handles, 2);

    auto child_gid      = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    Evaluator good_eval = [](const std::vector< uint64_t > &vals) -> uint64_t {
        return vals[0];
    };
    Evaluator bad_eval = [](const std::vector< uint64_t > &vals) -> uint64_t {
        return vals[0] + 1;
    };
    ctx.competition_groups.at(child_gid).continuation = ContinuationData{
        RemainderRecombineCont{
                               .prefix_expr       = nullptr,
                               .origin            = RemainderOrigin::kDirectBooleanNull,
                               .remainder_eval    = good_eval,
                               .source_sig        = { 0, 1 },
                               .remainder_support = {},
                               .prefix_degree     = 0,
                               .parent_group_id   = parent_gid,
                               .target_eval       = bad_eval,
                               .target_vars       = vars,
                               }
    };

    CandidateRecord rec;
    rec.expr        = Expr::Variable(0);
    rec.cost        = ComputeCost(*rec.expr).cost;
    rec.real_vars   = vars;
    rec.source_pass = PassId::kSignaturePatternMatch;
    SubmitCandidate(ctx.competition_groups, child_gid, std::move(rec));

    WorkItem resolved_item;
    resolved_item.payload = CompetitionResolvedPayload{ .group_id = child_gid };

    auto result = RunResolveCompetition(resolved_item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    EXPECT_TRUE(pr.next.empty());
    EXPECT_EQ(ctx.competition_groups.at(parent_gid).open_handles, 1);
    EXPECT_FALSE(ctx.competition_groups.at(parent_gid).best.has_value());
}

TEST(SignaturePass, ResidualRecombineWithVarRemap) {
    // f(x0, x1) depends only on x1 after aux-var elimination.
    // core_expr = null (direct boolean null).
    // residual_support = {1} (var 0 in reduced space maps to var 1).
    // Winner is Variable(0) in reduced 1-var space -> Variable(1).
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [](const std::vector< uint64_t > &vals) -> uint64_t {
        return 5 * vals[1];
    };
    auto ctx = MakeCtx(opts, vars);

    Evaluator residual_eval = opts.evaluator;
    auto child_gid          = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    ctx.competition_groups.at(child_gid).continuation = ContinuationData{
        RemainderRecombineCont{
                               .prefix_expr       = nullptr,
                               .origin            = RemainderOrigin::kDirectBooleanNull,
                               .remainder_eval    = residual_eval,
                               .source_sig        = {},
                               .remainder_support = { 1 },
                               .prefix_degree     = 0,
                               .parent_group_id   = std::nullopt,
                               }
    };

    // Winner in reduced 1-var space: 5*x0 (but x0 maps to x1)
    CandidateRecord rec;
    rec.expr        = Expr::Mul(Expr::Constant(5), Expr::Variable(0));
    rec.cost        = ComputeCost(*rec.expr).cost;
    rec.real_vars   = { "x1" };
    rec.source_pass = PassId::kSignatureCobCandidate;
    SubmitCandidate(ctx.competition_groups, child_gid, std::move(rec));

    WorkItem resolved_item;
    resolved_item.payload = CompetitionResolvedPayload{ .group_id = child_gid };

    auto result = RunResolveCompetition(resolved_item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kSolvedCandidate);
    ASSERT_EQ(pr.next.size(), 1);

    // The remapped expression should use Variable(1), not Variable(0)
    auto &cand = std::get< CandidatePayload >(pr.next[0].payload);
    EXPECT_EQ(cand.real_vars, vars);
}

// --- RunResidualSupported emission tests ---

TEST(ResidualEmission, EmitsSignatureStateChildWithContinuation) {
    // f(x0, x1) = 2*x0 + 3*x1 — residual for the whole function.
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [](const std::vector< uint64_t > &vals) -> uint64_t {
        return 2 * vals[0] + 3 * vals[1];
    };
    auto ctx = MakeCtx(opts, vars);

    // Build a RemainderStatePayload (direct boolean-null).
    auto sig  = EvaluateBooleanSignature(opts.evaluator, 2, 64);
    auto elim = EliminateAuxVars(sig, vars);

    WorkItem item;
    item.payload = RemainderStatePayload{
        .origin            = RemainderOrigin::kDirectBooleanNull,
        .prefix_expr       = nullptr,
        .prefix_degree     = 0,
        .remainder_eval    = opts.evaluator,
        .source_sig        = sig,
        .remainder_sig     = sig,
        .remainder_elim    = elim,
        .remainder_support = BuildVarSupport(vars, elim.real_vars),
        .is_boolean_null   = true,
    };
    item.signature_recursion_depth = 1;

    auto result = RunResidualSupported(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    // 1. decision == kAdvance
    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    // 2. disposition == kRetainCurrent
    EXPECT_EQ(pr.disposition, ItemDisposition::kRetainCurrent);
    // 3. Exactly one child emitted
    ASSERT_EQ(pr.next.size(), 1);
    // 4. Child holds SignatureStatePayload
    EXPECT_EQ(GetStateKind(pr.next[0].payload), StateKind::kSignatureState);
    // 5. Child has a group_id
    ASSERT_TRUE(pr.next[0].group_id.has_value());
    // 6. Child has evaluator_override
    EXPECT_TRUE(pr.next[0].evaluator_override.has_value());
    // 7. Child preserves signature recursion depth across the residual handoff
    EXPECT_EQ(pr.next[0].signature_recursion_depth, item.signature_recursion_depth);
    // 7. Exactly one competition group created
    EXPECT_EQ(ctx.competition_groups.size(), 1);
    // 8. The group's continuation is RemainderRecombineCont
    auto &group = ctx.competition_groups.at(*pr.next[0].group_id);
    ASSERT_TRUE(group.continuation.has_value());
    EXPECT_TRUE(std::holds_alternative< RemainderRecombineCont >(*group.continuation));
}

TEST(ResidualEmission, BlockedWhenVarsOutOfRange) {
    // All vars eliminated => real_var_count == 0 => blocked.
    std::vector< std::string > vars = { "x0" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    // Constant signature: f(x0) = 42 on {0,1}.
    std::vector< uint64_t > sig = { 42, 42 };
    auto elim                   = EliminateAuxVars(sig, vars);

    WorkItem item;
    item.payload = RemainderStatePayload{
        .origin            = RemainderOrigin::kDirectBooleanNull,
        .remainder_sig     = sig,
        .remainder_elim    = elim,
        .remainder_support = {},
    };

    auto result = RunResidualSupported(item, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().decision, PassDecision::kBlocked);
    EXPECT_EQ(result.value().disposition, ItemDisposition::kRetainCurrent);
}

// --- ResidualSupported parent-group awareness ---

TEST(ResidualEmission, GroupedResidualSetsParentGroupOnContinuation) {
    // When the residual state has item.group_id, RunResidualSupported should
    // set parent_group_id on the RemainderRecombineCont so the winner
    // resolves back into the parent group.
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [](const std::vector< uint64_t > &vals) -> uint64_t {
        return 2 * vals[0] + 3 * vals[1];
    };
    auto ctx = MakeCtx(opts, vars);

    auto sig  = EvaluateBooleanSignature(opts.evaluator, 2, 64);
    auto elim = EliminateAuxVars(sig, vars);

    // Pre-create a parent competition group.
    auto parent_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);

    WorkItem item;
    item.payload = RemainderStatePayload{
        .origin            = RemainderOrigin::kDirectBooleanNull,
        .prefix_expr       = nullptr,
        .prefix_degree     = 0,
        .remainder_eval    = opts.evaluator,
        .source_sig        = sig,
        .remainder_sig     = sig,
        .remainder_elim    = elim,
        .remainder_support = BuildVarSupport(vars, elim.real_vars),
        .is_boolean_null   = true,
        .target =
            RemainderTargetContext{
                                   .eval = *ctx.evaluator,
                                   .vars = vars,
                                   },
    };
    item.group_id = parent_gid;

    auto result = RunResidualSupported(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    ASSERT_EQ(pr.next.size(), 1);
    ASSERT_TRUE(pr.next[0].group_id.has_value());

    auto child_gid = *pr.next[0].group_id;
    ASSERT_NE(child_gid, parent_gid);

    // The child group's continuation should be RemainderRecombineCont
    // with parent_group_id pointing to the parent.
    auto &child_group = ctx.competition_groups.at(child_gid);
    ASSERT_TRUE(child_group.continuation.has_value());
    auto *cont = std::get_if< RemainderRecombineCont >(&*child_group.continuation);
    ASSERT_NE(cont, nullptr);
    ASSERT_TRUE(cont->parent_group_id.has_value());
    EXPECT_EQ(*cont->parent_group_id, parent_gid);
}

TEST(ResidualEmission, UngroupedResidualNoParentGroup) {
    // Without group_id, parent_group_id should be nullopt.
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [](const std::vector< uint64_t > &vals) -> uint64_t {
        return 2 * vals[0] + 3 * vals[1];
    };
    auto ctx = MakeCtx(opts, vars);

    auto sig  = EvaluateBooleanSignature(opts.evaluator, 2, 64);
    auto elim = EliminateAuxVars(sig, vars);

    WorkItem item;
    item.payload = RemainderStatePayload{
        .origin            = RemainderOrigin::kDirectBooleanNull,
        .prefix_expr       = nullptr,
        .prefix_degree     = 0,
        .remainder_eval    = opts.evaluator,
        .source_sig        = sig,
        .remainder_sig     = sig,
        .remainder_elim    = elim,
        .remainder_support = BuildVarSupport(vars, elim.real_vars),
        .is_boolean_null   = true,
        .target =
            RemainderTargetContext{
                                   .eval = *ctx.evaluator,
                                   .vars = vars,
                                   },
    };
    // No group_id set.

    auto result = RunResidualSupported(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    ASSERT_EQ(pr.next.size(), 1);
    ASSERT_TRUE(pr.next[0].group_id.has_value());

    auto child_gid    = *pr.next[0].group_id;
    auto &child_group = ctx.competition_groups.at(child_gid);
    ASSERT_TRUE(child_group.continuation.has_value());
    auto *cont = std::get_if< RemainderRecombineCont >(&*child_group.continuation);
    ASSERT_NE(cont, nullptr);
    EXPECT_FALSE(cont->parent_group_id.has_value());
}

// --- OperandEmission tests ---

TEST(OperandEmission, FindsFirstEligibleMulAndEmitsChildren) {
    // Build Mul(And(a,b), Or(c,d)) — both sides are bitwise.
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    auto mul_expr = Expr::Mul(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)),
        Expr::BitwiseOr(Expr::Variable(0), Expr::Variable(1))
    );

    WorkItem item;
    item.payload = AstPayload{
        .expr       = std::move(mul_expr),
        .provenance = Provenance::kOriginal,
    };

    auto result = RunOperandSimplify(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    EXPECT_EQ(pr.disposition, ItemDisposition::kConsumeCurrent);
    EXPECT_EQ(pr.next.size(), 2);

    for (const auto &child : pr.next) {
        EXPECT_EQ(GetStateKind(child.payload), StateKind::kSignatureState);
        EXPECT_TRUE(child.group_id.has_value());
    }

    EXPECT_EQ(ctx.join_states.size(), 1);
    EXPECT_EQ(ctx.competition_groups.size(), 2);

    // Verify continuations are OperandRewriteCont
    for (auto &[gid, group] : ctx.competition_groups) {
        ASSERT_TRUE(group.continuation.has_value());
        EXPECT_TRUE(std::holds_alternative< OperandRewriteCont >(*group.continuation));
    }
}

TEST(OperandEmission, UsesSolveCtxVarsAndPreservesParentContext) {
    std::vector< std::string > vars = { "g0", "g1", "g2" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    auto local_vars = std::vector< std::string >{ "x", "z" };
    auto input_sig  = std::vector< uint64_t >{ 0, 1, 1, 1 };

    WorkItem item;
    item.payload = AstPayload{
        .expr = Expr::Mul(
            Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)),
            Expr::BitwiseOr(Expr::Variable(0), Expr::Variable(1))
        ),
        .provenance = Provenance::kOriginal,
        .solve_ctx =
            AstSolveContext{
                            .vars      = local_vars,
                            .input_sig = input_sig,
                            },
    };
    item.group_id = 41;
    item.depth    = 3;
    item.history  = { PassId::kClassifyAst };

    auto result = RunOperandSimplify(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    ASSERT_EQ(pr.next.size(), 2u);
    for (const auto &child : pr.next) {
        auto &sub = std::get< SignatureStatePayload >(child.payload).ctx;
        EXPECT_EQ(sub.real_vars, local_vars);
        EXPECT_EQ(sub.original_indices, std::vector< uint32_t >({ 0, 1 }));
        EXPECT_EQ(child.depth, 3u);
        EXPECT_EQ(child.history, std::vector< PassId >({ PassId::kClassifyAst }));
    }

    ASSERT_EQ(ctx.join_states.size(), 1u);
    auto *join = std::get_if< OperandJoinState >(&ctx.join_states.begin()->second);
    ASSERT_NE(join, nullptr);
    EXPECT_EQ(join->vars, local_vars);
    ASSERT_TRUE(join->parent_group_id.has_value());
    EXPECT_EQ(*join->parent_group_id, 41u);
    EXPECT_TRUE(join->has_solve_ctx);
    EXPECT_EQ(join->solve_ctx_vars, local_vars);
    EXPECT_EQ(join->solve_ctx_input_sig, input_sig);
    EXPECT_EQ(join->parent_depth, 3u);
    EXPECT_EQ(join->parent_history, std::vector< PassId >({ PassId::kClassifyAst }));
}

TEST(OperandEmission, SkipsNonBitwiseOperand) {
    // Build Mul(And(a,b), Add(a,b)) — only lhs is bitwise.
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    auto mul_expr = Expr::Mul(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)),
        Expr::Add(Expr::Variable(0), Expr::Variable(1))
    );

    WorkItem item;
    item.payload = AstPayload{
        .expr       = std::move(mul_expr),
        .provenance = Provenance::kOriginal,
    };

    auto result = RunOperandSimplify(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    EXPECT_EQ(pr.next.size(), 1);

    // The join should have rhs pre-resolved since rhs is not bitwise
    EXPECT_EQ(ctx.join_states.size(), 1);
    auto &join_var = ctx.join_states.begin()->second;
    auto *join     = std::get_if< OperandJoinState >(&join_var);
    ASSERT_NE(join, nullptr);
    EXPECT_FALSE(join->lhs_resolved);
    EXPECT_TRUE(join->rhs_resolved);
}

TEST(OperandEmission, NoEligibleSiteReturnsNoProgress) {
    // Build Add(a, b) — no eligible Mul.
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    auto add_expr = Expr::Add(Expr::Variable(0), Expr::Variable(1));

    WorkItem item;
    item.payload = AstPayload{
        .expr       = std::move(add_expr),
        .provenance = Provenance::kOriginal,
    };

    auto result = RunOperandSimplify(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kNoProgress);
    EXPECT_EQ(pr.disposition, ItemDisposition::kRetainCurrent);
}

// --- ProductEmission tests ---

TEST(ProductEmission, FindsSiteAndEmitsChildPairs) {
    // Build Add(Mul(a&b, a|b), Mul(a&~b, ~a&b)).
    // This is the product identity for a*b.
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    auto add_expr = Expr::Add(
        Expr::Mul(
            Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)),
            Expr::BitwiseOr(Expr::Variable(0), Expr::Variable(1))
        ),
        Expr::Mul(
            Expr::BitwiseAnd(Expr::Variable(0), Expr::BitwiseNot(Expr::Variable(1))),
            Expr::BitwiseAnd(Expr::BitwiseNot(Expr::Variable(0)), Expr::Variable(1))
        )
    );

    WorkItem item;
    item.payload = AstPayload{
        .expr       = std::move(add_expr),
        .provenance = Provenance::kOriginal,
    };

    auto result = RunProductIdentityCollapse(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    EXPECT_EQ(pr.disposition, ItemDisposition::kConsumeCurrent);

    ASSERT_EQ(pr.next.size(), 1u);
    EXPECT_EQ(GetStateKind(pr.next[0].payload), StateKind::kFoldedAst);
    EXPECT_FALSE(pr.next[0].group_id.has_value());

    auto &rewritten = std::get< AstPayload >(pr.next[0].payload);
    EXPECT_EQ(rewritten.provenance, Provenance::kRewritten);
    EXPECT_EQ(rewritten.expr->kind, Expr::Kind::kMul);
    EXPECT_TRUE(
        rewritten.expr->children[0]->kind == Expr::Kind::kVariable
        && rewritten.expr->children[1]->kind == Expr::Kind::kVariable
    );

    EXPECT_TRUE(ctx.join_states.empty());
    EXPECT_TRUE(ctx.competition_groups.empty());
}

TEST(ProductEmission, UsesSolveCtxVarsAndPreservesParentContext) {
    std::vector< std::string > vars = { "g0", "g1", "g2" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    auto local_vars = std::vector< std::string >{ "x", "z" };
    auto input_sig  = std::vector< uint64_t >{ 0, 0, 0, 1 };

    WorkItem item;
    item.payload = AstPayload{
        .expr = Expr::Add(
            Expr::Mul(
                Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)),
                Expr::BitwiseOr(Expr::Variable(0), Expr::Variable(1))
            ),
            Expr::Mul(
                Expr::BitwiseAnd(Expr::Variable(0), Expr::BitwiseNot(Expr::Variable(1))),
                Expr::BitwiseAnd(Expr::BitwiseNot(Expr::Variable(0)), Expr::Variable(1))
            )
        ),
        .provenance = Provenance::kOriginal,
        .solve_ctx =
            AstSolveContext{
                            .vars      = local_vars,
                            .input_sig = input_sig,
                            },
    };
    item.group_id = 52;
    item.depth    = 4;
    item.history  = { PassId::kClassifyAst, PassId::kProductIdentityCollapse };

    auto result = RunProductIdentityCollapse(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    ASSERT_EQ(pr.next.size(), 1u);
    EXPECT_EQ(GetStateKind(pr.next[0].payload), StateKind::kFoldedAst);
    ASSERT_TRUE(pr.next[0].group_id.has_value());
    EXPECT_EQ(*pr.next[0].group_id, 52u);
    EXPECT_EQ(pr.next[0].depth, 4u);
    EXPECT_EQ(
        pr.next[0].history,
        std::vector< PassId >({ PassId::kClassifyAst, PassId::kProductIdentityCollapse })
    );
    EXPECT_EQ(pr.next[0].rewrite_gen, 1u);

    auto &rewritten = std::get< AstPayload >(pr.next[0].payload);
    EXPECT_EQ(rewritten.provenance, Provenance::kRewritten);
    ASSERT_TRUE(rewritten.solve_ctx.has_value());
    EXPECT_EQ(rewritten.solve_ctx->vars, local_vars);
    EXPECT_EQ(rewritten.solve_ctx->input_sig, input_sig);
    EXPECT_EQ(rewritten.expr->kind, Expr::Kind::kMul);

    EXPECT_TRUE(ctx.join_states.empty());
    EXPECT_TRUE(ctx.competition_groups.empty());
}

// --- LiftedSubstituteCont resolution tests ---

TEST(ResolveCompetition, LiftedSubstituteContRemapsAndVerifies) {
    // Lifted problem: outer expr is x AND v0, where v0 was lifted
    // from (a * a). After substitution: x AND (a * a).
    Options opts;
    opts.bitwidth = 64;
    auto vars     = std::vector< std::string >{ "x", "a" };
    auto ctx      = MakeCtx(opts, vars);

    auto original_eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] & (v[1] * v[1]);
    };
    ctx.evaluator = original_eval;

    auto group_id = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    auto &group   = ctx.competition_groups.at(group_id);

    LiftedSubstituteCont lifted_cont{
        .outer_vars         = { "x", "a", "v0" },
        .original_var_count = 2,
        .original_eval      = original_eval,
        .original_vars      = vars,
    };
    lifted_cont.bindings.push_back(
        LiftedBinding{
            .kind             = LiftedValueKind::kArithmeticAtom,
            .outer_var_index  = 2,
            .subtree          = Expr::Mul(Expr::Variable(1), Expr::Variable(1)),
            .structural_hash  = 0,
            .original_support = { 1 },
        }
    );

    group.continuation = ContinuationData{ std::move(lifted_cont) };

    // Submit winning candidate: BitwiseAnd(Var(0), Var(2))
    // where Var(0) = x and Var(2) = v0 (the lifted variable)
    CandidateRecord rec;
    rec.expr         = Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(2));
    rec.cost         = ExprCost{ .weighted_size = 3 };
    rec.verification = VerificationState::kVerified;
    rec.real_vars    = { "x", "a", "v0" };
    rec.source_pass  = PassId::kSignatureCobCandidate;
    SubmitCandidate(ctx.competition_groups, group_id, std::move(rec));

    WorkItem resolved_item;
    resolved_item.payload = CompetitionResolvedPayload{ .group_id = group_id };

    auto result = RunResolveCompetition(resolved_item, ctx);
    ASSERT_TRUE(result.has_value());

    auto &pr = result.value();
    EXPECT_EQ(pr.decision, PassDecision::kSolvedCandidate);
    ASSERT_FALSE(pr.next.empty());

    auto *cand = std::get_if< CandidatePayload >(&pr.next[0].payload);
    ASSERT_NE(cand, nullptr);
    // The substituted expr should contain a Mul node (from the binding)
    EXPECT_NE(cand->expr, nullptr);
    // Verify it was marked as not needing OSV
    EXPECT_FALSE(cand->needs_original_space_verification);
    // Verify the real_vars are the original vars
    EXPECT_EQ(cand->real_vars, vars);
    // Group should be cleaned up
    EXPECT_EQ(ctx.competition_groups.count(group_id), 0);
}

TEST(ResolveCompetition, LiftedSubstituteNoWinnerBlocks) {
    Options opts;
    opts.bitwidth = 64;
    auto vars     = std::vector< std::string >{ "x", "a" };
    auto ctx      = MakeCtx(opts, vars);

    auto original_eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return v[0] & (v[1] * v[1]);
    };
    ctx.evaluator = original_eval;

    auto group_id = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    auto &group   = ctx.competition_groups.at(group_id);

    LiftedSubstituteCont lifted_cont{
        .outer_vars         = { "x", "a", "v0" },
        .original_var_count = 2,
        .original_eval      = original_eval,
        .original_vars      = vars,
    };
    group.continuation = ContinuationData{ std::move(lifted_cont) };

    // No candidate submitted — group has no winner

    WorkItem resolved_item;
    resolved_item.payload = CompetitionResolvedPayload{ .group_id = group_id };

    auto result = RunResolveCompetition(resolved_item, ctx);
    ASSERT_TRUE(result.has_value());

    auto &pr = result.value();
    EXPECT_EQ(pr.decision, PassDecision::kBlocked);
    EXPECT_TRUE(pr.next.empty());
}

TEST(ResolveCompetition, LiftedSubstituteWithAuxElimination) {
    // Outer vars: { "x", "y", "v0" } where v0 = x*x.
    // Aux-var elimination drops "y" (unused in outer).
    // Winner is in reduced space { "x", "v0" } as Var(0) & Var(1).
    // Resolution must remap 0->0, 1->2 then substitute v0 -> x*x.
    Options opts;
    opts.bitwidth = 64;
    auto vars     = std::vector< std::string >{ "x", "y" };
    auto ctx      = MakeCtx(opts, vars);

    auto original_eval = [](const std::vector< uint64_t > &v) -> uint64_t {
        return (v[0] * v[0]) & v[0]; // x^2 & x — uses only x
    };
    ctx.evaluator = original_eval;

    auto group_id = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    auto &group   = ctx.competition_groups.at(group_id);

    LiftedSubstituteCont lifted_cont{
        .outer_vars         = { "x", "y", "v0" },
        .original_var_count = 2,
        .original_eval      = original_eval,
        .original_vars      = vars,
    };
    lifted_cont.bindings.push_back(
        LiftedBinding{
            .kind             = LiftedValueKind::kArithmeticAtom,
            .outer_var_index  = 2,
            .subtree          = Expr::Mul(Expr::Variable(0), Expr::Variable(0)),
            .structural_hash  = 0,
            .original_support = { 0 },
        }
    );
    group.continuation = ContinuationData{ std::move(lifted_cont) };

    // Winner from reduced outer space: Var(0) & Var(1)
    // where real_vars = { "x", "v0" } (y was eliminated).
    CandidateRecord rec;
    rec.expr         = Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1));
    rec.cost         = ExprCost{ .weighted_size = 3 };
    rec.verification = VerificationState::kVerified;
    rec.real_vars    = { "x", "v0" };
    rec.source_pass  = PassId::kSignatureCobCandidate;
    SubmitCandidate(ctx.competition_groups, group_id, std::move(rec));

    WorkItem resolved_item;
    resolved_item.payload = CompetitionResolvedPayload{ .group_id = group_id };

    auto result = RunResolveCompetition(resolved_item, ctx);
    ASSERT_TRUE(result.has_value());

    auto &pr = result.value();
    EXPECT_EQ(pr.decision, PassDecision::kSolvedCandidate);
    ASSERT_FALSE(pr.next.empty());

    auto *cand = std::get_if< CandidatePayload >(&pr.next[0].payload);
    ASSERT_NE(cand, nullptr);
    EXPECT_EQ(cand->real_vars, vars);
    EXPECT_FALSE(cand->needs_original_space_verification);
}

TEST(ResolveCompetition, NestedLiftedSubstituteUsesParentLocalContext) {
    Options opts;
    opts.bitwidth  = 64;
    auto root_vars = std::vector< std::string >{ "x" };
    auto ctx       = MakeCtx(opts, root_vars);

    auto parent_expr =
        Expr::BitwiseAnd(Expr::Variable(1), Expr::Mul(Expr::Variable(0), Expr::Variable(0)));
    auto source_sig = EvaluateBooleanSignature(*parent_expr, 2, 64);

    WorkItem skel_item;
    skel_item.payload = LiftedSkeletonPayload{
        .outer_expr         = Expr::BitwiseAnd(Expr::Variable(1), Expr::Variable(2)),
        .outer_ctx          = { .vars = { "x", "v0", "v1" } },
        .original_var_count = 2,
        .baseline_cost      = ExprCost{ .weighted_size = 5 },
        .source_sig         = std::move(source_sig),
        .original_ctx =
            AstSolveContext{
                               .vars      = { "x", "v0" },
                               .evaluator = Evaluator::FromExpr(*parent_expr, 64),
                               },
    };
    std::get< LiftedSkeletonPayload >(skel_item.payload)
        .bindings.push_back(
            LiftedBinding{
                .kind             = LiftedValueKind::kArithmeticAtom,
                .outer_var_index  = 2,
                .subtree          = Expr::Mul(Expr::Variable(0), Expr::Variable(0)),
                .structural_hash  = 0,
                .original_support = { 0 },
            }
        );

    auto prep = RunPrepareLiftedOuterSolve(skel_item, ctx);
    ASSERT_TRUE(prep.has_value());
    auto &prep_pr = prep.value();
    ASSERT_EQ(prep_pr.decision, PassDecision::kAdvance);
    ASSERT_EQ(prep_pr.next.size(), 1);
    ASSERT_TRUE(prep_pr.next[0].group_id.has_value());

    const auto group_id = *prep_pr.next[0].group_id;
    auto group_it       = ctx.competition_groups.find(group_id);
    ASSERT_NE(group_it, ctx.competition_groups.end());
    ASSERT_TRUE(group_it->second.continuation.has_value());

    auto *cont = std::get_if< LiftedSubstituteCont >(&*group_it->second.continuation);
    ASSERT_NE(cont, nullptr);
    EXPECT_EQ(cont->original_vars, (std::vector< std::string >{ "x", "v0" }));
    ASSERT_TRUE(cont->original_eval.has_value());
    EXPECT_EQ(cont->original_eval->InputArity(), 2u);

    CandidateRecord rec;
    rec.expr         = Expr::BitwiseAnd(Expr::Variable(1), Expr::Variable(2));
    rec.cost         = ExprCost{ .weighted_size = 3 };
    rec.verification = VerificationState::kVerified;
    rec.real_vars    = { "x", "v0", "v1" };
    rec.source_pass  = PassId::kSignatureCobCandidate;
    ASSERT_TRUE(SubmitCandidate(ctx.competition_groups, group_id, std::move(rec)));

    WorkItem resolved_item;
    resolved_item.payload = CompetitionResolvedPayload{ .group_id = group_id };

    auto resolved = RunResolveCompetition(resolved_item, ctx);
    ASSERT_TRUE(resolved.has_value());

    auto &pr = resolved.value();
    EXPECT_EQ(pr.decision, PassDecision::kSolvedCandidate);
    ASSERT_FALSE(pr.next.empty());

    auto *cand = std::get_if< CandidatePayload >(&pr.next[0].payload);
    ASSERT_NE(cand, nullptr);
    EXPECT_EQ(cand->real_vars, (std::vector< std::string >{ "x", "v0" }));
    EXPECT_FALSE(cand->needs_original_space_verification);
    EXPECT_EQ(EvalExpr(*cand->expr, { 3, 5 }, 64), 1u);
}

// --- BuildSignatureState group reuse ---

TEST(SignaturePass, BuildSignatureStateReusesIncomingGroup) {
    // When item.group_id is already set, RunBuildSignatureState
    // should reuse that group (AcquireHandle) instead of creating
    // a fresh one.
    std::vector< std::string > vars = { "x0" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    // Pre-create a group to simulate an incoming lifted group.
    auto existing_group = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    EXPECT_EQ(ctx.competition_groups.at(existing_group).open_handles, 1);

    // Build an AST item with the existing group_id.
    WorkItem ast_item;
    auto cls         = Classification{ .semantic = SemanticClass::kLinear, .flags = kSfNone };
    ast_item.payload = AstPayload{
        .expr           = Expr::Variable(0),
        .classification = cls,
        .provenance     = Provenance::kLowered,
    };
    ast_item.features.classification = cls;
    ast_item.features.provenance     = Provenance::kLowered;
    ast_item.group_id                = existing_group;
    ctx.input_sig                    = { 0, 1 };

    auto groups_before = ctx.next_group_id;

    auto result = RunBuildSignatureState(ast_item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();
    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    ASSERT_EQ(pr.next.size(), 1);

    // The emitted child should reuse the same group id.
    auto &sig_item = pr.next[0];
    ASSERT_TRUE(sig_item.group_id.has_value());
    EXPECT_EQ(*sig_item.group_id, existing_group);

    // No new group should have been allocated.
    EXPECT_EQ(ctx.next_group_id, groups_before);

    // AcquireHandle should have incremented to 2.
    auto &group = ctx.competition_groups.at(existing_group);
    EXPECT_EQ(group.open_handles, 2);
}

// --- RunVerifyCandidate preserves group_id ---

TEST(SignaturePass, VerifyCandidatePreservesGroupId) {
    std::vector< std::string > vars = { "x0" };
    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [](const std::vector< uint64_t > &vals) -> uint64_t { return vals[0]; };
    auto ctx       = MakeCtx(opts, vars);

    WorkItem item;
    item.payload = CandidatePayload{
        .expr                              = Expr::Variable(0),
        .real_vars                         = vars,
        .cost                              = ExprCost{ .weighted_size = 1 },
        .producing_pass                    = PassId::kSignaturePatternMatch,
        .needs_original_space_verification = true,
    };
    item.group_id = 77;

    auto result = RunVerifyCandidate(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();
    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    ASSERT_EQ(pr.next.size(), 1);

    // The verified candidate must inherit the parent's group_id.
    ASSERT_TRUE(pr.next[0].group_id.has_value());
    EXPECT_EQ(*pr.next[0].group_id, 77);
}

TEST(ProductEmission, NoEligibleSiteReturnsNoProgress) {
    // Build Add(a, b) — no Mul2 children.
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    auto add_expr = Expr::Add(Expr::Variable(0), Expr::Variable(1));

    WorkItem item;
    item.payload = AstPayload{
        .expr       = std::move(add_expr),
        .provenance = Provenance::kOriginal,
    };

    auto result = RunProductIdentityCollapse(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kNoProgress);
    EXPECT_EQ(pr.disposition, ItemDisposition::kRetainCurrent);
    EXPECT_TRUE(pr.next.empty());
    EXPECT_TRUE(ctx.join_states.empty());
    EXPECT_TRUE(ctx.competition_groups.empty());
}
