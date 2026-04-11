/// Fastpath verify-fail delta taxonomy.
///
/// For each expression where IsBooleanValued(sig) is true and the
/// ANF candidate fails FullWidthCheckEval:
///
///   1. Compute Δ(x) = f_original(x) - f_ANF(x)
///   2. Confirm Δ is zero on {0,1}^n (sanity)
///   3. Evaluate Δ at random full-width points
///   4. Simplify Δ through the normal pipeline
///   5. Classify the delta:
///      - Pure carry terms (products that vanish on {0,1})
///      - Affine correction (linear Δ)
///      - Low-bit-mask localized
///      - Product shadow (x*y - x&y terms)
///      - Complex / unclassified

#include "ExprParser.h"
#include "cobra/core/AnfTransform.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Classification.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Evaluator.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/SignatureSimplifier.h"
#include "cobra/core/Simplifier.h"
#include "cobra/core/SimplifyOutcome.h"
#include "dataset_audit_utils.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace cobra;
using cobra::test_support::find_separator;
using cobra::test_support::flags_str;
using cobra::test_support::semantic_str;
using cobra::test_support::trim;

namespace {

    constexpr uint32_t kBw = 64;

    uint64_t abs_signed_delta(uint64_t d) {
        auto signed_d = static_cast< int64_t >(d);
        if (signed_d >= 0) { return d; }
        return static_cast< uint64_t >(~d) + 1U;
    }

    uint32_t active_bit_width(uint64_t d) { return static_cast< uint32_t >(std::bit_width(d)); }

    // ── Delta classification ──

    enum class DeltaFamily {
        kZero,            // Δ is identically zero (false positive)
        kProductShadow,   // Δ contains Mul(var,var) terms — x*y vs x&y
        kAffineMasked,    // Δ is linear (no products), possibly masked
        kPolynomialCarry, // Δ is polynomial (products present, no bitwise)
        kMixedCarry,      // Δ has both products and bitwise ops
        kSimplifiable,    // Δ simplifies through normal pipeline
        kComplex,         // Δ doesn't simplify and doesn't fit above
    };

    std::string delta_family_str(DeltaFamily f) {
        switch (f) {
            case DeltaFamily::kZero:
                return "zero";
            case DeltaFamily::kProductShadow:
                return "product_shadow";
            case DeltaFamily::kAffineMasked:
                return "affine_masked";
            case DeltaFamily::kPolynomialCarry:
                return "poly_carry";
            case DeltaFamily::kMixedCarry:
                return "mixed_carry";
            case DeltaFamily::kSimplifiable:
                return "simplifiable";
            case DeltaFamily::kComplex:
                return "complex";
        }
        return "?";
    }

    struct DeltaRecord
    {
        int line_num = 0;
        std::string dataset;
        uint32_t num_vars = 0;

        // Original expression classification
        SemanticClass orig_class  = SemanticClass::kLinear;
        StructuralFlag orig_flags = kSfNone;

        // ANF candidate
        uint32_t anf_cost = 0;

        // Full pipeline result (for comparison)
        double full_time_ms  = 0.0;
        bool full_simplified = false;
        uint32_t full_cost   = 0;
        uint32_t full_expns  = 0;

        // Delta properties
        bool delta_zero_on_boolean  = true; // sanity: Δ=0 on {0,1}^n
        uint64_t delta_max_abs      = 0;    // max |Δ| at random FW points
        uint32_t delta_nonzero_bits = 0;    // max observed active bit-width of Δ

        // Delta classification
        SemanticClass delta_class  = SemanticClass::kLinear;
        StructuralFlag delta_flags = kSfNone;
        DeltaFamily delta_family   = DeltaFamily::kComplex;

        // Delta simplification
        bool delta_simplified = false;
        uint32_t delta_cost   = 0;
        std::string delta_rendered;

        // Delta structural analysis
        bool has_mul_var_var   = false; // Mul(var-dep, var-dep) in delta expr
        bool has_bitwise       = false; // any bitwise in delta
        bool is_constant_delta = false; // Δ is constant across FW samples
    };

