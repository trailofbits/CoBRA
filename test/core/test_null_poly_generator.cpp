#include "cobra/core/BasisTransform.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/NullPolyGenerator.h"
#include "cobra/core/PolyExprBuilder.h"
#include "cobra/core/PolyNormalizer.h"
#include "cobra/core/SignatureChecker.h"
#include <gtest/gtest.h>
#include <random>

using namespace cobra;

namespace {

    ExponentTuple make_exp(std::initializer_list< uint8_t > exps) {
        auto data = exps.begin();
        return ExponentTuple::FromExponents(data, static_cast< uint8_t >(exps.size()));
    }

    // Add monomial null polynomial terms to a seed and verify normalization.
    void verify_null_template(const PolyIR &seed, const CoeffMap &null_monomial_terms) {
        uint64_t mask = Bitmask(seed.bitwidth);

        PolyIR perturbed;
        perturbed.num_vars = seed.num_vars;
        perturbed.bitwidth = seed.bitwidth;
        perturbed.terms    = seed.terms;
        for (const auto &[tuple, c] : null_monomial_terms) {
            perturbed.terms[tuple] = (perturbed.terms[tuple] + c) & mask;
        }
        for (auto it = perturbed.terms.begin(); it != perturbed.terms.end();) {
            if (it->second == 0) {
                it = perturbed.terms.erase(it);
            } else {
                ++it;
            }
        }

        auto norm_seed      = NormalizePolynomial(seed);
        auto norm_perturbed = NormalizePolynomial(perturbed);
        EXPECT_EQ(norm_seed, norm_perturbed);
    }

} // namespace

// --- Deterministic smoke tests ---

TEST(NullPolyGeneratorSmokeTest, UnivariateXSquaredMinusX) {
    // 2^{w-1} * (x^2 - x) is null for any w >= 2.
    // Factorial form: 2^{w-1} * x_(2).
    for (uint32_t w : { 2, 3, 4, 8, 16, 32, 64 }) {
        uint64_t mask     = Bitmask(w);
        uint64_t half     = (w < 64) ? (1ULL << (w - 1)) : (1ULL << 63);
        uint64_t neg_half = (0 - half) & mask;

        PolyIR seed{
            1, w, { { make_exp({ 1 }), 3 }, { make_exp({ 2 }), 7 } }
        };
        CoeffMap null_terms = {
            { make_exp({ 2 }),     half },
            { make_exp({ 1 }), neg_half }
        };
        verify_null_template(seed, null_terms);
    }
}

TEST(NullPolyGeneratorSmokeTest, MultivariateXSquaredYMinusXY) {
    // 2^{w-1} * (x^2*y - x*y) is null for any w >= 2.
    // Factorial form: 2^{w-1} * x_(2)*y_(1).
    for (uint32_t w : { 2, 4, 8, 32, 64 }) {
        uint64_t mask     = Bitmask(w);
        uint64_t half     = (w < 64) ? (1ULL << (w - 1)) : (1ULL << 63);
        uint64_t neg_half = (0 - half) & mask;

        PolyIR seed{ 2, w, { { make_exp({ 1, 1 }), 5 } } };
        CoeffMap null_terms = {
            { make_exp({ 2, 1 }),     half },
            { make_exp({ 1, 1 }), neg_half }
        };
        verify_null_template(seed, null_terms);
    }
}

TEST(NullPolyGeneratorSmokeTest, TwoSquaredCoords) {
    // 2^{w-2} * (x^2*y^2 - x^2*y - x*y^2 + x*y) is null for w >= 2.
    // Factorial form: 2^{w-2} * x_(2)*y_(2).
    // Test at w >= 4 for nontrivial coefficient; w=2 exercises k >= w.
    for (uint32_t w : { 4, 8, 16, 32, 64 }) {
        uint64_t mask        = Bitmask(w);
        uint64_t quarter     = 1ULL << (w - 2);
        uint64_t neg_quarter = (0 - quarter) & mask;

        PolyIR seed{
            2, w, { { make_exp({ 1, 0 }), 2 }, { make_exp({ 0, 1 }), 3 } }
        };
        CoeffMap null_terms = {
            { make_exp({ 2, 2 }),     quarter },
            { make_exp({ 2, 1 }), neg_quarter },
            { make_exp({ 1, 2 }), neg_quarter },
            { make_exp({ 1, 1 }),     quarter }
        };
        verify_null_template(seed, null_terms);
    }
}

// --- k >= w test ---

TEST(NullPolyGeneratorTest, KGeqW_UnconstrainedCoordinate) {
    // w=2, tuple (2,2): k=2 >= w=2, so any coefficient is valid.
    // 1 * x_(2)*y_(2) in factorial basis is a null polynomial mod 4.
    // Convert to monomial and verify.
    uint32_t w = 2;

    CoeffMap null_factorial = {
        { make_exp({ 2, 2 }), 1 }
    };
    CoeffMap null_monomial = ToMonomialBasis(null_factorial, 2, w);

    PolyIR seed{
        2, w, { { make_exp({ 1, 0 }), 1 }, { make_exp({ 0, 1 }), 1 } }
    };
    verify_null_template(seed, null_monomial);
}

