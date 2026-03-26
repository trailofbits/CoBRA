#include "CompetitionGroup.h"
#include "ContinuationTypes.h"
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

// --- BitwiseComposeCont resolution test ---

TEST(SignaturePass, BitwiseComposeContResolution) {
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [](const std::vector< uint64_t > &vals) -> uint64_t {
        return vals[0] & vals[1];
    };
    auto ctx = MakeCtx(opts, vars);

    // Parent group: where the composed result will land
    auto parent_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);

    // Child group: represents the sub-problem for g(rest) = x1
    auto child_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    ctx.competition_groups.at(child_gid).continuation = ContinuationData{
        BitwiseComposeCont{
                           .var_k                   = 0,
                           .gate                    = GateKind::kAnd,
                           .add_coeff               = 0,
                           .active_context_indices  = { 1 },
                           .parent_group_id         = parent_gid,
                           .parent_real_vars        = vars,
                           .parent_original_indices = { 0, 1 },
                           .parent_num_vars         = 2,
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
}

// --- HybridComposeCont resolution test ---

TEST(SignaturePass, HybridComposeContResolution) {
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth  = 64;
    opts.evaluator = [](const std::vector< uint64_t > &vals) -> uint64_t {
        return vals[0] ^ (vals[0] & vals[1]);
    };
    auto ctx = MakeCtx(opts, vars);

    auto parent_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);

    auto child_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    ctx.competition_groups.at(child_gid).continuation = ContinuationData{
        HybridComposeCont{
                          .var_k                   = 0,
                          .op                      = ExtractOp::kXor,
                          .parent_group_id         = parent_gid,
                          .parent_real_vars        = vars,
                          .parent_original_indices = { 0, 1 },
                          .parent_num_vars         = 2,
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
}

// --- OperandRewriteCont resolution tests ---

TEST(SignaturePass, OperandJoinResolveBothSides) {
    // Build: Mul(x0 ^ x1, x0 | x1) — both operands are bitwise.
    // The join should produce a rewritten AST when both sides resolve.
    std::vector< std::string > vars = { "x0", "x1" };
    Options opts;
    opts.bitwidth = 64;
    auto ctx      = MakeCtx(opts, vars);

    // Original Mul: (x0 ^ x1) * (x0 | x1)
    auto orig_mul = Expr::Mul(
        Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1)),
        Expr::BitwiseOr(Expr::Variable(0), Expr::Variable(1))
    );
    auto baseline = ComputeCost(*orig_mul).cost;

    OperandJoinState join;
    join.full_ast      = CloneExpr(*orig_mul);
    join.original_mul  = CloneExpr(*orig_mul);
    join.target_hash   = std::hash< Expr >{}(*orig_mul);
    join.baseline_cost = baseline;
    join.vars          = vars;
    join.bitwidth      = 64;
    join.rewrite_gen   = 0;
    join.lhs_resolved  = false;
    join.rhs_resolved  = false;

    auto join_id = CreateJoin(ctx.join_states, ctx.next_join_id, JoinState{ std::move(join) });

    // Create LHS child group with OperandRewriteCont
    auto lhs_gid = CreateGroup(ctx.competition_groups, ctx.next_group_id);
    ctx.competition_groups.at(lhs_gid).continuation = ContinuationData{
        OperandRewriteCont{
                           .join_id = join_id,
                           .role    = OperandRewriteCont::OperandRole::kLhs,
                           }
    };

    // Submit x0 + x1 as the LHS winner (cheaper than x0 ^ x1)
    CandidateRecord lhs_rec;
    lhs_rec.expr        = Expr::Add(Expr::Variable(0), Expr::Variable(1));
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

    // Submit x0 + x1 as the RHS winner too (for simplicity)
    CandidateRecord rhs_rec;
    rhs_rec.expr        = Expr::Add(Expr::Variable(0), Expr::Variable(1));
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

    // Should emit a rewritten AST
    EXPECT_EQ(pr.decision, PassDecision::kAdvance);
    // If a candidate was emitted (cost gate passed), check it
    if (!pr.next.empty()) {
        EXPECT_EQ(GetStateKind(pr.next[0].payload), StateKind::kFoldedAst);
        auto &ast = std::get< AstPayload >(pr.next[0].payload);
        EXPECT_EQ(ast.provenance, Provenance::kRewritten);
        EXPECT_EQ(pr.next[0].rewrite_gen, 1);
    }
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
    auto baseline = ComputeCost(*orig).cost;

    ProductJoinState join;
    join.original_expr = CloneExpr(*orig);
    join.baseline_cost = baseline;
    join.vars          = vars;
    join.bitwidth      = 64;
    join.rewrite_gen   = 0;

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
    EXPECT_EQ(pr.next[0].rewrite_gen, 1);

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
