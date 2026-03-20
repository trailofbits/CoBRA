#include "cobra/core/WeightedPolyFit.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/MathUtils.h"
#include "cobra/core/MonomialKey.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <functional>
#include <vector>

namespace cobra {

    namespace {

        uint64_t FallingFactorial(uint64_t x, uint8_t n, uint64_t mask) {
            uint64_t result = 1;
            for (uint8_t i = 0; i < n; ++i) { result = (result * (x - i)) & mask; }
            return result;
        }

        std::vector< std::vector< uint8_t > >
        EnumerateBasis(uint32_t num_vars, uint8_t max_degree, uint8_t per_var_cap) {
            std::vector< std::vector< uint8_t > > basis;
            std::vector< uint8_t > cur(num_vars, 0);

            std::function< void(uint32_t, uint32_t) > enumerate = [&](uint32_t dim,
                                                                      uint32_t remaining) {
                if (dim == num_vars) {
                    basis.push_back(cur);
                    return;
                }
                const auto kLim = std::min(static_cast< uint32_t >(per_var_cap), remaining);
                for (uint32_t e = 0; e <= kLim; ++e) {
                    cur[dim] = static_cast< uint8_t >(e);
                    enumerate(dim + 1, remaining - e);
                }
            };
            enumerate(0, static_cast< uint32_t >(max_degree));

            std::sort(
                basis.begin(), basis.end(),
                [](const std::vector< uint8_t > &a, const std::vector< uint8_t > &b) {
                    uint32_t da = 0;
                    uint32_t db = 0;
                    for (auto e : a) { da += e; }
                    for (auto e : b) { db += e; }
                    if (da != db) { return da < db; }
                    return a < b;
                }
            );

            return basis;
        }

        // 2-adic forward elimination + consistency check + back-substitution.
        // Returns solved coefficients, or nullopt if rank-deficient or
        // inconsistent.
        std::optional< std::vector< uint64_t > > Solve2Adic(
            std::vector< std::vector< uint64_t > > &mat, std::vector< uint64_t > &rhs,
            size_t num_cols, uint64_t mask, uint32_t bitwidth
        ) {
            const auto kNumRows = mat.size();
            std::vector< bool > is_pivot(kNumRows, false);
            std::vector< size_t > pivot_row(num_cols, 0);
            std::vector< bool > has_pivot(num_cols, false);

            for (size_t col = 0; col < num_cols; ++col) {
                size_t best_row  = kNumRows;
                uint32_t best_v2 = bitwidth + 1;
                for (size_t j = 0; j < kNumRows; ++j) {
                    if (is_pivot[j] || mat[j][col] == 0) { continue; }
                    auto v2 = static_cast< uint32_t >(std::countr_zero(mat[j][col]));
                    if (v2 < best_v2) {
                        best_v2  = v2;
                        best_row = j;
                    }
                }
                if (best_row == kNumRows) { continue; }

                is_pivot[best_row] = true;
                has_pivot[col]     = true;
                pivot_row[col]     = best_row;

                if (best_v2 >= bitwidth) { continue; }
                const uint32_t kPrec     = bitwidth - best_v2;
                const uint64_t kPrecMask = Bitmask(kPrec);
                const uint64_t kPivInv   = ModInverseOdd(mat[best_row][col] >> best_v2, kPrec);

                for (size_t i = 0; i < kNumRows; ++i) {
                    if (is_pivot[i] || mat[i][col] == 0) { continue; }
                    const uint64_t kMult = ((mat[i][col] >> best_v2) * kPivInv) & kPrecMask;
                    for (size_t c = 0; c < num_cols; ++c) {
                        mat[i][c] = (mat[i][c] - kMult * mat[best_row][c]) & mask;
                    }
                    rhs[i] = (rhs[i] - kMult * rhs[best_row]) & mask;
                }
            }

            for (size_t col = 0; col < num_cols; ++col) {
                if (!has_pivot[col]) { return std::nullopt; }
            }
            for (size_t i = 0; i < kNumRows; ++i) {
                if (!is_pivot[i] && rhs[i] != 0) { return std::nullopt; }
            }

            // Back-substitution
            std::vector< uint64_t > h_raw(num_cols, 0);
            for (size_t col_idx = num_cols; col_idx-- > 0;) {
                const size_t kRow = pivot_row[col_idx];
                uint64_t adj_rhs  = rhs[kRow];
                for (size_t c = col_idx + 1; c < num_cols; ++c) {
                    adj_rhs = (adj_rhs - h_raw[c] * mat[kRow][c]) & mask;
                }
                if (mat[kRow][col_idx] == 0) { return std::nullopt; }
                const auto kT = static_cast< uint32_t >(std::countr_zero(mat[kRow][col_idx]));
                if (kT >= bitwidth) {
                    h_raw[col_idx] = 0;
                    continue;
                }
                if (kT > 0 && (adj_rhs & ((1ULL << kT) - 1)) != 0) { return std::nullopt; }
                const uint32_t kPrec     = bitwidth - kT;
                const uint64_t kPrecMask = Bitmask(kPrec);
                const uint64_t kPivInv   = ModInverseOdd(mat[kRow][col_idx] >> kT, kPrec);
                h_raw[col_idx]           = ((adj_rhs >> kT) * kPivInv) & kPrecMask;
            }

            return h_raw;
        }

