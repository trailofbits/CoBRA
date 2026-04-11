// Mask pattern audit for unsupported expressions.
//
// Scans QSynth unsupported cases for contiguous low-bit mask patterns
// at the root, near-root (depth 1-2), and after pre-simplification.
// Also checks for mask patterns that could appear after lifting.
//
// Usage: ./bench_mask_audit (manual, not in CI)

#include "ExprParser.h"
#include "cobra/core/DynamicMask.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/PatternMatcher.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/Simplifier.h"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace cobra;

namespace {

    struct MaskAuditResult
    {
        uint32_t line            = 0;
        bool has_root_mask       = false;
        uint32_t root_mask_width = 0;
        bool has_depth1_mask     = false;
        uint32_t depth1_count    = 0;
        bool has_depth2_mask     = false;
        uint32_t depth2_count    = 0;
        bool has_any_interior    = false;
        uint32_t interior_count  = 0;
        bool has_shr             = false;
        std::string outcome;
        uint32_t node_count = 0;
    };

    uint32_t CountNodes(const Expr &e) {
        uint32_t n = 1;
        for (const auto &c : e.children) { n += CountNodes(*c); }
        return n;
    }

    // Check if a node is And(expr, 2^m-1) or And(2^m-1, expr).
    bool IsLowBitMask(const Expr &expr, uint32_t bitwidth) {
        return DetectRootLowBitMask(expr, bitwidth).has_value();
    }

    // Count mask patterns at a given depth.
    void CountMasksAtDepth(
        const Expr &expr, uint32_t bitwidth, uint32_t current_depth, uint32_t target_depth,
        uint32_t &count
    ) {
        if (current_depth == target_depth) {
            if (IsLowBitMask(expr, bitwidth)) { count++; }
            return;
        }
        for (const auto &child : expr.children) {
            CountMasksAtDepth(*child, bitwidth, current_depth + 1, target_depth, count);
        }
    }

    // Count ALL interior mask patterns (any depth).
    uint32_t CountAllInteriorMasks(const Expr &expr, uint32_t bitwidth) {
        uint32_t count = 0;
        for (const auto &child : expr.children) {
            if (IsLowBitMask(*child, bitwidth)) { count++; }
            count += CountAllInteriorMasks(*child, bitwidth);
        }
        return count;
    }

    void RunMaskAudit(const std::string &dataset_name, const std::string &path) {
        std::ifstream in(path);
        ASSERT_TRUE(in.good()) << "Cannot open " << path;

        std::vector< MaskAuditResult > unsupported;
        std::vector< MaskAuditResult > all_results;
        std::string line;
        uint32_t lineno = 0;

        while (std::getline(in, line)) {
            lineno++;
            if (line.empty() || line[0] == '#') { continue; }
            auto sep = line.find(',');
            if (sep == std::string::npos) { sep = line.find('\t'); }
            std::string expr_str = (sep != std::string::npos) ? line.substr(0, sep) : line;

            auto ast_result = ParseToAst(expr_str, 64);
            if (!ast_result.has_value()) { continue; }

            const auto &expr = *ast_result->expr;
            MaskAuditResult r;
            r.line       = lineno;
            r.node_count = CountNodes(expr);
            r.has_shr    = ContainsShr(expr);

            // Root mask
            auto root_mask    = DetectRootLowBitMask(expr, 64);
            r.has_root_mask   = root_mask.has_value();
            r.root_mask_width = root_mask ? root_mask->effective_width : 0;

            // Depth 1 masks
            CountMasksAtDepth(expr, 64, 0, 1, r.depth1_count);
            r.has_depth1_mask = r.depth1_count > 0;

            // Depth 2 masks
            CountMasksAtDepth(expr, 64, 0, 2, r.depth2_count);
            r.has_depth2_mask = r.depth2_count > 0;

            // Any interior masks
            r.interior_count   = CountAllInteriorMasks(expr, 64);
            r.has_any_interior = r.interior_count > 0;

            // Run simplifier to check outcome
            auto parse_result = ParseAndEvaluate(expr_str, 64);
            if (parse_result.has_value()) {
                Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
                auto result = Simplify(
                    parse_result->sig, parse_result->vars, ast_result->expr.get(), opts
                );
                if (result.has_value()) {
                    r.outcome = (result->kind == SimplifyOutcome::Kind::kSimplified)
                        ? "simplified"
                        : "unsupported";
                } else {
                    r.outcome = "error";
                }
            }

            all_results.push_back(r);
            if (r.outcome == "unsupported") { unsupported.push_back(r); }
        }

        // Report
        std::cout << "\n=== " << dataset_name << " Mask Audit ===\n";
        std::cout << "total expressions: " << all_results.size() << "\n";
        std::cout << "unsupported: " << unsupported.size() << "\n";

        // Aggregate stats across unsupported
        uint32_t root_masks   = 0;
        uint32_t depth1_masks = 0;
        uint32_t depth2_masks = 0;
        uint32_t interior_any = 0;
        uint32_t has_shr      = 0;

        for (const auto &r : unsupported) {
            if (r.has_root_mask) { root_masks++; }
            if (r.has_depth1_mask) { depth1_masks++; }
            if (r.has_depth2_mask) { depth2_masks++; }
            if (r.has_any_interior) { interior_any++; }
            if (r.has_shr) { has_shr++; }
        }

        std::cout << "\nUnsupported expressions with mask patterns:\n";
        std::cout << "  root-level mask:      " << root_masks << "\n";
        std::cout << "  depth-1 child mask:   " << depth1_masks << "\n";
        std::cout << "  depth-2 child mask:   " << depth2_masks << "\n";
        std::cout << "  any interior mask:    " << interior_any << "\n";
        std::cout << "  contains kShr:        " << has_shr << "\n";

        // Also check across ALL expressions
        uint32_t all_root     = 0;
        uint32_t all_interior = 0;
        for (const auto &r : all_results) {
            if (r.has_root_mask) { all_root++; }
            if (r.has_any_interior) { all_interior++; }
        }
        std::cout << "\nAll expressions with mask patterns:\n";
        std::cout << "  root-level mask:      " << all_root << "\n";
        std::cout << "  any interior mask:    " << all_interior << "\n";

        // Detail for unsupported with masks
        if (interior_any > 0) {
            std::cout << "\nUnsupported with interior masks (detail):\n";
            std::cout << std::left << std::setw(8) << "line" << std::setw(8) << "nodes"
                      << std::setw(8) << "root" << std::setw(8) << "d1" << std::setw(8) << "d2"
                      << std::setw(10) << "interior" << std::setw(6) << "shr"
                      << "\n";
            std::cout << std::string(56, '-') << "\n";
            for (const auto &r : unsupported) {
                if (!r.has_any_interior) { continue; }
                std::cout << std::left << std::setw(8) << r.line << std::setw(8) << r.node_count
                          << std::setw(8) << (r.has_root_mask ? "YES" : "no") << std::setw(8)
                          << r.depth1_count << std::setw(8) << r.depth2_count << std::setw(10)
                          << r.interior_count << std::setw(6) << (r.has_shr ? "YES" : "no")
                          << "\n";
            }
        }
    }

} // namespace

TEST(MaskAudit, QSynthEA) { RunMaskAudit("QSynth EA", DATASET_DIR "/gamba/qsynth_ea.txt"); }

TEST(MaskAudit, Syntia) { RunMaskAudit("Syntia", DATASET_DIR "/gamba/syntia.txt"); }
