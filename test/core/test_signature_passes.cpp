#include "CompetitionGroup.h"
#include "Orchestrator.h"
#include "OrchestratorPasses.h"
#include "SignaturePasses.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classification.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
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
    EXPECT_EQ(*p0, PassId::kSupportedSolve);

    item.attempted_mask |= (1ULL << static_cast< uint8_t >(PassId::kSupportedSolve));
    auto p1              = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(p1.has_value());
    EXPECT_EQ(*p1, PassId::kSignaturePatternMatch);

    item.attempted_mask |= (1ULL << static_cast< uint8_t >(PassId::kSignaturePatternMatch));
    auto p2              = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(p2.has_value());
    EXPECT_EQ(*p2, PassId::kSignatureAnf);

    item.attempted_mask |= (1ULL << static_cast< uint8_t >(PassId::kSignatureAnf));
    auto p3              = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(p3.has_value());
    EXPECT_EQ(*p3, PassId::kPrepareCoeffModel);

    item.attempted_mask |= (1ULL << static_cast< uint8_t >(PassId::kPrepareCoeffModel));
    auto p4              = SelectNextPass(item, policy, 0, cache);
    ASSERT_TRUE(p4.has_value());
    EXPECT_EQ(*p4, PassId::kSignatureMultivarPolyRecovery);

    item.attempted_mask |=
        (1ULL << static_cast< uint8_t >(PassId::kSignatureMultivarPolyRecovery));
    auto p5 = SelectNextPass(item, policy, 0, cache);
    EXPECT_FALSE(p5.has_value());
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

    auto item   = MakeSignatureItem(sig, vars, ctx);
    auto gid    = *item.group_id;
    auto result = RunPrepareCoeffModel(item, ctx);
    ASSERT_TRUE(result.has_value());
    auto &pr = result.value();

    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    EXPECT_EQ(pr.disposition, ItemDisposition::kRetainCurrent);
    ASSERT_EQ(pr.next.size(), 1);

    auto &child = pr.next[0];
    EXPECT_EQ(GetStateKind(child.payload), StateKind::kSignatureCoeffState);
    ASSERT_TRUE(child.group_id.has_value());
    EXPECT_EQ(*child.group_id, gid);

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
        .route    = Route::kPowerRecovery,
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
    auto cls         = Classification{ .semantic = SemanticClass::kLinear,
                                       .flags    = kSfNone,
                                       .route    = Route::kBitwiseOnly };
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
}