TEST(NullPolyGeneratorTest, KGeqW_ArbitraryCoeff) {
    // w=2, tuple (2,2): try coefficient 3 (all values valid).
    uint32_t w              = 2;
    CoeffMap null_factorial = {
        { make_exp({ 2, 2 }), 3 }
    };
    CoeffMap null_monomial = ToMonomialBasis(null_factorial, 2, w);

    PolyIR seed{ 2, w, { { make_exp({ 1, 1 }), 2 } } };
    verify_null_template(seed, null_monomial);
}

// --- Generator API tests ---

TEST(NullPolyGeneratorTest, AddNullPreservesNormalization) {
    PolyIR seed{
        2,
        8,
        { { make_exp({ 1, 0 }), 3 }, { make_exp({ 0, 1 }), 5 }, { make_exp({ 2, 0 }), 1 } }
    };

    NullPolyConfig config{ 10, 2, 42 };
    auto perturbed = AddNullPolynomial(seed, config);

    auto norm_seed      = NormalizePolynomial(seed);
    auto norm_perturbed = NormalizePolynomial(perturbed);
    EXPECT_EQ(norm_seed, norm_perturbed);
}

TEST(NullPolyGeneratorTest, GenerateEquivalentVariants) {
    PolyIR seed{
        2,
        16,
        { { make_exp({ 1, 0 }), 10 }, { make_exp({ 1, 1 }), 7 }, { make_exp({ 2, 0 }), 3 } }
    };

    NullPolyConfig config{ 15, 2, 12345 };
    auto variants = GenerateEquivalentVariants(seed, 10, config);
    ASSERT_EQ(variants.size(), 10u);

    auto expected = NormalizePolynomial(seed);
    for (const auto &v : variants) { EXPECT_EQ(NormalizePolynomial(v), expected); }

    // Diversity check: at least one variant differs from seed
    bool found_different = false;
    for (const auto &v : variants) {
        if (v.terms != seed.terms) {
            found_different = true;
            break;
        }
    }
    EXPECT_TRUE(found_different) << "No variant differed from seed (degenerate null space?)";
}

TEST(NullPolyGeneratorTest, EmptySeed) {
    PolyIR seed{ 2, 8, {} };
    NullPolyConfig config{ 5, 2, 99 };
    auto perturbed = AddNullPolynomial(seed, config);

    // Empty seed + null = null, which normalizes to empty
    auto norm = NormalizePolynomial(perturbed);
    EXPECT_TRUE(norm.coeffs.empty());
}

TEST(NullPolyGeneratorTest, DeterministicReproducibility) {
    PolyIR seed{ 1, 8, { { make_exp({ 1 }), 1 } } };
    NullPolyConfig config{ 10, 2, 777 };

    auto r1 = AddNullPolynomial(seed, config);
    auto r2 = AddNullPolynomial(seed, config);
    EXPECT_EQ(r1.terms, r2.terms);
}

// --- Randomized equivalence ---

class RandomizedEquivalenceTest
    : public ::testing::TestWithParam< std::tuple< uint8_t, uint32_t > >
{};

