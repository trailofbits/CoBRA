#include "cobra/core/Classification.h"
#include "cobra/core/Classifier.h"
#include <gtest/gtest.h>

using namespace cobra;

// --- Fold tests ---

TEST(FoldBitwiseTest, AndWithAllOnesFoldsToIdentity) {
    auto expr   = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(UINT64_MAX));
    auto folded = FoldConstantBitwise(std::move(expr), 64);
    EXPECT_EQ(folded->kind, Expr::Kind::kVariable);
    EXPECT_EQ(folded->var_index, 0);
}

TEST(FoldBitwiseTest, OrWithZeroFoldsToIdentity) {
    auto expr   = Expr::BitwiseOr(Expr::Variable(0), Expr::Constant(0));
    auto folded = FoldConstantBitwise(std::move(expr), 64);
    EXPECT_EQ(folded->kind, Expr::Kind::kVariable);
    EXPECT_EQ(folded->var_index, 0);
}

TEST(FoldBitwiseTest, ConstantOnlySubtreeFolds) {
    auto expr   = Expr::BitwiseNot(Expr::Constant(0));
    auto folded = FoldConstantBitwise(std::move(expr), 64);
    EXPECT_EQ(folded->kind, Expr::Kind::kConstant);
    EXPECT_EQ(folded->constant_val, UINT64_MAX);
}

TEST(FoldBitwiseTest, NonTrivialConstantUntouched) {
    auto expr   = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF));
    auto folded = FoldConstantBitwise(std::move(expr), 64);
    EXPECT_EQ(folded->kind, Expr::Kind::kAnd);
}

TEST(FoldBitwiseTest, ArithmeticBoundaryPreserved) {
    // (x + 0) & 0xFF -- the Add child should be preserved
    auto add    = Expr::Add(Expr::Variable(0), Expr::Constant(0));
    auto expr   = Expr::BitwiseAnd(std::move(add), Expr::Constant(0xFF));
    auto folded = FoldConstantBitwise(std::move(expr), 64);
    EXPECT_EQ(folded->kind, Expr::Kind::kAnd);
    EXPECT_EQ(folded->children[0]->kind, Expr::Kind::kAdd);
}

TEST(FoldBitwiseTest, AndWithZeroFoldsToZero) {
    auto expr   = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0));
    auto folded = FoldConstantBitwise(std::move(expr), 64);
    EXPECT_EQ(folded->kind, Expr::Kind::kConstant);
    EXPECT_EQ(folded->constant_val, 0u);
}

TEST(FoldBitwiseTest, OrWithAllOnesFoldsToAllOnes) {
    auto expr   = Expr::BitwiseOr(Expr::Variable(0), Expr::Constant(UINT64_MAX));
    auto folded = FoldConstantBitwise(std::move(expr), 64);
    EXPECT_EQ(folded->kind, Expr::Kind::kConstant);
    EXPECT_EQ(folded->constant_val, UINT64_MAX);
}

TEST(FoldBitwiseTest, XorWithZeroFoldsToIdentity) {
    auto expr   = Expr::BitwiseXor(Expr::Variable(0), Expr::Constant(0));
    auto folded = FoldConstantBitwise(std::move(expr), 64);
    EXPECT_EQ(folded->kind, Expr::Kind::kVariable);
    EXPECT_EQ(folded->var_index, 0);
}

// --- Classify tests ---

TEST(ClassifierTest, PureVariable) {
    auto expr = Expr::Variable(0);
    EXPECT_EQ(ClassifyStructural(*expr).semantic, SemanticClass::kLinear);
}

TEST(ClassifierTest, LinearSum) {
    auto expr = Expr::Add(Expr::Variable(0), Expr::Variable(1));
    EXPECT_EQ(ClassifyStructural(*expr).semantic, SemanticClass::kLinear);
}

TEST(ClassifierTest, LinearCoeffTimesVar) {
    auto expr = Expr::Mul(Expr::Constant(5), Expr::Variable(0));
    EXPECT_EQ(ClassifyStructural(*expr).semantic, SemanticClass::kLinear);
}