    // Check if an Expr contains Mul(var-dep, var-dep)
    bool has_product_of_vars(const Expr &e) {
        if (e.kind == Expr::Kind::kMul && e.children.size() == 2) {
            if (HasVarDep(*e.children[0]) && HasVarDep(*e.children[1])) { return true; }
        }
        return std::ranges::any_of(e.children, [](const auto &c) {
            return has_product_of_vars(*c);
        });
    }

    bool has_bitwise_op(const Expr &e) {
        if (e.kind == Expr::Kind::kAnd || e.kind == Expr::Kind::kOr
            || e.kind == Expr::Kind::kXor || e.kind == Expr::Kind::kNot)
        {
            return true;
        }
        return std::ranges::any_of(e.children, [](const auto &c) {
            return has_bitwise_op(*c);
        });
    }

    DeltaFamily classify_delta(
        const Expr &delta_expr, Classification delta_cls, bool is_constant, bool simplified
    ) {
        if (is_constant) { return DeltaFamily::kZero; }
        if (simplified) { return DeltaFamily::kSimplifiable; }

        bool has_mul = has_product_of_vars(delta_expr);
        bool has_bw  = has_bitwise_op(delta_expr);

        if (has_mul && !has_bw) {
            if (delta_cls.semantic == SemanticClass::kPolynomial
                || delta_cls.semantic == SemanticClass::kLinear)
            {
                return DeltaFamily::kPolynomialCarry;
            }
            return DeltaFamily::kProductShadow;
        }
        if (!has_mul && !has_bw) { return DeltaFamily::kAffineMasked; }
        if (has_mul && has_bw) { return DeltaFamily::kMixedCarry; }
        // has_bw but no mul
        return DeltaFamily::kAffineMasked;
    }

    struct DatasetConfig
    {
        std::string name;
        std::string path;
    };

