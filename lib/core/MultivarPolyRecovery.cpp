#include "cobra/core/MultivarPolyRecovery.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/MathUtils.h"
#include "cobra/core/MonomialKey.h"
#include "cobra/core/PassContract.h"
#include "cobra/core/PolyExprBuilder.h"
#include "cobra/core/PolyIR.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/Simplifier.h"
#include <array>
#include <cstdint>
#include <vector>

namespace cobra {

    namespace multivar_poly {
        enum Subcode : uint16_t {
            kEmptySupport     = 1,
            kTooManyVars      = 2,
            kBitwidthRange    = 3,
            kMaxDegreeZero    = 4,
            kBadSupportIndex  = 5,
            kDivisibilityFail = 6,
            kCapBelowMin      = 10,
            kNoVerifiedDegree = 11,
        };
    } // namespace multivar_poly

    SolverResult< NormalizedPoly > RecoverMultivarPoly(
        const Evaluator &eval, const std::vector< uint32_t > &support_vars,
        uint32_t total_num_vars, uint32_t bitwidth, uint8_t max_degree
    ) {
        if (support_vars.empty()) {
            ReasonDetail reason{
                .top = { .code    = { ReasonCategory::kGuardFailed, ReasonDomain::kMultivarPoly,
                                      multivar_poly::kEmptySupport },
                        .message = "empty support variable set" }
            };
            return SolverResult< NormalizedPoly >::Inapplicable(std::move(reason));
        }
        if (total_num_vars > kMaxPolyVars) {
            ReasonDetail reason{
                .top = { .code    = { ReasonCategory::kGuardFailed, ReasonDomain::kMultivarPoly,
                                      multivar_poly::kTooManyVars },
                        .message = "total_num_vars exceeds kMaxPolyVars" }
            };
            return SolverResult< NormalizedPoly >::Inapplicable(std::move(reason));
        }
        if (bitwidth < 2 || bitwidth > 64) {
            ReasonDetail reason{
                .top = { .code    = { ReasonCategory::kGuardFailed, ReasonDomain::kMultivarPoly,
                                      multivar_poly::kBitwidthRange },
                        .message = "bitwidth out of range [2, 64]" }
            };
            return SolverResult< NormalizedPoly >::Inapplicable(std::move(reason));
        }
        if (max_degree < 1) {
            ReasonDetail reason{
                .top = { .code    = { ReasonCategory::kGuardFailed, ReasonDomain::kMultivarPoly,
                                      multivar_poly::kMaxDegreeZero },
                        .message = "max_degree must be >= 1" }
            };
            return SolverResult< NormalizedPoly >::Inapplicable(std::move(reason));
        }
        for (auto idx : support_vars) {
            if (idx >= total_num_vars) {
                ReasonDetail reason{
                    .top = { .code    = { ReasonCategory::kGuardFailed,
                                          ReasonDomain::kMultivarPoly,
                                          multivar_poly::kBadSupportIndex },
                            .message = "support index >= total_num_vars" }
                };
                return SolverResult< NormalizedPoly >::Inapplicable(std::move(reason));
            }
        }

        const auto kK        = static_cast< uint32_t >(support_vars.size());
        const uint64_t kMask = Bitmask(bitwidth);
        const auto kBase     = static_cast< size_t >(max_degree) + 1;

        // Compute table size = kBase^kK
        size_t table_size = 1;
        for (uint32_t i = 0; i < kK; ++i) { table_size *= kBase; }

        // Evaluate on {0..max_degree}^kK grid (little-endian mixed-radix)
        std::vector< uint64_t > table(table_size);
        std::vector< uint64_t > point(total_num_vars, 0);

        for (size_t idx = 0; idx < table_size; ++idx) {
            size_t tmp = idx;
            for (uint32_t i = 0; i < kK; ++i) {
                point[support_vars[i]]  = tmp % kBase;
                tmp                    /= kBase;
            }
            table[idx] = eval(point) & kMask;
        }
        for (uint32_t i = 0; i < kK; ++i) { point[support_vars[i]] = 0; }

        // Tensor-product forward differences: max_degree passes per dimension
        for (uint32_t dim = 0; dim < kK; ++dim) {
            size_t stride = 1;
            for (uint32_t i = 0; i < dim; ++i) { stride *= kBase; }

            for (uint32_t pass = 1; pass <= max_degree; ++pass) {
                for (size_t idx = table_size; idx-- > 0;) {
                    const auto kCoord = static_cast< uint32_t >((idx / stride) % kBase);
                    if (kCoord < pass) { continue; }
                    table[idx] = (table[idx] - table[idx - stride]) & kMask;
                }
            }
        }

        // Convert forward differences to factorial-basis coefficients
        auto nv = static_cast< uint8_t >(total_num_vars);
        NormalizedPoly result;
        result.num_vars = nv;
        result.bitwidth = bitwidth;

        std::array< uint8_t, kMaxPolyVars > exps{};

        for (size_t idx = 0; idx < table_size; ++idx) {
            const uint64_t kAlpha = table[idx];
            if (kAlpha == 0) { continue; }

            // Decode mixed-radix to per-variable exponents
            exps.fill(0);
            size_t tmp = idx;
            uint32_t q = 0;
            for (uint32_t i = 0; i < kK; ++i) {
                const auto kE          = static_cast< uint8_t >(tmp % kBase);
                exps[support_vars[i]]  = kE;
                q                     += TwosInFactorial(kE);
                tmp                   /= kBase;
            }

            // Null-space term: 2^q >= bitwidth means coefficient vanishes
            if (q >= bitwidth) { continue; }

            // Divisibility gate: alpha must be divisible by 2^q
            if (q > 0) {
                const uint64_t kLowBits = kAlpha & ((1ULL << q) - 1);
                if (kLowBits != 0) {
                    ReasonDetail reason{
                        .top = { .code    = { ReasonCategory::kNoSolution,
                                              ReasonDomain::kMultivarPoly,
                                              multivar_poly::kDivisibilityFail },
                                .message = "falling-factorial coefficient fails divisibility "
                                            "gate" }
                    };
                    return SolverResult< NormalizedPoly >::Blocked(std::move(reason));
                }
            }

            const uint32_t kPrecBits = bitwidth - q;

            // Compute product of odd parts mod 2^kPrecBits
            uint64_t odd_product = 1;
            const uint64_t kPrecMask =
                (kPrecBits >= 64) ? UINT64_MAX : ((1ULL << kPrecBits) - 1);
            for (uint32_t i = 0; i < kK; ++i) {
                uint8_t e = exps[support_vars[i]];
                if (e >= 2) {
                    odd_product = (odd_product * OddPartFactorial(e, kPrecBits)) & kPrecMask;
                }
            }

            // h = (alpha >> q) * ModInverseOdd(odd_product) mod 2^kPrecBits
            uint64_t h = (kAlpha >> q) & kPrecMask;
            if (odd_product != 1) {
                h = (h * ModInverseOdd(odd_product, kPrecBits)) & kPrecMask;
            }

            if (h == 0) { continue; }

            auto key           = MonomialKey::FromExponents(exps.data(), nv);
            result.coeffs[key] = h;
        }

        return SolverResult< NormalizedPoly >::Success(std::move(result));
    }