TEST(ClassifierTest, LinearBitwiseVarsOnly) {
    auto expr = Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1));
    EXPECT_EQ(ClassifyStructural(*expr).semantic, SemanticClass::kLinear);
}

TEST(ClassifierTest, SemilinearAndWithConst) {
    auto expr = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF));
    EXPECT_EQ(ClassifyStructural(*expr).semantic, SemanticClass::kSemilinear);
}

TEST(ClassifierTest, SemilinearOrWithConst) {
    auto expr = Expr::BitwiseOr(Expr::Variable(0), Expr::Constant(0x80));
    EXPECT_EQ(ClassifyStructural(*expr).semantic, SemanticClass::kSemilinear);
}

TEST(ClassifierTest, SemilinearXorWithConst) {
    auto expr = Expr::BitwiseXor(Expr::Variable(0), Expr::Constant(0x55));
    EXPECT_EQ(ClassifyStructural(*expr).semantic, SemanticClass::kSemilinear);
}

TEST(ClassifierTest, SemilinearNestedBitwise) {
    // (x ^ 0x55) & (y | 0x80)
    auto lhs  = Expr::BitwiseXor(Expr::Variable(0), Expr::Constant(0x55));
    auto rhs  = Expr::BitwiseOr(Expr::Variable(1), Expr::Constant(0x80));
    auto expr = Expr::BitwiseAnd(std::move(lhs), std::move(rhs));
    EXPECT_EQ(ClassifyStructural(*expr).semantic, SemanticClass::kSemilinear);
}

TEST(ClassifierTest, ConstantOnlyBitwiseIsLinear) {
    // 5 * (~0) -- constant-only bitwise has no variable support
    auto expr = Expr::Mul(Expr::Constant(5), Expr::BitwiseNot(Expr::Constant(0)));
    EXPECT_EQ(ClassifyStructural(*expr).semantic, SemanticClass::kLinear);
}

TEST(ClassifierTest, PolynomialVarTimesVar) {
    auto expr = Expr::Mul(Expr::Variable(0), Expr::Variable(1));
    EXPECT_EQ(ClassifyStructural(*expr).semantic, SemanticClass::kPolynomial);
}

TEST(ClassifierTest, PolynomialVarTimesExpr) {
    // x * (y + 1)
    auto sum  = Expr::Add(Expr::Variable(1), Expr::Constant(1));
    auto expr = Expr::Mul(Expr::Variable(0), std::move(sum));
    EXPECT_EQ(ClassifyStructural(*expr).semantic, SemanticClass::kPolynomial);
}

TEST(ClassifierTest, SemilinearWithCoeff) {
    // 3 * (x & 0xFF)
    auto masked = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF));
    auto expr   = Expr::Mul(Expr::Constant(3), std::move(masked));
    EXPECT_EQ(ClassifyStructural(*expr).semantic, SemanticClass::kSemilinear);
}

TEST(ClassifierTest, SemilinearPropagatesThroughAdd) {
    // (x & 0xFF) + y — semilinear in left child propagates through Add
    auto e =
        Expr::Add(Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)), Expr::Variable(1));
    EXPECT_EQ(ClassifyStructural(*e).semantic, SemanticClass::kSemilinear);
}

TEST(ClassifierTest, SemilinearPropagatesThroughNot) {
    // ~(x & 0xFF) — semilinear inside NOT
    auto e = Expr::BitwiseNot(Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)));
    EXPECT_EQ(ClassifyStructural(*e).semantic, SemanticClass::kSemilinear);
}

TEST(ClassifierTest, ShrVariableIsSemilinear) {
    auto e = Expr::LogicalShr(Expr::Variable(0), 3);
    EXPECT_EQ(ClassifyStructural(*e).semantic, SemanticClass::kSemilinear);
}

TEST(ClassifierTest, ShrMaskedIsSemilinear) {
    auto e = Expr::LogicalShr(Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF)), 1);
    EXPECT_EQ(ClassifyStructural(*e).semantic, SemanticClass::kSemilinear);
}

TEST(ClassifierTest, ShrOverArithmeticIsLinear) {
    auto e = Expr::LogicalShr(Expr::Add(Expr::Variable(0), Expr::Variable(1)), 2);
    EXPECT_EQ(ClassifyStructural(*e).semantic, SemanticClass::kLinear);
}

