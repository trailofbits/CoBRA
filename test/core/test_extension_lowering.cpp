#include "cobra/core/ExtensionLowering.h"

#include <cstdint>
#include <gtest/gtest.h>

using namespace cobra;

// ---------- EvalZeroExtend ----------

TEST(EvalZeroExtendTest, OneBitWidth) {
    // 1-bit zext: only bit 0 survives.
    constexpr uint64_t kMask64 = UINT64_MAX;
    EXPECT_EQ(EvalZeroExtend(0, 1, kMask64), 0u);
    EXPECT_EQ(EvalZeroExtend(1, 1, kMask64), 1u);
    EXPECT_EQ(EvalZeroExtend(0xFF, 1, kMask64), 1u);
}

TEST(EvalZeroExtendTest, EightBitWidth) {
    constexpr uint64_t kMask64 = UINT64_MAX;
    EXPECT_EQ(EvalZeroExtend(0, 8, kMask64), 0u);
    EXPECT_EQ(EvalZeroExtend(0x7F, 8, kMask64), 0x7Fu);
    EXPECT_EQ(EvalZeroExtend(0x80, 8, kMask64), 0x80u);
    EXPECT_EQ(EvalZeroExtend(0xFF, 8, kMask64), 0xFFu);
    // Upper bits cleared.
    EXPECT_EQ(EvalZeroExtend(0xDEAD00FF, 8, kMask64), 0xFFu);
}

TEST(EvalZeroExtendTest, ThirtyTwoBitWidth) {
    constexpr uint64_t kMask64 = UINT64_MAX;
    EXPECT_EQ(EvalZeroExtend(0, 32, kMask64), 0u);
    EXPECT_EQ(EvalZeroExtend(0x7FFFFFFFu, 32, kMask64), 0x7FFFFFFFu);
    EXPECT_EQ(EvalZeroExtend(0x80000000u, 32, kMask64), 0x80000000u);
    EXPECT_EQ(EvalZeroExtend(0xFFFFFFFFu, 32, kMask64), 0xFFFFFFFFu);
    EXPECT_EQ(EvalZeroExtend(0xDEADBEEF12345678ULL, 32, kMask64), 0x12345678u);
}

TEST(EvalZeroExtendTest, SixtyFourBitIsIdentity) {
    constexpr uint64_t kMask64 = UINT64_MAX;
    EXPECT_EQ(EvalZeroExtend(0, 64, kMask64), 0u);
    EXPECT_EQ(EvalZeroExtend(0xDEADBEEFCAFEBABEULL, 64, kMask64), 0xDEADBEEFCAFEBABEULL);
}

TEST(EvalZeroExtendTest, ResultMaskApplied) {
    // 8-bit zext into a 32-bit result mask.
    constexpr uint64_t kMask32 = 0xFFFFFFFFu;
    EXPECT_EQ(EvalZeroExtend(0xFF, 8, kMask32), 0xFFu);
    // 32-bit zext into a 32-bit result mask.
    EXPECT_EQ(EvalZeroExtend(0xFFFFFFFFFFFFFFFFULL, 32, kMask32), 0xFFFFFFFFu);
}

// ---------- EvalSignExtend ----------

TEST(EvalSignExtendTest, OneBitWidth) {
    constexpr uint64_t kMask64 = UINT64_MAX;
    // sext i1 0 -> 0
    EXPECT_EQ(EvalSignExtend(0, 1, kMask64), 0u);
    // sext i1 1 -> all ones (sign bit set in 1-bit value)
    EXPECT_EQ(EvalSignExtend(1, 1, kMask64), UINT64_MAX);
}

TEST(EvalSignExtendTest, EightBitWidth) {
    constexpr uint64_t kMask64 = UINT64_MAX;
    EXPECT_EQ(EvalSignExtend(0, 8, kMask64), 0u);
    EXPECT_EQ(EvalSignExtend(0x7F, 8, kMask64), 0x7Fu);
    // 0x80 has sign bit set -> extends to all-F upper bits.
    EXPECT_EQ(EvalSignExtend(0x80, 8, kMask64), 0xFFFFFFFFFFFFFF80ULL);
    EXPECT_EQ(EvalSignExtend(0xFF, 8, kMask64), 0xFFFFFFFFFFFFFFFFULL);
}