    void run_delta_taxonomy(const DatasetConfig &ds, std::vector< DeltaRecord > &records) {
        std::ifstream file(ds.path);
        if (!file.is_open()) {
            std::cerr << "Cannot open " << ds.path << "\n";
            return;
        }

        std::string line;
        int line_num = 0;

        while (std::getline(file, line)) {
            ++line_num;
            if (line.empty()) { continue; }

            size_t sep = find_separator(line);
            if (sep == std::string::npos) { continue; }

            std::string obfuscated = trim(line.substr(0, sep));
            if (obfuscated.empty()) { continue; }

            auto parse_result = ParseAndEvaluate(obfuscated, kBw);
            if (!parse_result.has_value()) { continue; }

            auto ast_result = ParseToAst(obfuscated, kBw);
            if (!ast_result.has_value()) { continue; }

            auto folded      = FoldConstantBitwise(std::move(ast_result.value().expr), kBw);
            const auto &sig  = parse_result.value().sig;
            const auto &vars = parse_result.value().vars;
            auto num_vars    = static_cast< uint32_t >(vars.size());

            // Gate: only boolean-valued signatures
            if (!IsBooleanValued(sig)) { continue; }
            if (num_vars > 16) { continue; }

            // Compute ANF
            auto anf      = ComputeAnf(sig, num_vars);
            auto anf_expr = BuildAnfExpr(anf, num_vars);

            // Gate: must FAIL full-width check
            auto orig_eval = Evaluator::FromExpr(*folded, kBw);
            auto fw        = FullWidthCheckEval(orig_eval, num_vars, *anf_expr, kBw);
            if (fw.passed) { continue; } // Not a fastpath_verify_fail

            DeltaRecord rec;
            rec.line_num = line_num;
            rec.dataset  = ds.name;
            rec.num_vars = num_vars;
            rec.anf_cost = ComputeCost(*anf_expr).cost.weighted_size;

            // Original classification
            auto orig_cls  = ClassifyStructural(*folded);
            rec.orig_class = orig_cls.semantic;
            rec.orig_flags = orig_cls.flags;

            // ── Build delta expression: original - ANF ──
            // Δ(x) = f_original(x) - f_ANF(x)
            auto delta_expr = Expr::Add(CloneExpr(*folded), Expr::Negate(CloneExpr(*anf_expr)));
            auto delta_folded = FoldConstantBitwise(std::move(delta_expr), kBw);

            // Classify delta structurally
            auto delta_cls      = ClassifyStructural(*delta_folded);
            rec.delta_class     = delta_cls.semantic;
            rec.delta_flags     = delta_cls.flags;
            rec.has_mul_var_var = has_product_of_vars(*delta_folded);
            rec.has_bitwise     = has_bitwise_op(*delta_folded);

            // ── Verify Δ=0 on {0,1}^n ──
            {
                uint32_t sig_len = 1U << num_vars;
                for (uint32_t pt = 0; pt < sig_len; ++pt) {
                    std::vector< uint64_t > vals(num_vars);
                    for (uint32_t v = 0; v < num_vars; ++v) { vals[v] = (pt >> v) & 1; }
                    uint64_t d = EvalExpr(*delta_folded, vals, kBw);
                    if (d != 0) {
                        rec.delta_zero_on_boolean = false;
                        break;
                    }
                }
            }

            // ── Evaluate Δ at random full-width points ──
            {
                std::mt19937_64 rng(42 + static_cast< uint64_t >(line_num));
                uint64_t max_abs         = 0;
                uint32_t max_active_bits = 0;
                bool all_same            = true;
                uint64_t first_val       = 0;

                for (int sample = 0; sample < 32; ++sample) {
                    std::vector< uint64_t > vals(num_vars);
                    for (uint32_t v = 0; v < num_vars; ++v) { vals[v] = rng(); }
                    uint64_t d = EvalExpr(*delta_folded, vals, kBw);
                    if (sample == 0) {
                        first_val = d;
                    } else if (d != first_val) {
                        all_same = false;
                    }

                    // Track magnitude (interpret as signed)
                    max_abs = std::max(max_abs, abs_signed_delta(d));

                    // Track active bits
                    max_active_bits = std::max(max_active_bits, active_bit_width(d));
                }
                rec.delta_max_abs      = max_abs;
                rec.delta_nonzero_bits = max_active_bits;
                rec.is_constant_delta  = all_same;
            }

            // ── Try to simplify Δ ──
            {
                auto delta_sig = EvaluateBooleanSignature(*delta_folded, num_vars, kBw);
                Options opts{ .bitwidth = kBw, .max_vars = 16, .spot_check = true };
                auto delta_result = Simplify(delta_sig, vars, delta_folded.get(), opts);
                if (delta_result.has_value()
                    && delta_result.value().kind == SimplifyOutcome::Kind::kSimplified)
                {
                    rec.delta_simplified = true;
                    rec.delta_cost = ComputeCost(*delta_result.value().expr).cost.weighted_size;
                    rec.delta_rendered =
                        Render(*delta_result.value().expr, delta_result.value().real_vars, kBw);
                }
            }

            // ── Classify delta family ──
            rec.delta_family = classify_delta(
                *delta_folded, delta_cls, rec.is_constant_delta, rec.delta_simplified
            );

            // ── Run full pipeline for comparison ──
            {
                Options opts{ .bitwidth = kBw, .max_vars = 16, .spot_check = true };
                auto t0          = std::chrono::steady_clock::now();
                auto result      = Simplify(sig, vars, folded.get(), opts);
                auto t1          = std::chrono::steady_clock::now();
                rec.full_time_ms = std::chrono::duration< double, std::milli >(t1 - t0).count();
                if (result.has_value()
                    && result.value().kind == SimplifyOutcome::Kind::kSimplified)
                {
                    rec.full_simplified = true;
                    rec.full_cost       = ComputeCost(*result.value().expr).cost.weighted_size;
                }
                rec.full_expns =
                    result.has_value() ? result.value().telemetry.total_expansions : 0;
            }

            records.push_back(rec);
        }
    }