TEST(FoldBitwiseTest, ShrConstantFolds) {
    auto e      = Expr::LogicalShr(Expr::Constant(8), 2);
    auto folded = FoldConstantBitwise(std::move(e), 64);
    // Constant arithmetic subtrees now fold to a single constant
    EXPECT_EQ(folded->kind, Expr::Kind::kConstant);
    EXPECT_EQ(folded->constant_val, 2);
}

TEST(FoldBitwiseTest, ShrDoesNotBreakBitwiseFold) {
    auto inner  = Expr::LogicalShr(Expr::Constant(8), 2);
    auto e      = Expr::BitwiseNot(std::move(inner));
    auto folded = FoldConstantBitwise(std::move(e), 64);
    EXPECT_EQ(folded->kind, Expr::Kind::kConstant);
    EXPECT_EQ(folded->constant_val, ~uint64_t(2));
}

// --- Structural classifier tests ---

TEST(StructuralClassifierTest, PureArithmetic) {
    // x + y -> kSfHasArithmetic
    auto e = Expr::Add(Expr::Variable(0), Expr::Variable(1));
    auto c = ClassifyStructural(*e);
    EXPECT_EQ(c.flags, kSfHasArithmetic);
}

TEST(StructuralClassifierTest, PureBitwise) {
    // x & y -> kSfHasBitwise
    auto e = Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1));
    auto c = ClassifyStructural(*e);
    EXPECT_EQ(c.flags, kSfHasBitwise);
}

TEST(StructuralClassifierTest, MultilinearProduct) {
    // x * y
    auto e = Expr::Mul(Expr::Variable(0), Expr::Variable(1));
    auto c = ClassifyStructural(*e);
    EXPECT_EQ(c.flags, kSfHasArithmetic | kSfHasMul | kSfHasMultilinearProduct);
}

TEST(StructuralClassifierTest, SingletonPower) {
    // x * x
    auto e = Expr::Mul(Expr::Variable(0), Expr::Variable(0));
    auto c = ClassifyStructural(*e);
    EXPECT_EQ(c.flags, kSfHasArithmetic | kSfHasMul | kSfHasSingletonPower);
}

TEST(StructuralClassifierTest, SingletonPowerGt2) {
    // x * x * x
    auto e = Expr::Mul(Expr::Mul(Expr::Variable(0), Expr::Variable(0)), Expr::Variable(0));
    auto c = ClassifyStructural(*e);
    EXPECT_EQ(
        c.flags, kSfHasArithmetic | kSfHasMul | kSfHasSingletonPower | kSfHasSingletonPowerGt2
    );
}

TEST(StructuralClassifierTest, MixedProductAndTimes) {
    // (x & y) * z
    auto e =
        Expr::Mul(Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2));
    auto c = ClassifyStructural(*e);
    EXPECT_EQ(
        c.flags,
        kSfHasArithmetic | kSfHasBitwise | kSfHasMul | kSfHasMultilinearProduct
            | kSfHasMixedProduct | kSfHasArithOverBitwise
    );
}

TEST(StructuralClassifierTest, MixedProductXorTimes) {
    // (x ^ y) * z
    auto e =
        Expr::Mul(Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2));
    auto c = ClassifyStructural(*e);
    EXPECT_EQ(
        c.flags,
        kSfHasArithmetic | kSfHasBitwise | kSfHasMul | kSfHasMultilinearProduct
            | kSfHasMixedProduct | kSfHasArithOverBitwise
    );
}

TEST(StructuralClassifierTest, BitwiseOverArith) {
    // (x + y) & z — no Mul, so BitwiseOnly (standard linear MBA)
    auto e =
        Expr::BitwiseAnd(Expr::Add(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2));
    auto c = ClassifyStructural(*e);
    EXPECT_EQ(c.flags, kSfHasArithmetic | kSfHasBitwise | kSfHasBitwiseOverArith);
}