    SolverResult< PolyRecoveryResult > RecoverAndVerifyPoly(
        const Evaluator &eval, const std::vector< uint32_t > &support_vars,
        uint32_t total_num_vars, uint32_t bitwidth, uint8_t max_degree_cap, uint8_t min_degree
    ) {
        if (max_degree_cap < min_degree) {
            ReasonDetail reason{
                .top = { .code    = { ReasonCategory::kGuardFailed, ReasonDomain::kMultivarPoly,
                                      multivar_poly::kCapBelowMin },
                        .message = "max_degree_cap < min_degree" }
            };
            return SolverResult< PolyRecoveryResult >::Inapplicable(std::move(reason));
        }

        // Each degree is recovered independently (no incremental reuse).
        for (uint8_t d = min_degree; d <= max_degree_cap; ++d) {
            auto poly = RecoverMultivarPoly(eval, support_vars, total_num_vars, bitwidth, d);
            if (!poly.Succeeded()) { continue; }

            auto expr = BuildPolyExpr(poly.TakePayload());
            if (!expr.has_value()) { continue; }

            auto check = FullWidthCheckEval(eval, total_num_vars, *expr.value(), bitwidth);
            if (!check.passed) { continue; }

            return SolverResult< PolyRecoveryResult >::Success(
                PolyRecoveryResult{ std::move(expr.value()), d }
            );
        }
        ReasonDetail reason{
            .top = { .code    = { ReasonCategory::kSearchExhausted, ReasonDomain::kMultivarPoly,
                                  multivar_poly::kNoVerifiedDegree },
                    .message = "no degree produced a verified polynomial" }
        };
        return SolverResult< PolyRecoveryResult >::Blocked(std::move(reason));
    }

} // namespace cobra