    void print_taxonomy(const std::string &title, const std::vector< DeltaRecord > &records) {
        if (records.empty()) {
            std::cerr << "\n  " << title << ": no fastpath_verify_fail cases\n\n";
            return;
        }

        std::cerr << "\n╔═══════════════════════════════════════════════════════╗\n";
        std::cerr << "║  " << std::left << std::setw(52) << title << "║\n";
        std::cerr << "╚═══════════════════════════════════════════════════════╝\n\n";

        int total = static_cast< int >(records.size());
        std::cerr << "  Total fastpath_verify_fail: " << total << "\n\n";

        // Sanity check
        int delta_nonzero_boolean = 0;
        for (const auto &r : records) {
            if (!r.delta_zero_on_boolean) { delta_nonzero_boolean++; }
        }
        if (delta_nonzero_boolean > 0) {
            std::cerr << "  WARNING: " << delta_nonzero_boolean
                      << " deltas are nonzero on {0,1}^n!\n\n";
        }

        // Delta family distribution
        std::map< DeltaFamily, int > family_counts;
        std::map< DeltaFamily, double > family_time;
        for (const auto &r : records) {
            family_counts[r.delta_family]++;
            family_time[r.delta_family] += r.full_time_ms;
        }
        std::cerr << "  Delta family distribution:\n";
        std::cerr << "    " << std::left << std::setw(20) << "family" << std::right
                  << std::setw(8) << "count" << std::setw(12) << "total_ms" << "\n";
        std::cerr << "    " << std::string(40, '-') << "\n";
        for (const auto &[fam, cnt] : family_counts) {
            std::cerr << "    " << std::left << std::setw(20) << delta_family_str(fam)
                      << std::right << std::setw(8) << cnt << std::setw(12) << std::fixed
                      << std::setprecision(1) << family_time[fam] << "\n";
        }
        std::cerr << "\n";

        // Original classification distribution
        std::map< SemanticClass, int > orig_cls_counts;
        for (const auto &r : records) { orig_cls_counts[r.orig_class]++; }
        std::cerr << "  Original classification:\n";
        for (const auto &[cls, cnt] : orig_cls_counts) {
            std::cerr << "    " << semantic_str(cls) << ": " << cnt << "\n";
        }
        std::cerr << "\n";

        // Delta classification distribution
        std::map< SemanticClass, int > delta_cls_counts;
        for (const auto &r : records) { delta_cls_counts[r.delta_class]++; }
        std::cerr << "  Delta classification:\n";
        for (const auto &[cls, cnt] : delta_cls_counts) {
            std::cerr << "    " << semantic_str(cls) << ": " << cnt << "\n";
        }
        std::cerr << "\n";

        // Delta structural features
        int has_mul = 0, has_bw = 0, is_const = 0;
        for (const auto &r : records) {
            if (r.has_mul_var_var) { has_mul++; }
            if (r.has_bitwise) { has_bw++; }
            if (r.is_constant_delta) { is_const++; }
        }
        std::cerr << "  Delta structural features:\n";
        std::cerr << "    has Mul(var,var): " << has_mul << "\n";
        std::cerr << "    has bitwise:     " << has_bw << "\n";
        std::cerr << "    constant Δ:      " << is_const << "\n\n";

        // Variable count distribution
        std::map< uint32_t, int > var_counts;
        for (const auto &r : records) { var_counts[r.num_vars]++; }
        std::cerr << "  Variable count distribution:\n";
        for (const auto &[nv, cnt] : var_counts) {
            std::cerr << "    " << nv << " vars: " << cnt << "\n";
        }
        std::cerr << "\n";

        // Delta simplification success
        int simplified = 0;
        for (const auto &r : records) {
            if (r.delta_simplified) { simplified++; }
        }
        std::cerr << "  Delta simplified: " << simplified << " / " << total << "\n\n";

        // Show simplified deltas (the repair candidates)
        if (simplified > 0) {
            std::cerr << "  Simplified deltas (Δ = original - ANF):\n";
            int shown = 0;
            for (const auto &r : records) {
                if (!r.delta_simplified) { continue; }
                if (shown >= 20) {
                    std::cerr << "    ... and " << (simplified - 20) << " more\n";
                    break;
                }
                std::cerr << "    L" << r.line_num << " [" << r.num_vars << "v"
                          << " " << semantic_str(r.orig_class) << "]  Δ=" << r.delta_rendered
                          << "  (cost " << r.delta_cost << ", anf_cost " << r.anf_cost
                          << ", full_cost " << r.full_cost << ", " << std::setprecision(1)
                          << r.full_time_ms << "ms)\n";
                shown++;
            }
            std::cerr << "\n";
        }

        // Show unsimplified deltas (top 10 by time)
        std::vector< const DeltaRecord * > unsimplified;
        for (const auto &r : records) {
            if (!r.delta_simplified) { unsimplified.push_back(&r); }
        }
        std::sort(unsimplified.begin(), unsimplified.end(), [](const auto *a, const auto *b) {
            return a->full_time_ms > b->full_time_ms;
        });
        if (!unsimplified.empty()) {
            std::cerr << "  Top 10 unsimplified deltas (by time):\n";
            int shown = 0;
            for (const auto *r : unsimplified) {
                if (shown >= 10) { break; }
                std::cerr << "    L" << r->line_num << " [" << r->num_vars << "v"
                          << " " << semantic_str(r->orig_class) << "  "
                          << flags_str(r->orig_flags)
                          << "]  family=" << delta_family_str(r->delta_family)
                          << "  delta_class=" << semantic_str(r->delta_class) << "  "
                          << std::setprecision(1) << r->full_time_ms << "ms"
                          << "  expns=" << r->full_expns << "\n";
                shown++;
            }
            std::cerr << "\n";
        }
    }

} // namespace