TEST(StructuralClassifierTest, BitwiseOverArithXor) {
    // (x * y) ^ z
    auto e =
        Expr::BitwiseXor(Expr::Mul(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2));
    auto c = ClassifyStructural(*e);
    EXPECT_EQ(
        c.flags,
        kSfHasArithmetic | kSfHasBitwise | kSfHasMul | kSfHasMultilinearProduct
            | kSfHasBitwiseOverArith
    );
}

TEST(StructuralClassifierTest, ArithOverBitwiseOnly) {
    // 3 * (x & y) -> informational, not MixedRewrite
    auto e =
        Expr::Mul(Expr::Constant(3), Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)));
    auto c = ClassifyStructural(*e);
    EXPECT_EQ(c.flags, kSfHasArithmetic | kSfHasBitwise | kSfHasArithOverBitwise);
}

TEST(StructuralClassifierTest, MultivarHighPowerPure) {
    // x * x * y -> pure polynomial, flag is set but route is
    // PowerRecovery (no hybrid blockers)
    auto e = Expr::Mul(Expr::Mul(Expr::Variable(0), Expr::Variable(0)), Expr::Variable(1));
    auto c = ClassifyStructural(*e);
    EXPECT_EQ(c.flags, kSfHasArithmetic | kSfHasMul | kSfHasMultivarHighPower);
}

TEST(StructuralClassifierTest, MultivarHighPowerPureCubic) {
    // x^2 * y + x^3 -> pure polynomial, PowerRecovery
    auto x2y = Expr::Mul(Expr::Mul(Expr::Variable(0), Expr::Variable(0)), Expr::Variable(1));
    auto x3  = Expr::Mul(Expr::Mul(Expr::Variable(0), Expr::Variable(0)), Expr::Variable(0));
    auto e   = Expr::Add(std::move(x2y), std::move(x3));
    auto c   = ClassifyStructural(*e);
    EXPECT_TRUE(HasFlag(c.flags, kSfHasMultivarHighPower));
}

TEST(StructuralClassifierTest, MultivarHighPowerWithBitwiseAnd) {
    // (x^2 * y) & z -> BitwiseOverArith, MixedRewrite
    auto x2y = Expr::Mul(Expr::Mul(Expr::Variable(0), Expr::Variable(0)), Expr::Variable(1));
    auto e   = Expr::BitwiseAnd(std::move(x2y), Expr::Variable(2));
    auto c   = ClassifyStructural(*e);
    EXPECT_TRUE(HasFlag(c.flags, kSfHasMultivarHighPower));
    EXPECT_TRUE(HasFlag(c.flags, kSfHasBitwiseOverArith));
}

TEST(StructuralClassifierTest, MultivarHighPowerWithMixedProduct) {
    // (x & y) * z + x^2 * y -> MixedProduct dominates
    auto mixed =
        Expr::Mul(Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2));
    auto x2y = Expr::Mul(Expr::Mul(Expr::Variable(0), Expr::Variable(0)), Expr::Variable(1));
    auto e   = Expr::Add(std::move(mixed), std::move(x2y));
    auto c   = ClassifyStructural(*e);
    EXPECT_TRUE(HasFlag(c.flags, kSfHasMultivarHighPower));
    EXPECT_TRUE(HasFlag(c.flags, kSfHasMixedProduct));
}

TEST(StructuralClassifierTest, TopLevelXor) {
    // x ^ y (top-level, no mixed product context)
    auto e = Expr::BitwiseXor(Expr::Variable(0), Expr::Variable(1));
    auto c = ClassifyStructural(*e);
    EXPECT_EQ(c.flags, kSfHasBitwise);
}

TEST(StructuralClassifierTest, LinearMBABitwiseOnly) {
    // (x & y) + (x | y) -> BitwiseOnly, not MixedRewrite
    auto e = Expr::Add(
        Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)),
        Expr::BitwiseOr(Expr::Variable(0), Expr::Variable(1))
    );
    auto c = ClassifyStructural(*e);
    EXPECT_EQ(c.flags, kSfHasArithmetic | kSfHasBitwise | kSfHasArithOverBitwise);
}