        std::optional< WeightedFitResult > TrySolve(
            const Evaluator &target, const WeightFn &weight,
            const std::vector< uint32_t > &support_vars, uint32_t total_num_vars,
            uint32_t bitwidth, uint8_t max_degree, uint8_t grid_deg
        ) {
            const auto kK        = static_cast< uint32_t >(support_vars.size());
            const uint64_t kMask = Bitmask(bitwidth);
            const auto kPerVarCap =
                static_cast< uint8_t >(std::min< uint32_t >(max_degree, grid_deg));

            auto basis_exps     = EnumerateBasis(kK, max_degree, kPerVarCap);
            const auto kNumCols = basis_exps.size();

            const auto kGridBase = static_cast< size_t >(grid_deg) + 1;
            size_t num_rows      = 1;
            for (uint32_t i = 0; i < kK; ++i) { num_rows *= kGridBase; }

            std::vector< std::vector< uint64_t > > mat(
                num_rows, std::vector< uint64_t >(kNumCols, 0)
            );
            std::vector< uint64_t > rhs(num_rows, 0);
            std::vector< uint64_t > full_point(total_num_vars, 0);
            std::vector< uint64_t > local_point(kK, 0);

            for (size_t row = 0; row < num_rows; ++row) {
                size_t tmp = row;
                for (uint32_t i = 0; i < kK; ++i) {
                    local_point[i]               = tmp % kGridBase;
                    full_point[support_vars[i]]  = tmp % kGridBase;
                    tmp                         /= kGridBase;
                }
                rhs[row]             = target(full_point) & kMask;
                const uint64_t kWVal = weight(local_point, bitwidth);
                for (size_t col = 0; col < kNumCols; ++col) {
                    uint64_t phi = 1;
                    for (uint32_t i = 0; i < kK; ++i) {
                        phi =
                            (phi * FallingFactorial(local_point[i], basis_exps[col][i], kMask))
                            & kMask;
                    }
                    mat[row][col] = (kWVal * phi) & kMask;
                }
                for (uint32_t i = 0; i < kK; ++i) { full_point[support_vars[i]] = 0; }
            }

            auto h_raw = Solve2Adic(mat, rhs, kNumCols, kMask, bitwidth);
            if (!h_raw.has_value()) { return std::nullopt; }

            const auto kNv = static_cast< uint8_t >(total_num_vars);
            NormalizedPoly poly;
            poly.num_vars       = kNv;
            poly.bitwidth       = bitwidth;
            uint8_t degree_used = 0;
            std::array< uint8_t, kMaxPolyVars > exps{};

            for (size_t col = 0; col < kNumCols; ++col) {
                uint64_t h = (*h_raw)[col] & kMask;
                if (h == 0) { continue; }
                exps.fill(0);
                uint32_t total_deg = 0;
                for (uint32_t i = 0; i < kK; ++i) {
                    exps[support_vars[i]]  = basis_exps[col][i];
                    total_deg             += basis_exps[col][i];
                }
                uint32_t q = 0;
                for (uint32_t i = 0; i < kK; ++i) { q += TwosInFactorial(basis_exps[col][i]); }
                if (q >= bitwidth) { continue; }
                if (q > 0) { h &= Bitmask(bitwidth - q); }
                if (h == 0) { continue; }

                auto key         = MonomialKey::FromExponents(exps.data(), kNv);
                poly.coeffs[key] = h;
                if (total_deg > degree_used) {
                    degree_used = static_cast< uint8_t >(total_deg);
                }
            }

            return WeightedFitResult{ std::move(poly), degree_used };
        }

    } // namespace

    std::optional< WeightedFitResult > RecoverWeightedPoly(
        const Evaluator &target, const WeightFn &weight,
        const std::vector< uint32_t > &support_vars, uint32_t total_num_vars, uint32_t bitwidth,
        uint8_t max_degree, uint8_t grid_degree
    ) {
        if (support_vars.empty()) { return std::nullopt; }
        if (total_num_vars > kMaxPolyVars) { return std::nullopt; }
        if (bitwidth < 2 || bitwidth > 64) { return std::nullopt; }
        for (auto idx : support_vars) {
            if (idx >= total_num_vars) { return std::nullopt; }
        }

        return TrySolve(
            target, weight, support_vars, total_num_vars, bitwidth, max_degree, grid_degree
        );
    }

} // namespace cobra
