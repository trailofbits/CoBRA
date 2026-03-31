// test/verify/test_extended_semantics_tier1.cpp
//
// Tier 1: Extended-semantics probes on all QSynth unsupported
// expressions.  Produces structured fingerprints and multi-label
// cluster assignments.

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
#include <map>
#include <memory>
#include <set>
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

    ReasonInfo ExtractTopReason(const Diagnostic &diag) {
        ReasonInfo info;
        if (diag.reason_code.has_value()) {
            info.category = diag.reason_code->category;
            info.domain   = diag.reason_code->domain;
            info.subcode  = diag.reason_code->subcode;
        }
        return info;
    }

    ReasonInfo ExtractTerminalReason(const Diagnostic &diag) {
        ReasonInfo info;
        if (!diag.cause_chain.empty()) {
            const auto &last = diag.cause_chain.back();
            info.category    = last.code.category;
            info.domain      = last.code.domain;
            info.subcode     = last.code.subcode;
        } else if (diag.reason_code.has_value()) {
            info.category = diag.reason_code->category;
            info.domain   = diag.reason_code->domain;
            info.subcode  = diag.reason_code->subcode;
        }
        return info;
    }

    std::string ExtractTerminalMessage(const Diagnostic &diag) {
        if (!diag.cause_chain.empty()) { return diag.cause_chain.back().message; }
        return diag.reason;
    }

} // namespace