TEST_P(RandomizedEquivalenceTest, NormalizationInvariant) {
    auto [num_vars, bitwidth] = GetParam();
    uint64_t mask             = Bitmask(bitwidth);

    // Generate a random seed PolyIR with a few terms
    std::mt19937_64 seed_rng(num_vars * 1000 + bitwidth);
    std::uniform_int_distribution< unsigned > exp_dist(0, 2);
    std::uniform_int_distribution< uint64_t > coeff_dist(
        1, (bitwidth < 64) ? ((1ULL << bitwidth) - 1) : UINT64_MAX
    );

    PolyIR seed;
    seed.num_vars           = num_vars;
    seed.bitwidth           = bitwidth;
    uint32_t num_seed_terms = 3 + (num_vars * 2);
    uint8_t exps[kMaxPolyVars];
    for (uint32_t t = 0; t < num_seed_terms; ++t) {
        for (uint8_t i = 0; i < num_vars; ++i) {
            exps[i] = static_cast< uint8_t >(exp_dist(seed_rng));
        }
        auto tuple        = ExponentTuple::FromExponents(exps, num_vars);
        Coeff c           = coeff_dist(seed_rng);
        seed.terms[tuple] = (seed.terms[tuple] + c) & mask;
    }
    // Strip zeros
    for (auto it = seed.terms.begin(); it != seed.terms.end();) {
        if (it->second == 0) {
            it = seed.terms.erase(it);
        } else {
            ++it;
        }
    }

    auto expected = NormalizePolynomial(seed);

    // Generate 10 variants with different RNG seeds
    for (uint64_t s = 0; s < 10; ++s) {
        NullPolyConfig config{ static_cast< uint32_t >(5 + num_vars * 3), 2,
                               s * 31337 + bitwidth };
        auto variants = GenerateEquivalentVariants(seed, 5, config);
        for (const auto &v : variants) {
            EXPECT_EQ(NormalizePolynomial(v), expected)
                << "Failed for num_vars=" << (int) num_vars << " bitwidth=" << bitwidth
                << " rng_seed=" << config.rng_seed;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    ParameterGrid, RandomizedEquivalenceTest,
    ::testing::Combine(
        ::testing::Values(1, 2, 3, 4),            // num_vars
        ::testing::Values(2, 3, 4, 8, 16, 32, 64) // bitwidth
    ),
    [](const auto &info) {
        return "nv" + std::to_string(std::get< 0 >(info.param)) + "_bw"
            + std::to_string(std::get< 1 >(info.param));
    }
);

// --- End-to-end pipeline: normalize → build_poly_expr → eval ---

class EndToEndEquivalenceTest
    : public ::testing::TestWithParam< std::tuple< uint8_t, uint32_t > >
{};

TEST_P(EndToEndEquivalenceTest, EvalIdentical) {
    auto [num_vars, bitwidth] = GetParam();
    uint64_t mask             = Bitmask(bitwidth);

    // Build a random seed PolyIR
    std::mt19937_64 seed_rng(num_vars * 7777 + bitwidth);
    std::uniform_int_distribution< unsigned > exp_dist(0, 2);
    std::uniform_int_distribution< uint64_t > coeff_dist(
        1, (bitwidth < 64) ? ((1ULL << bitwidth) - 1) : UINT64_MAX
    );

    PolyIR seed;
    seed.num_vars           = num_vars;
    seed.bitwidth           = bitwidth;
    uint32_t num_seed_terms = 3 + (num_vars * 2);
    uint8_t exps[kMaxPolyVars];
    for (uint32_t t = 0; t < num_seed_terms; ++t) {
        for (uint8_t i = 0; i < num_vars; ++i) {
            exps[i] = static_cast< uint8_t >(exp_dist(seed_rng));
        }
        auto tuple        = ExponentTuple::FromExponents(exps, num_vars);
        Coeff c           = coeff_dist(seed_rng);
        seed.terms[tuple] = (seed.terms[tuple] + c) & mask;
    }
    for (auto it = seed.terms.begin(); it != seed.terms.end();) {
        if (it->second == 0) {
            it = seed.terms.erase(it);
        } else {
            ++it;
        }
    }

    // Generate a variant via null-polynomial injection
    NullPolyConfig config{ static_cast< uint32_t >(5 + num_vars * 3), 2, 42424242 };
    auto variant = AddNullPolynomial(seed, config);

    // Normalize both
    auto norm_seed    = NormalizePolynomial(seed);
    auto norm_variant = NormalizePolynomial(variant);

    // Build Expr from both normalized forms
    auto seed_expr_result    = BuildPolyExpr(norm_seed);
    auto variant_expr_result = BuildPolyExpr(norm_variant);
    ASSERT_TRUE(seed_expr_result.has_value()) << "build_poly_expr failed for seed";
    ASSERT_TRUE(variant_expr_result.has_value()) << "build_poly_expr failed for variant";

    const auto &seed_expr    = seed_expr_result.value();
    const auto &variant_expr = variant_expr_result.value();

    // Evaluate both at random full-width inputs
    std::mt19937_64 eval_rng(bitwidth * 31 + num_vars);
    std::uniform_int_distribution< uint64_t > val_dist(
        0, (bitwidth < 64) ? ((1ULL << bitwidth) - 1) : UINT64_MAX
    );

    for (int sample = 0; sample < 20; ++sample) {
        std::vector< uint64_t > inputs(num_vars);
        for (uint8_t i = 0; i < num_vars; ++i) { inputs[i] = val_dist(eval_rng); }

        uint64_t seed_val    = EvalExpr(*seed_expr, inputs, bitwidth);
        uint64_t variant_val = EvalExpr(*variant_expr, inputs, bitwidth);
        EXPECT_EQ(seed_val, variant_val) << "Mismatch at sample " << sample
                                         << " for nv=" << (int) num_vars << " bw=" << bitwidth;
    }
}

INSTANTIATE_TEST_SUITE_P(
    E2EGrid, EndToEndEquivalenceTest,
    ::testing::Combine(::testing::Values(1, 2, 3, 4), ::testing::Values(2, 4, 8, 32, 64)),
    [](const auto &info) {
        return "nv" + std::to_string(std::get< 0 >(info.param)) + "_bw"
            + std::to_string(std::get< 1 >(info.param));
    }
);