TEST(EvalSignExtendTest, ThirtyTwoBitWidth) {
    constexpr uint64_t kMask64 = UINT64_MAX;
    EXPECT_EQ(EvalSignExtend(0, 32, kMask64), 0u);
    EXPECT_EQ(EvalSignExtend(0x7FFFFFFFu, 32, kMask64), 0x7FFFFFFFu);
    EXPECT_EQ(EvalSignExtend(0x80000000u, 32, kMask64), 0xFFFFFFFF80000000ULL);
    EXPECT_EQ(EvalSignExtend(0xFFFFFFFFu, 32, kMask64), 0xFFFFFFFFFFFFFFFFULL);
}

TEST(EvalSignExtendTest, SixtyFourBitIsIdentity) {
    constexpr uint64_t kMask64 = UINT64_MAX;
    EXPECT_EQ(EvalSignExtend(0, 64, kMask64), 0u);
    EXPECT_EQ(EvalSignExtend(0xDEADBEEFCAFEBABEULL, 64, kMask64), 0xDEADBEEFCAFEBABEULL);
}

TEST(EvalSignExtendTest, ResultMaskApplied) {
    // sext 8-bit 0x80 into 32-bit result mask: upper 32 bits cleared.
    constexpr uint64_t kMask32 = 0xFFFFFFFFu;
    EXPECT_EQ(EvalSignExtend(0x80, 8, kMask32), 0xFFFFFF80u);
    // sext 8-bit 0xFF into 32-bit result mask.
    EXPECT_EQ(EvalSignExtend(0xFF, 8, kMask32), 0xFFFFFFFFu);
}

#if defined(GTEST_HAS_DEATH_TEST) && !defined(NDEBUG)
TEST(EvalZeroExtendDeathTest, ZeroBitsAsserts) {
    EXPECT_DEATH(EvalZeroExtend(0, 0, UINT64_MAX), "");
}

TEST(EvalZeroExtendDeathTest, OverSixtyFourBitsAsserts) {
    EXPECT_DEATH(EvalZeroExtend(0, 65, UINT64_MAX), "");
}

TEST(EvalSignExtendDeathTest, ZeroBitsAsserts) {
    EXPECT_DEATH(EvalSignExtend(0, 0, UINT64_MAX), "");
}

TEST(EvalSignExtendDeathTest, OverSixtyFourBitsAsserts) {
    EXPECT_DEATH(EvalSignExtend(0, 65, UINT64_MAX), "");
}
#endif

#include "cobra/core/BitWidth.h"
#include "cobra/core/CompiledExpr.h"

// ---------- Helper: evaluate a lowered Expr at a value ----------

namespace {

    uint64_t EvalExprAt(const Expr &expr, uint64_t input, uint32_t bitwidth) {
        auto compiled                = CompileExpr(expr, bitwidth);
        std::vector< uint64_t > vals = { input };
        std::vector< uint64_t > stack(compiled.stack_size);
        return EvalCompiledExpr(compiled, vals, stack);
    }

} // anonymous namespace

// ---------- LowerZeroExtend ----------

TEST(LowerZeroExtendTest, SixtyFourBitIsIdentity) {
    auto inner  = Expr::Variable(0);
    auto *raw   = inner.get();
    auto result = LowerZeroExtend(std::move(inner), 64);
    // Must return the same pointer (no wrapping).
    EXPECT_EQ(result.get(), raw);
}

TEST(LowerZeroExtendTest, EightBitStructure) {
    auto result = LowerZeroExtend(Expr::Variable(0), 8);
    // Should be And(Variable(0), Constant(0xFF)).
    ASSERT_EQ(result->kind, Expr::Kind::kAnd);
    ASSERT_EQ(result->children.size(), 2u);
    EXPECT_EQ(result->children[0]->kind, Expr::Kind::kVariable);
    EXPECT_EQ(result->children[1]->kind, Expr::Kind::kConstant);
    EXPECT_EQ(result->children[1]->constant_val, 0xFFu);
}

TEST(LowerZeroExtendTest, SemanticOneBit) {
    auto lowered = LowerZeroExtend(Expr::Variable(0), 1);
    // zext i1 at 64-bit width.
    EXPECT_EQ(EvalExprAt(*lowered, 0, 64), EvalZeroExtend(0, 1, UINT64_MAX));
    EXPECT_EQ(EvalExprAt(*lowered, 1, 64), EvalZeroExtend(1, 1, UINT64_MAX));
    EXPECT_EQ(EvalExprAt(*lowered, 0xFF, 64), EvalZeroExtend(0xFF, 1, UINT64_MAX));
}