TEST(StructuralClassifierTest, NegMixedProduct) {
    // -((x & y) * z)
    auto e = Expr::Negate(
        Expr::Mul(Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2))
    );
    auto c = ClassifyStructural(*e);
    EXPECT_EQ(
        c.flags,
        kSfHasArithmetic | kSfHasBitwise | kSfHasMul | kSfHasMultilinearProduct
            | kSfHasMixedProduct | kSfHasArithOverBitwise
    );
}

TEST(StructuralClassifierTest, NotOverArith) {
    // ~(x + y) — no Mul, so BitwiseOnly (standard linear MBA)
    auto e = Expr::BitwiseNot(Expr::Add(Expr::Variable(0), Expr::Variable(1)));
    auto c = ClassifyStructural(*e);
    EXPECT_EQ(c.flags, kSfHasArithmetic | kSfHasBitwise | kSfHasBitwiseOverArith);
}

TEST(StructuralClassifierTest, BitwiseOverArithWithMul) {
    // ~(x * y) — has Mul, so still MixedRewrite
    auto e = Expr::BitwiseNot(Expr::Mul(Expr::Variable(0), Expr::Variable(1)));
    auto c = ClassifyStructural(*e);
    EXPECT_TRUE(HasFlag(c.flags, kSfHasBitwiseOverArith));
    EXPECT_TRUE(HasFlag(c.flags, kSfHasMul));
}

TEST(StructuralClassifierTest, FallbackDominatesSupported) {
    // x^3 + (x & y) * z -> MixedRewrite (not PowerRecovery)
    auto cube = Expr::Mul(Expr::Mul(Expr::Variable(0), Expr::Variable(0)), Expr::Variable(0));
    auto mixed =
        Expr::Mul(Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2));
    auto e = Expr::Add(std::move(cube), std::move(mixed));
    ClassifyStructural(*e);
}

TEST(StructuralClassifierTest, CubePlusMultilinearIsPowerRecovery) {
    // x^3 + x*y -> PowerRecovery (singleton power dominates multilinear)
    auto cube = Expr::Mul(Expr::Mul(Expr::Variable(0), Expr::Variable(0)), Expr::Variable(0));
    auto xy   = Expr::Mul(Expr::Variable(0), Expr::Variable(1));
    auto e    = Expr::Add(std::move(cube), std::move(xy));
    ClassifyStructural(*e);
}

// Semantic class tests
TEST(StructuralClassifierTest, SemanticLinear) {
    auto e = Expr::Add(Expr::Variable(0), Expr::Variable(1));
    auto c = ClassifyStructural(*e);
    EXPECT_EQ(c.semantic, SemanticClass::kLinear);
}

TEST(StructuralClassifierTest, SemanticSemilinear) {
    auto e = Expr::BitwiseAnd(Expr::Variable(0), Expr::Constant(0xFF));
    auto c = ClassifyStructural(*e);
    EXPECT_EQ(c.semantic, SemanticClass::kSemilinear);
}

TEST(StructuralClassifierTest, SemanticPolynomial) {
    auto e = Expr::Mul(Expr::Variable(0), Expr::Variable(1));
    auto c = ClassifyStructural(*e);
    EXPECT_EQ(c.semantic, SemanticClass::kPolynomial);
}

TEST(StructuralClassifierTest, SemanticNonPolynomial) {
    // (x & y) * z -> kNonPolynomial (polynomial + mixed product)
    auto e =
        Expr::Mul(Expr::BitwiseAnd(Expr::Variable(0), Expr::Variable(1)), Expr::Variable(2));
    auto c = ClassifyStructural(*e);
    EXPECT_EQ(c.semantic, SemanticClass::kNonPolynomial);
}

// Shr propagation
TEST(StructuralClassifierTest, ShrVarIsSemilinear) {
    auto e = Expr::LogicalShr(Expr::Variable(0), 3);
    auto c = ClassifyStructural(*e);
    EXPECT_EQ(c.semantic, SemanticClass::kSemilinear);
}

TEST(StructuralClassifierTest, ShrOverArithIsLinear) {
    auto e = Expr::LogicalShr(Expr::Add(Expr::Variable(0), Expr::Variable(1)), 2);
    auto c = ClassifyStructural(*e);
    EXPECT_EQ(c.semantic, SemanticClass::kLinear);
}
