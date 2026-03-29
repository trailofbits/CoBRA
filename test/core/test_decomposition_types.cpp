#include "Orchestrator.h"
#include "OrchestratorPasses.h"
#include "cobra/core/Expr.h"
#include <gtest/gtest.h>

using namespace cobra;

TEST(StateKind, CoreCandidateKind) {
    StateData data = CoreCandidatePayload{
        .core_expr      = Expr::Constant(42),
        .extractor_kind = ExtractorKind::kProductAST,
    };
    EXPECT_EQ(GetStateKind(data), StateKind::kCoreCandidate);
}

TEST(StateKind, ResidualStateKind) {
    StateData data = RemainderStatePayload{
        .origin = RemainderOrigin::kDirectBooleanNull,
    };
    EXPECT_EQ(GetStateKind(data), StateKind::kRemainderState);
}

TEST(RemainderOrigin, ValuesDistinct) {
    EXPECT_NE(
        static_cast< uint8_t >(RemainderOrigin::kDirectBooleanNull),
        static_cast< uint8_t >(RemainderOrigin::kProductCore)
    );
    EXPECT_NE(
        static_cast< uint8_t >(RemainderOrigin::kPolynomialCore),
        static_cast< uint8_t >(RemainderOrigin::kTemplateCore)
    );
}

TEST(Fingerprint, CoreCandidateFingerprint) {
    WorkItem a, b;
    a.payload = CoreCandidatePayload{
        .core_expr      = Expr::Constant(42),
        .extractor_kind = ExtractorKind::kProductAST,
    };
    b.payload = CoreCandidatePayload{
        .core_expr      = Expr::Constant(99),
        .extractor_kind = ExtractorKind::kProductAST,
    };
    auto fa = ComputeFingerprint(a, 64);
    auto fb = ComputeFingerprint(b, 64);
    EXPECT_NE(fa.payload_hash, fb.payload_hash);
}

TEST(Fingerprint, ResidualStateFingerprintByOrigin) {
    WorkItem a, b;
    a.payload = RemainderStatePayload{
        .origin          = RemainderOrigin::kDirectBooleanNull,
        .is_boolean_null = true,
    };
    b.payload = RemainderStatePayload{
        .origin          = RemainderOrigin::kProductCore,
        .is_boolean_null = true,
    };
    auto fa = ComputeFingerprint(a, 64);
    auto fb = ComputeFingerprint(b, 64);
    EXPECT_NE(fa.payload_hash, fb.payload_hash);
}

TEST(Fingerprint, ResidualStateFingerprintByBooleanNull) {
    WorkItem a, b;
    a.payload = RemainderStatePayload{
        .origin          = RemainderOrigin::kProductCore,
        .is_boolean_null = true,
    };
    b.payload = RemainderStatePayload{
        .origin          = RemainderOrigin::kProductCore,
        .is_boolean_null = false,
    };
    auto fa = ComputeFingerprint(a, 64);
    auto fb = ComputeFingerprint(b, 64);
    EXPECT_NE(fa.payload_hash, fb.payload_hash);
}

TEST(ItemMetadata, DecompositionCausesDefault) {
    ItemMetadata meta;
    EXPECT_TRUE(meta.decomposition_causes.empty());
}