TEST(LowerZeroExtendTest, SemanticEightBit) {
    auto lowered = LowerZeroExtend(Expr::Variable(0), 8);
    for (uint64_t v : { 0ULL, 0x7FULL, 0x80ULL, 0xFFULL, 0xDEAD00FFULL }) {
        EXPECT_EQ(EvalExprAt(*lowered, v, 64), EvalZeroExtend(v, 8, UINT64_MAX));
    }
}

TEST(LowerZeroExtendTest, SemanticThirtyTwoBit) {
    auto lowered = LowerZeroExtend(Expr::Variable(0), 32);
    for (uint64_t v :
         { 0ULL, 0x7FFFFFFFULL, 0x80000000ULL, 0xFFFFFFFFULL, 0xDEADBEEF12345678ULL })
    {
        EXPECT_EQ(EvalExprAt(*lowered, v, 64), EvalZeroExtend(v, 32, UINT64_MAX));
    }
}

// ---------- LowerSignExtend ----------

TEST(LowerSignExtendTest, SixtyFourBitIsIdentity) {
    auto inner  = Expr::Variable(0);
    auto *raw   = inner.get();
    auto result = LowerSignExtend(std::move(inner), 64);
    EXPECT_EQ(result.get(), raw);
}

TEST(LowerSignExtendTest, EightBitStructure) {
    auto result = LowerSignExtend(Expr::Variable(0), 8);
    // Should be Add(Xor(And(Variable(0), 0xFF), 0x80), Neg(0x80)).
    ASSERT_EQ(result->kind, Expr::Kind::kAdd);
    ASSERT_EQ(result->children.size(), 2u);
    // Left child: Xor(And(...), sign_bit)
    EXPECT_EQ(result->children[0]->kind, Expr::Kind::kXor);
    // Right child: Neg(sign_bit)
    EXPECT_EQ(result->children[1]->kind, Expr::Kind::kNeg);
}

TEST(LowerSignExtendTest, SemanticOneBit) {
    auto lowered = LowerSignExtend(Expr::Variable(0), 1);
    EXPECT_EQ(EvalExprAt(*lowered, 0, 64), EvalSignExtend(0, 1, UINT64_MAX));
    // sext i1 1 -> all ones.
    EXPECT_EQ(EvalExprAt(*lowered, 1, 64), EvalSignExtend(1, 1, UINT64_MAX));
    EXPECT_EQ(EvalExprAt(*lowered, 1, 64), UINT64_MAX);
}

TEST(LowerSignExtendTest, SemanticEightBit) {
    auto lowered = LowerSignExtend(Expr::Variable(0), 8);
    for (uint64_t v : { 0ULL, 0x7FULL, 0x80ULL, 0xFFULL }) {
        EXPECT_EQ(EvalExprAt(*lowered, v, 64), EvalSignExtend(v, 8, UINT64_MAX));
    }
}

TEST(LowerSignExtendTest, SemanticThirtyTwoBit) {
    auto lowered = LowerSignExtend(Expr::Variable(0), 32);
    for (uint64_t v : { 0ULL, 0x7FFFFFFFULL, 0x80000000ULL, 0xFFFFFFFFULL }) {
        EXPECT_EQ(EvalExprAt(*lowered, v, 64), EvalSignExtend(v, 32, UINT64_MAX));
    }
}

#if defined(GTEST_HAS_DEATH_TEST) && !defined(NDEBUG)
TEST(LowerZeroExtendDeathTest, ZeroBitsAsserts) {
    EXPECT_DEATH(LowerZeroExtend(Expr::Variable(0), 0), "");
}

TEST(LowerZeroExtendDeathTest, OverSixtyFourBitsAsserts) {
    EXPECT_DEATH(LowerZeroExtend(Expr::Variable(0), 65), "");
}

TEST(LowerSignExtendDeathTest, ZeroBitsAsserts) {
    EXPECT_DEATH(LowerSignExtend(Expr::Variable(0), 0), "");
}

TEST(LowerSignExtendDeathTest, OverSixtyFourBitsAsserts) {
    EXPECT_DEATH(LowerSignExtend(Expr::Variable(0), 65), "");
}
#endif