TEST(FastpathDeltaTaxonomy, Syntia) {
    std::vector< DeltaRecord > records;
    run_delta_taxonomy({ "syntia", DATASET_DIR "/gamba/syntia.txt" }, records);
    print_taxonomy("Syntia — FP Verify-Fail Delta", records);
    EXPECT_FALSE(records.empty());
}

TEST(FastpathDeltaTaxonomy, QSynthEA) {
    std::vector< DeltaRecord > records;
    run_delta_taxonomy({ "qsynth", DATASET_DIR "/gamba/qsynth_ea.txt" }, records);
    print_taxonomy("QSynth — FP Verify-Fail Delta", records);
    EXPECT_TRUE(true); // some datasets may have 0
}

TEST(FastpathDeltaTaxonomy, Combined) {
    std::vector< DeltaRecord > all;

    DatasetConfig datasets[] = {
        {            "syntia",            DATASET_DIR "/gamba/syntia.txt" },
        {            "qsynth",         DATASET_DIR "/gamba/qsynth_ea.txt" },
        {         "neureduce",         DATASET_DIR "/gamba/neureduce.txt" },
        {    "mba_obf_linear",    DATASET_DIR "/gamba/mba_obf_linear.txt" },
        { "mba_obf_nonlinear", DATASET_DIR "/gamba/mba_obf_nonlinear.txt" },
        {         "loki_tiny",         DATASET_DIR "/gamba/loki_tiny.txt" },
    };

    for (const auto &ds : datasets) { run_delta_taxonomy(ds, all); }

    print_taxonomy("GAMBA Combined — FP Verify-Fail Delta", all);

    // Per-dataset breakdown
    std::map< std::string, std::map< DeltaFamily, int > > per_ds;
    for (const auto &r : all) { per_ds[r.dataset][r.delta_family]++; }
    std::cerr << "  Per-dataset family breakdown:\n";
    for (const auto &[dsname, families] : per_ds) {
        std::cerr << "    " << dsname << ": ";
        for (const auto &[fam, cnt] : families) {
            std::cerr << delta_family_str(fam) << "=" << cnt << " ";
        }
        std::cerr << "\n";
    }
    std::cerr << "\n";

    EXPECT_FALSE(all.empty());
}
