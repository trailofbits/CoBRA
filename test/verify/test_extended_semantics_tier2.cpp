// test/verify/test_extended_semantics_tier2.cpp
//
// Tier 2: Dense characterization on fw_real_vars <= 2 unsupported
// expressions.  Runs Tier 1 probes plus dense grids, difference
// tables, vanishing sets, divisibility, and per-bit output tables.

#include "ExprParser.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Simplifier.h"
#include "cobra/core/SimplifyOutcome.h"
#include "extended_semantics_probes.h"
#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace cobra;
using namespace cobra::probe;

namespace {

    size_t find_separator(const std::string &line) {
        int depth         = 0;
        size_t last_comma = std::string::npos;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '(') {
                ++depth;
            } else if (line[i] == ')') {
                --depth;
            } else if (line[i] == '\t' && depth == 0) {
                return i;
            } else if (line[i] == ',' && depth == 0) {
                last_comma = i;
            }
        }
        return last_comma;
    }

    std::string trim(const std::string &s) {
        auto start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) { return ""; }
        auto end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

} // namespace

TEST(ExtendedSemanticsTier2, DenseCharacterization) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    constexpr uint32_t kBw      = 64;
    constexpr uint32_t kGridMax = 15;

    int targets_probed    = 0;
    int dense_artifacts   = 0;
    int annotated_targets = 0;
    std::string line;
    int line_num = 0;

    while (std::getline(file, line)) {
        ++line_num;
        if (line.empty() || line[0] == '#') { continue; }

        size_t sep = find_separator(line);
        if (sep == std::string::npos) { continue; }

        std::string obfuscated = trim(line.substr(0, sep));
        std::string gt_str     = trim(line.substr(sep + 1));
        if (obfuscated.empty()) { continue; }

        auto parse = ParseAndEvaluate(obfuscated, kBw);
        if (!parse.has_value()) { continue; }
        auto ast = ParseToAst(obfuscated, kBw);
        if (!ast.has_value()) { continue; }

        auto folded_ptr = std::make_shared< std::unique_ptr< Expr > >(
            FoldConstantBitwise(std::move(ast.value().expr), kBw)
        );

        const auto &sig  = parse.value().sig;
        const auto &vars = parse.value().vars;
        auto nv          = static_cast< uint32_t >(vars.size());

        Options opts{ .bitwidth = kBw, .max_vars = 16, .spot_check = true };
        opts.evaluator = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, 64);
        };

        auto result = Simplify(sig, vars, folded_ptr->get(), opts);
        if (!result.has_value()) { continue; }
        if (result.value().kind == SimplifyOutcome::Kind::kSimplified) { continue; }
        if (result.value().kind != SimplifyOutcome::Kind::kUnchangedUnsupported) {
            continue; // skip kError
        }

        // FW elimination for target selection
        auto fw_elim = EliminateAuxVars(sig, vars, opts.evaluator, kBw);
        auto fw_nv   = static_cast< uint32_t >(fw_elim.real_vars.size());

        if (fw_nv > 2) { continue; }

        // ── Build FW-local residual ──────────────────────────
        auto fw_coeffs = InterpolateCoefficients(fw_elim.reduced_sig, fw_nv, kBw);
        auto fw_cob    = BuildCobExpr(fw_coeffs, fw_nv, kBw);
        auto cob_str   = Render(*fw_cob, fw_elim.real_vars, kBw);

        std::vector< uint32_t > fw_var_map;
        for (const auto &rv : fw_elim.real_vars) {
            for (uint32_t j = 0; j < nv; ++j) {
                if (vars[j] == rv) {
                    fw_var_map.push_back(j);
                    break;
                }
            }
        }

        auto fw_cob_shared  = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*fw_cob));
        auto orig_eval_copy = opts.evaluator;

        Evaluator residual_eval = [orig_eval_copy, fw_var_map, nv, fw_nv, fw_cob_shared](
                                      const std::vector< uint64_t > &v
                                  ) -> uint64_t {
            std::vector< uint64_t > full(nv, 0);
            for (uint32_t i = 0;
                 i < std::min(fw_nv, static_cast< uint32_t >(fw_var_map.size())); ++i)
            {
                full[fw_var_map[i]] = v[i];
            }
            uint64_t f = orig_eval_copy(full);
            uint64_t c = EvalExpr(**fw_cob_shared, v, 64);
            return f - c;
        };

        // ── Run Tier 1 probes ────────────────────────────────
        ProbeConfig cfg;
        ProbeContext ctx{ residual_eval, fw_nv, cfg };

        auto bn         = ProbeBooleanNull(ctx);
        auto slices     = ProbeUnivariateSlices(ctx);
        auto period     = ProbePeriodicity(slices);
        auto overlap    = ProbeOverlapSensitivity(ctx);
        auto diag_probe = ProbeDiagonalStructure(ctx);
        auto bit_dep    = ProbeBitDependency(ctx);

        ++targets_probed;
        bool emitted_dense_artifact   = false;
        bool emitted_dense_annotation = false;

        // ═══════════════════════════════════════════════════════
        //  Dense report
        // ═══════════════════════════════════════════════════════

        std::cerr << "\n══════════════════════════════════════════════════════\n";
        std::cerr << "L" << line_num << " fw_vars=" << fw_nv << " vars={";
        for (size_t i = 0; i < fw_elim.real_vars.size(); ++i) {
            if (i > 0) { std::cerr << ","; }
            std::cerr << fw_elim.real_vars[i];
        }
        std::cerr << "}\n";
        std::cerr << "  GT:  " << gt_str << "\n";
        std::cerr << "  CoB: " << cob_str << "\n";

        // Tier 1 summary
        bool is_bn = IsDerivedBooleanNull(bn);
        std::cerr << "  BN=" << (is_bn ? "Y" : "n") << " period=" << period.dominant_period
                  << " overlap=" << (overlap.suggests_overlap_sensitivity ? "Y" : "n")
                  << " diag_vanish=" << (diag_probe.full_all_zero ? "Y" : "n")
                  << " dep_radius=" << bit_dep.estimated_influence_radius << "\n";

        if (fw_nv == 2) {
            // ── Dense grid r(x,y) ────────────────────────────
            // Domain: [0..kGridMax], signed int64 interpretation
            emitted_dense_artifact   = true;
            emitted_dense_annotation = true;
            std::cerr << "\n  r(x,y) grid [0.." << kGridMax << "]x[0.." << kGridMax
                      << "] (signed int64, variable pair: " << fw_elim.real_vars[0] << ","
                      << fw_elim.real_vars[1] << "):\n";
            std::cerr << "       ";
            for (uint32_t y = 0; y <= kGridMax; ++y) { std::cerr << std::setw(8) << y; }
            std::cerr << "\n";
            for (uint32_t x = 0; x <= kGridMax; ++x) {
                std::cerr << "  x=" << std::setw(2) << x << " ";
                for (uint32_t y = 0; y <= kGridMax; ++y) {
                    std::vector< uint64_t > v = { x, y };
                    auto val                  = static_cast< int64_t >(residual_eval(v));
                    std::cerr << std::setw(8) << val;
                }
                std::cerr << "\n";
            }

            // ── Mixed difference Δ_x Δ_y r(x,y) ────────────
            std::cerr << "\n  Mixed diff Δ_x Δ_y r(x,y) "
                      << "[0.." << (kGridMax - 1) << "]x[0.." << (kGridMax - 1) << "]:\n";
            std::cerr << "       ";
            for (uint32_t y = 0; y < kGridMax; ++y) { std::cerr << std::setw(8) << y; }
            std::cerr << "\n";
            for (uint32_t x = 0; x < kGridMax; ++x) {
                std::cerr << "  x=" << std::setw(2) << x << " ";
                for (uint32_t y = 0; y < kGridMax; ++y) {
                    std::vector< uint64_t > v00 = { x, y };
                    std::vector< uint64_t > v10 = { x + 1, y };
                    std::vector< uint64_t > v01 = { x, y + 1 };
                    std::vector< uint64_t > v11 = { x + 1, y + 1 };
                    auto d                      = static_cast< int64_t >(residual_eval(v11))
                        - static_cast< int64_t >(residual_eval(v10))
                        - static_cast< int64_t >(residual_eval(v01))
                        + static_cast< int64_t >(residual_eval(v00));
                    std::cerr << std::setw(8) << d;
                }
                std::cerr << "\n";
            }

            // ── Vanishing set ────────────────────────────────
            std::cerr << "\n  Vanishing set (r=0) on [0.." << kGridMax << "]²:\n";
            int zero_count  = 0;
            int total_count = 0;
            for (uint32_t x = 0; x <= kGridMax; ++x) {
                for (uint32_t y = 0; y <= kGridMax; ++y) {
                    std::vector< uint64_t > v = { x, y };
                    ++total_count;
                    if (residual_eval(v) == 0) { ++zero_count; }
                }
            }
            std::cerr << "    zeros: " << zero_count << "/" << total_count << "\n";

            // ── Divisibility (signed two's-complement heuristic) ─
            std::cerr << "\n  Divisibility (signed int64, sampled "
                      << "[2..31]², heuristic not modulo-ring):\n";
            bool div_xx1 = true;
            bool div_yy1 = true;
            bool div_msa = true;
            bool div_xmy = true;
            for (uint64_t x = 2; x <= 31; ++x) {
                for (uint64_t y = 2; y <= 31; ++y) {
                    std::vector< uint64_t > v = { x, y };
                    auto val                  = static_cast< int64_t >(residual_eval(v));
                    if (val == 0) { continue; }

                    auto ix = static_cast< int64_t >(x);
                    auto iy = static_cast< int64_t >(y);

                    int64_t f_xx1 = ix * (ix - 1);
                    int64_t f_yy1 = iy * (iy - 1);
                    int64_t f_msa = ix * iy - static_cast< int64_t >(x & y);
                    int64_t f_xmy = ix - iy;

                    if (f_xx1 != 0 && (val % f_xx1) != 0) { div_xx1 = false; }
                    if (f_yy1 != 0 && (val % f_yy1) != 0) { div_yy1 = false; }
                    if (f_msa != 0 && (val % f_msa) != 0) { div_msa = false; }
                    if (f_xmy != 0 && (val % f_xmy) != 0) { div_xmy = false; }
                }
            }
            std::cerr << "    div by x(x-1): " << (div_xx1 ? "YES" : "no") << "\n";
            std::cerr << "    div by y(y-1): " << (div_yy1 ? "YES" : "no") << "\n";
            std::cerr << "    div by x*y-(x&y): " << (div_msa ? "YES" : "no") << "\n";
            std::cerr << "    div by (x-y): " << (div_xmy ? "YES" : "no") << "\n";

            // ── Per-bit output (selected bits) ───────────────
            std::cerr << "\n  Per-bit response bit_k(r(x,y)) for "
                      << "k=0..3, x,y in [0..7] "
                      << "(observed, not solved):\n";
            for (uint32_t k = 0; k < 4; ++k) {
                uint64_t bit_mask = 1ULL << k;
                std::cerr << "  bit " << k << ":\n";
                std::cerr << "       ";
                for (uint32_t y = 0; y <= 7; ++y) { std::cerr << std::setw(2) << y; }
                std::cerr << "\n";
                for (uint32_t x = 0; x <= 7; ++x) {
                    std::cerr << "  x=" << x << " ";
                    for (uint32_t y = 0; y <= 7; ++y) {
                        std::vector< uint64_t > v = { x, y };
                        uint64_t r                = residual_eval(v);
                        std::cerr << std::setw(2) << ((r & bit_mask) ? 1 : 0);
                    }
                    std::cerr << "\n";
                }
            }
        } else if (fw_nv == 1) {
            // 1-variable: emit a dense univariate table with the same
            // explicit domain/variable annotation discipline as the
            // 2-variable grid.
            emitted_dense_artifact   = true;
            emitted_dense_annotation = true;
            std::cerr << "\n  r(x) table [0..31] (signed int64, variable: "
                      << fw_elim.real_vars[0] << "):\n    ";
            for (uint64_t x = 0; x <= 31; ++x) {
                std::vector< uint64_t > v = { x };
                auto val                  = static_cast< int64_t >(residual_eval(v));
                std::cerr << val << " ";
            }
            std::cerr << "\n";
        }

        if (emitted_dense_artifact) { ++dense_artifacts; }
        if (emitted_dense_annotation) { ++annotated_targets; }

        // Probe coverage assertions
        EXPECT_TRUE(bn.tested) << "L" << line_num;
        EXPECT_GT(bn.boolean_sample_count, 0u) << "L" << line_num;
        EXPECT_GT(bn.full_width_probe_count, 0u) << "L" << line_num;
        EXPECT_TRUE(period.tested) << "L" << line_num;
        EXPECT_TRUE(bit_dep.tested) << "L" << line_num;
        EXPECT_TRUE(emitted_dense_artifact) << "L" << line_num;
        EXPECT_TRUE(emitted_dense_annotation) << "L" << line_num;
        if (fw_nv >= 2) {
            EXPECT_TRUE(overlap.tested) << "L" << line_num;
            EXPECT_TRUE(diag_probe.tested) << "L" << line_num;
        }
    }

    std::cerr << "\n══════════════════════════════════════════════════════\n";
    std::cerr << "  Total Tier 2 targets probed: " << targets_probed << "\n";
    std::cerr << "══════════════════════════════════════════════════════\n";

    EXPECT_GT(targets_probed, 0);
    EXPECT_EQ(dense_artifacts, targets_probed);
    EXPECT_EQ(annotated_targets, targets_probed);
}