TEST(ExtendedSemanticsTier1, FullFrontierProbe) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    std::vector< ExtendedSemanticsRecord > records;
    int total_parsed     = 0;
    int total_simplified = 0;
    int total_unsup      = 0;

    std::string line;
    int line_num           = 0;
    constexpr uint32_t kBw = 64;

    while (std::getline(file, line)) {
        ++line_num;
        if (line.empty() || line[0] == '#') { continue; }

        size_t sep = find_separator(line);
        if (sep == std::string::npos) { continue; }

        std::string obfuscated = trim(line.substr(0, sep));
        if (obfuscated.empty()) { continue; }

        auto parse = ParseAndEvaluate(obfuscated, kBw);
        if (!parse.has_value()) { continue; }

        auto ast = ParseToAst(obfuscated, kBw);
        if (!ast.has_value()) { continue; }

        auto folded_ptr = std::make_shared< std::unique_ptr< Expr > >(
            FoldConstantBitwise(std::move(ast.value().expr), kBw)
        );

        ++total_parsed;

        const auto &sig  = parse.value().sig;
        const auto &vars = parse.value().vars;
        auto nv          = static_cast< uint32_t >(vars.size());

        Options opts{ .bitwidth = kBw, .max_vars = 16, .spot_check = true };
        opts.evaluator = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, 64);
        };

        auto result = Simplify(sig, vars, folded_ptr->get(), opts);
        if (!result.has_value()) { continue; }

        if (result.value().kind == SimplifyOutcome::Kind::kSimplified) {
            ++total_simplified;
            continue;
        }
        if (result.value().kind != SimplifyOutcome::Kind::kUnchangedUnsupported) {
            continue; // skip kError — not a valid unsupported case
        }
        ++total_unsup;

        const auto &out = result.value();

        // ── Build record identity fields ─────────────────────
        ExtendedSemanticsRecord rec;
        rec.line_num = line_num;

        auto cls     = ClassifyStructural(**folded_ptr);
        rec.semantic = cls.semantic;
        rec.flags    = cls.flags;

        auto bool_elim     = EliminateAuxVars(sig, vars);
        rec.bool_real_vars = static_cast< uint32_t >(bool_elim.real_vars.size());

        auto fw_elim     = EliminateAuxVars(sig, vars, opts.evaluator, kBw);
        rec.fw_real_vars = static_cast< uint32_t >(fw_elim.real_vars.size());

        rec.top_reason         = ExtractTopReason(out.diag);
        rec.top_reason_message = out.diag.reason;
        rec.terminal_reason    = ExtractTerminalReason(out.diag);
        rec.terminal_message   = ExtractTerminalMessage(out.diag);

        // ── Build FW-local residual evaluator ────────────────
        // CoB from fw_elim.reduced_sig in fw_elim.real_vars order
        uint32_t fw_nv = rec.fw_real_vars;
        auto fw_coeffs = InterpolateCoefficients(fw_elim.reduced_sig, fw_nv, kBw);
        auto fw_cob    = BuildCobExpr(fw_coeffs, fw_nv, kBw);

        // Map fw_elim.real_vars back to original variable indices
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
            // Map FW-local vars to original variable space
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

        // ── Run all Tier 1 probes ────────────────────────────
        ProbeConfig cfg;
        ProbeContext ctx{ residual_eval, fw_nv, cfg };

        rec.boolean_null   = ProbeBooleanNull(ctx);
        rec.slices         = ProbeUnivariateSlices(ctx);
        rec.periodicity    = ProbePeriodicity(rec.slices);
        rec.overlap        = ProbeOverlapSensitivity(ctx);
        rec.diagonal       = ProbeDiagonalStructure(ctx);
        rec.bit_dependency = ProbeBitDependency(ctx);
        rec.small_diagonal = ProbeSmallDiagonal(ctx);
        rec.parity         = ProbeParity(ctx);

        records.push_back(std::move(rec));
    }

    // ═══════════════════════════════════════════════════════════
    //  Clustering
    // ═══════════════════════════════════════════════════════════

    auto assignments = AssignClusters(records);
    auto summaries   = BuildClusterSummaries(assignments, records);

    // ═══════════════════════════════════════════════════════════
    //  Report Section 1: Summary
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n";
    std::cerr << "═══════════════════════════════════════════════════════════\n";
    std::cerr << "  EXTENDED-SEMANTICS TIER 1 PROBE SUITE\n";
    std::cerr << "  Parsed: " << total_parsed << "  Simplified: " << total_simplified
              << "  Unsupported: " << total_unsup << "  Probed: " << records.size() << "\n";
    std::cerr << "═══════════════════════════════════════════════════════════\n";

    // ── Summary table ────────────────────────────────────────
    std::cerr << "\n── Per-expression summary ──────────────────────────────\n";
    std::cerr << std::left << std::setw(5) << "Line" << std::setw(4) << "BV" << std::setw(4)
              << "FV" << std::setw(26) << "TopReason" << std::setw(26) << "TermReason"
              << std::setw(20) << "Flags" << std::setw(5) << "BcZ" << std::setw(5) << "FwW"
              << std::setw(4) << "BN" << std::setw(6) << "Per" << std::setw(5) << "Ovl"
              << std::setw(5) << "Diag" << std::setw(5) << "Dep" << std::setw(5) << "SDZ"
              << std::setw(5) << "Par"
              << "\n";

    for (const auto &rec : records) {
        bool bn = IsDerivedBooleanNull(rec.boolean_null);
        std::cerr << std::left << std::setw(5) << rec.line_num << std::setw(4)
                  << rec.bool_real_vars << std::setw(4) << rec.fw_real_vars << std::setw(26)
                  << ReasonStr(rec.top_reason) << std::setw(26)
                  << ReasonStr(rec.terminal_reason) << std::setw(20) << FlagStr(rec.flags)
                  << std::setw(5) << (rec.boolean_null.zero_on_boolean_cube ? "Y" : "n")
                  << std::setw(5)
                  << (rec.boolean_null.found_nonzero_full_width_witness ? "Y" : "n")
                  << std::setw(4) << (bn ? "Y" : "n") << std::setw(6)
                  << rec.periodicity.dominant_period << std::setw(5)
                  << (rec.overlap.suggests_overlap_sensitivity ? "Y" : "n") << std::setw(5)
                  << (rec.diagonal.full_all_zero ? "Y" : "n") << std::setw(5)
                  << rec.bit_dependency.estimated_influence_radius << std::setw(5)
                  << (rec.small_diagonal.all_zero ? "Y" : "n") << std::setw(5)
                  << (rec.parity.suggests_parity_dependence ? "Y" : "n") << "\n";
    }

    // ═══════════════════════════════════════════════════════════
    //  Report Section 2: Periodicity distribution
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n── Periodicity distribution ────────────────────────────\n";
    std::map< uint32_t, int > period_dist;
    for (const auto &rec : records) { period_dist[rec.periodicity.dominant_period]++; }
    for (const auto &[period, count] : period_dist) {
        std::cerr << "  period=" << std::setw(3) << period << " : " << count
                  << " expressions\n";
    }

    // Cross-tab: period × terminal domain
    std::cerr << "\n  Period × terminal domain:\n";
    std::map< std::string, std::map< uint32_t, int > > domain_period;
    for (const auto &rec : records) {
        domain_period[DomainStr(rec.terminal_reason.domain)][rec.periodicity.dominant_period]++;
    }
    for (const auto &[domain, periods] : domain_period) {
        std::cerr << "  " << std::setw(14) << domain << ": ";
        for (const auto &[p, c] : periods) { std::cerr << "p" << p << "=" << c << " "; }
        std::cerr << "\n";
    }

    // ═══════════════════════════════════════════════════════════
    //  Report Section 3: Overlap sensitivity detail
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n── Overlap sensitivity detail ──────────────────────────\n";
    for (const auto &rec : records) {
        if (!rec.overlap.suggests_overlap_sensitivity) { continue; }
        std::cerr << "  L" << rec.line_num << ":";
        for (const auto &p : rec.overlap.pairs) {
            double dz = (p.disjoint_sample_count > 0)
                ? static_cast< double >(p.residual_zero_when_disjoint)
                    / static_cast< double >(p.disjoint_sample_count)
                : 0;
            double oz = (p.overlap_sample_count > 0)
                ? static_cast< double >(p.residual_zero_when_overlap)
                    / static_cast< double >(p.overlap_sample_count)
                : 0;
            std::cerr << " (" << p.var_i << "," << p.var_j
                      << " dz=" << static_cast< int >(dz * 100)
                      << "% oz=" << static_cast< int >(oz * 100) << "%)";
        }
        std::cerr << "\n";
    }

    // ═══════════════════════════════════════════════════════════
    //  Report Section 4: Bit dependency profile
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n── Bit dependency profile ──────────────────────────────\n";
    std::map< uint32_t, int > radius_dist;
    for (const auto &rec : records) {
        radius_dist[rec.bit_dependency.estimated_influence_radius]++;
    }
    for (const auto &[radius, count] : radius_dist) {
        std::cerr << "  radius=" << std::setw(2) << radius << " : " << count
                  << " expressions\n";
    }

    // ═══════════════════════════════════════════════════════════
    //  Report Section 5: Probe correlation matrix
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n── Probe correlation matrix ────────────────────────────\n";

    // Build per-record tag sets
    std::map< int, std::set< std::string > > tag_sets;
    for (const auto &ca : assignments) {
        for (const auto &t : ca.tags) { tag_sets[ca.line_num].insert(t); }
    }

    auto count_co = [&](const std::string &a, const std::string &b) {
        int n = 0;
        for (const auto &[line, tags] : tag_sets) {
            if (tags.count(a) && tags.count(b)) { ++n; }
        }
        return n;
    };

    // Collect all non-untagged tags
    std::set< std::string > all_tags;
    for (const auto &ca : assignments) {
        for (const auto &t : ca.tags) {
            if (t != "untagged") { all_tags.insert(t); }
        }
    }

    std::vector< std::string > tag_list(all_tags.begin(), all_tags.end());
    for (size_t i = 0; i < tag_list.size(); ++i) {
        for (size_t j = i + 1; j < tag_list.size(); ++j) {
            int co = count_co(tag_list[i], tag_list[j]);
            if (co > 0) {
                std::cerr << "  " << tag_list[i] << " + " << tag_list[j] << " : " << co << "\n";
            }
        }
    }

    // Terminal domain × tag
    std::cerr << "\n  Terminal domain × tag:\n";
    std::map< std::string, std::map< std::string, int > > dom_tag;
    for (size_t i = 0; i < records.size(); ++i) {
        std::string dom = DomainStr(records[i].terminal_reason.domain);
        for (const auto &t : assignments[i].tags) { dom_tag[dom][t]++; }
    }
    for (const auto &[dom, tags] : dom_tag) {
        std::cerr << "  " << std::setw(14) << dom << ":";
        for (const auto &[t, c] : tags) { std::cerr << " " << t << "=" << c; }
        std::cerr << "\n";
    }

    // ═══════════════════════════════════════════════════════════
    //  Report Section 6: Cluster assignment table
    // ═══════════════════════════════════════════════════════════

    int multi_tag_count = 0;
    for (const auto &ca : assignments) {
        if (ca.tags.size() > 1 || (ca.tags.size() == 1 && ca.tags[0] != "untagged")) {
            if (ca.tags.size() > 1) { ++multi_tag_count; }
        }
    }

    std::cerr << "\n── Cluster assignments ─────────────────────────────────\n";
    std::cerr << "  (" << multi_tag_count << " expressions carry multiple tags)\n\n";

    for (const auto &ca : assignments) {
        std::cerr << "  L" << std::setw(3) << ca.line_num << " [";
        for (size_t i = 0; i < ca.tags.size(); ++i) {
            if (i > 0) { std::cerr << ", "; }
            std::cerr << ca.tags[i];
        }
        std::cerr << "]";
        if (!ca.defining_features.empty()) { std::cerr << " — " << ca.defining_features[0]; }
        std::cerr << "\n";
    }

    // ═══════════════════════════════════════════════════════════
    //  Report Section 7: Cluster summaries
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n── Cluster summaries ───────────────────────────────────\n";
    for (const auto &cs : summaries) {
        // Count how many members also appear in other groups
        int multi = 0;
        for (int ln : cs.line_nums) {
            auto it = tag_sets.find(ln);
            if (it != tag_sets.end() && it->second.size() > 1) { ++multi; }
        }

        std::cerr << "\n  [" << cs.tag << "] " << cs.line_nums.size() << " members"
                  << " (vars " << cs.min_vars << "-" << cs.max_vars << ")"
                  << " (" << multi << " of " << cs.line_nums.size()
                  << " also in other groups)\n";

        std::cerr << "    Lines: {";
        for (size_t i = 0; i < cs.line_nums.size(); ++i) {
            if (i > 0) { std::cerr << ", "; }
            std::cerr << cs.line_nums[i];
        }
        std::cerr << "}\n";

        std::cerr << "    Representatives: {";
        for (size_t i = 0; i < cs.representative_lines.size(); ++i) {
            if (i > 0) { std::cerr << ", "; }
            std::cerr << cs.representative_lines[i];
        }
        std::cerr << "}\n";

        if (!cs.defining_features.empty()) {
            std::cerr << "    Features: " << cs.defining_features[0] << "\n";
        }
    }

    std::cerr << "\n";

    // ═══════════════════════════════════════════════════════════
    //  Assertions
    // ═══════════════════════════════════════════════════════════

    EXPECT_GT(total_unsup, 0);
    EXPECT_EQ(records.size(), static_cast< size_t >(total_unsup));

    // Probe coverage
    for (const auto &rec : records) {
        EXPECT_TRUE(rec.boolean_null.tested)
            << "L" << rec.line_num << " boolean_null not tested";
        EXPECT_GT(rec.boolean_null.boolean_sample_count, 0u) << "L" << rec.line_num;
        EXPECT_GT(rec.boolean_null.full_width_probe_count, 0u) << "L" << rec.line_num;
        EXPECT_TRUE(rec.periodicity.tested) << "L" << rec.line_num;
        EXPECT_TRUE(rec.bit_dependency.tested) << "L" << rec.line_num;
        EXPECT_GT(rec.bit_dependency.max_bit_tested, 0u) << "L" << rec.line_num;

        if (rec.fw_real_vars >= 2) {
            EXPECT_TRUE(rec.overlap.tested) << "L" << rec.line_num;
            EXPECT_GT(rec.overlap.pairs.size(), 0u) << "L" << rec.line_num;
            EXPECT_TRUE(rec.diagonal.tested) << "L" << rec.line_num;
        }
    }

    // Cluster coverage
    EXPECT_EQ(assignments.size(), records.size());
    bool any_non_untagged = false;
    for (const auto &ca : assignments) {
        for (const auto &t : ca.tags) {
            if (t != "untagged") {
                any_non_untagged = true;
                break;
            }
        }
    }
    EXPECT_TRUE(any_non_untagged) << "No probe triggered any classification";
}
