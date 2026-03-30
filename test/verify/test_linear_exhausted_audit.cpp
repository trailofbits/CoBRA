#include "ExprParser.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/CoBExprBuilder.h"
#include "cobra/core/CoeffInterpolator.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/Simplifier.h"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <iomanip>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using namespace cobra;

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
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) { return ""; }
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    std::string semantic_str(SemanticClass s) {
        switch (s) {
            case SemanticClass::kLinear:
                return "Linear";
            case SemanticClass::kSemilinear:
                return "Semilinear";
            case SemanticClass::kPolynomial:
                return "Polynomial";
            case SemanticClass::kNonPolynomial:
                return "NonPoly";
        }
        return "?";
    }

    std::string flag_str(StructuralFlag f) {
        std::string s;
        if (HasFlag(f, kSfHasBitwise)) { s += "Bw|"; }
        if (HasFlag(f, kSfHasArithmetic)) { s += "Ar|"; }
        if (HasFlag(f, kSfHasMul)) { s += "Mul|"; }
        if (HasFlag(f, kSfHasMultilinearProduct)) { s += "Mlp|"; }
        if (HasFlag(f, kSfHasSingletonPower)) { s += "Pow|"; }
        if (HasFlag(f, kSfHasMixedProduct)) { s += "MxP|"; }
        if (HasFlag(f, kSfHasBitwiseOverArith)) { s += "BoA|"; }
        if (HasFlag(f, kSfHasArithOverBitwise)) { s += "AoB|"; }
        if (HasFlag(f, kSfHasMultivarHighPower)) { s += "HiP|"; }
        if (!s.empty()) { s.pop_back(); }
        return s;
    }

    std::string category_str(ReasonCategory cat) {
        switch (cat) {
            case ReasonCategory::kSearchExhausted:
                return "search-exhausted";
            case ReasonCategory::kVerifyFailed:
                return "verify-failed";
            case ReasonCategory::kRepresentationGap:
                return "representation-gap";
            case ReasonCategory::kNoSolution:
                return "no-solution";
            case ReasonCategory::kResourceLimit:
                return "resource-limit";
            case ReasonCategory::kGuardFailed:
                return "guard-failed";
            case ReasonCategory::kInapplicable:
                return "inapplicable";
            case ReasonCategory::kCostRejected:
                return "cost-rejected";
            case ReasonCategory::kInternalInvariant:
                return "internal";
            case ReasonCategory::kNone:
                return "none";
        }
        return "unknown";
    }

    uint32_t count_nodes(const Expr &e) {
        uint32_t n = 1;
        for (const auto &c : e.children) { n += count_nodes(*c); }
        return n;
    }

    bool has_var_dep(const Expr &e) { return HasVarDep(e); }

    // Check if node has arithmetic under bitwise
    bool has_arith_under_bitwise(const Expr &e, bool under_bw) {
        bool is_bw =
            (e.kind == Expr::Kind::kAnd || e.kind == Expr::Kind::kOr
             || e.kind == Expr::Kind::kXor || e.kind == Expr::Kind::kNot);
        bool is_arith = (e.kind == Expr::Kind::kAdd || e.kind == Expr::Kind::kNeg);
        if (under_bw && is_arith) { return true; }
        for (const auto &c : e.children) {
            if (has_arith_under_bitwise(*c, is_bw)) { return true; }
        }
        return false;
    }

    // Count distinct subtree hashes (for duplication analysis)
    struct DupInfo
    {
        uint32_t total_subtrees = 0;
        uint32_t dup_subtrees   = 0;
        uint32_t largest_dup    = 0;
        uint32_t max_repeat     = 0;
    };

    void collect_hashes(
        const Expr &e, std::unordered_map< size_t, uint32_t > &counts,
        std::unordered_map< size_t, uint32_t > &sizes
    ) {
        if (e.kind != Expr::Kind::kConstant && e.kind != Expr::Kind::kVariable) {
            size_t h = std::hash< Expr >{}(e);
            counts[h]++;
            if (sizes.find(h) == sizes.end()) { sizes[h] = count_nodes(e); }
        }
        for (const auto &c : e.children) { collect_hashes(*c, counts, sizes); }
    }

    DupInfo compute_dup_info(const Expr &e) {
        std::unordered_map< size_t, uint32_t > counts;
        std::unordered_map< size_t, uint32_t > sizes;
        collect_hashes(e, counts, sizes);

        DupInfo info;
        for (const auto &[h, cnt] : counts) {
            info.total_subtrees += cnt;
            if (cnt >= 2) {
                info.dup_subtrees += cnt;
                if (cnt > info.max_repeat) { info.max_repeat = cnt; }
                if (sizes[h] > info.largest_dup) { info.largest_dup = sizes[h]; }
            }
        }
        return info;
    }

    // Replicate budgeted lifting to see what the outer looks like
    struct LiftEntry
    {
        size_t hash;
        std::string rendered;
        const Expr *ptr;
        uint32_t count;
        uint32_t size;
    };

    void collect_repeats(
        const Expr &node, const std::vector< std::string > &vars, uint32_t bw,
        std::unordered_map< size_t, std::vector< size_t > > &by_hash,
        std::vector< LiftEntry > &entries
    ) {
        bool is_leaf =
            (node.kind == Expr::Kind::kConstant || node.kind == Expr::Kind::kVariable);
        if (!is_leaf) {
            size_t h      = std::hash< Expr >{}(node);
            auto rendered = Render(node, vars, bw);
            auto sz       = count_nodes(node);

            bool found = false;
            auto hit   = by_hash.find(h);
            if (hit != by_hash.end()) {
                for (size_t idx : hit->second) {
                    if (entries[idx].rendered == rendered) {
                        entries[idx].count++;
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                by_hash[h].push_back(entries.size());
                entries.push_back(
                    LiftEntry{ .hash     = h,
                               .rendered = std::move(rendered),
                               .ptr      = &node,
                               .count    = 1,
                               .size     = sz }
                );
            }
        }
        for (const auto &c : node.children) { collect_repeats(*c, vars, bw, by_hash, entries); }
    }

    bool is_ancestor_of(const Expr *a, const Expr *d) {
        if (a == d) { return true; }
        for (const auto &c : a->children) {
            if (is_ancestor_of(c.get(), d)) { return true; }
        }
        return false;
    }

    std::unique_ptr< Expr > replace_with_virtual(
        const Expr &node, const std::vector< std::pair< size_t, std::string > > &targets,
        const std::vector< uint32_t > &virt_idx, const std::vector< std::string > &vars,
        uint32_t bw
    ) {
        if (node.kind != Expr::Kind::kConstant && node.kind != Expr::Kind::kVariable) {
            size_t h      = std::hash< Expr >{}(node);
            auto rendered = Render(node, vars, bw);
            for (size_t i = 0; i < targets.size(); ++i) {
                if (targets[i].first == h && targets[i].second == rendered) {
                    return Expr::Variable(virt_idx[i]);
                }
            }
        }
        auto r          = std::make_unique< Expr >();
        r->kind         = node.kind;
        r->constant_val = node.constant_val;
        r->var_index    = node.var_index;
        for (const auto &c : node.children) {
            r->children.push_back(replace_with_virtual(*c, targets, virt_idx, vars, bw));
        }
        return r;
    }

    // The 19 Linear search-exhausted line numbers
    const std::vector< int > kLinearExhausted = { 24,  56,  79,  85,  89,  96,  115,
                                                  131, 199, 206, 221, 244, 259, 266,
                                                  276, 301, 321, 344, 473 };

} // namespace

TEST(LinearExhaustedAudit, DeepDiagnostic) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    // Read all lines first
    std::vector< std::string > lines;
    std::string line;
    while (std::getline(file, line)) { lines.push_back(line); }

    std::set< int > target_set(kLinearExhausted.begin(), kLinearExhausted.end());

    // Track aggregate stats
    int total_arith_under_bw = 0;
    int total_cob_bool_ok    = 0;
    int total_cob_fw_fail    = 0;
    int total_lift_viable    = 0;
    int total_lift_selected  = 0;
    std::map< std::string, int > terminal_reasons;
    std::map< std::string, int > outer_classes;
    std::map< std::string, int > cob_term_patterns;

    std::cerr << "\n";
    std::cerr << "═══════════════════════════════════════════\n";
    std::cerr << "  LINEAR SEARCH-EXHAUSTED DEEP AUDIT\n";
    std::cerr << "  19 cases from QSynth\n";
    std::cerr << "═══════════════════════════════════════════\n";

    for (int target_line : kLinearExhausted) {
        if (target_line < 1 || target_line > static_cast< int >(lines.size())) { continue; }
        const auto &raw = lines[target_line - 1];
        if (raw.empty() || raw[0] == '#') { continue; }

        size_t sep = find_separator(raw);
        if (sep == std::string::npos) { continue; }

        std::string obfuscated = trim(raw.substr(0, sep));
        std::string gt_str     = trim(raw.substr(sep + 1));
        if (obfuscated.empty()) { continue; }

        auto parse = ParseAndEvaluate(obfuscated, 64);
        if (!parse.has_value()) { continue; }
        auto ast = ParseToAst(obfuscated, 64);
        if (!ast.has_value()) { continue; }

        auto folded_ptr = std::make_shared< std::unique_ptr< Expr > >(
            FoldConstantBitwise(std::move(ast.value().expr), 64)
        );

        auto cls   = ClassifyStructural(**folded_ptr);
        auto elim  = EliminateAuxVars(parse.value().sig, parse.value().vars);
        auto rv    = static_cast< uint32_t >(elim.real_vars.size());
        auto nodes = count_nodes(**folded_ptr);
        bool aub   = has_arith_under_bitwise(**folded_ptr, false);
        if (aub) { total_arith_under_bw++; }

        std::cerr << "\n────────────────────────────────────\n";
        std::cerr << "L" << target_line << " vars=" << rv << " nodes=" << nodes
                  << " sem=" << semantic_str(cls.semantic) << " flags={" << flag_str(cls.flags)
                  << "}"
                  << " aub=" << (aub ? "YES" : "no") << "\n";
        std::cerr << "  GT: " << gt_str << "\n";

        // ── CoB analysis ──────────────────────────────
        auto &vars        = parse.value().vars;
        auto &reduced_sig = elim.reduced_sig;
        uint32_t nv       = static_cast< uint32_t >(elim.real_vars.size());

        auto coeffs   = InterpolateCoefficients(reduced_sig, nv, 64);
        auto cob_expr = BuildCobExpr(coeffs, nv, 64);

        // Boolean check
        bool bool_ok = true;
        for (uint32_t bits = 0; bits < (1U << nv); ++bits) {
            std::vector< uint64_t > bools(nv);
            for (uint32_t v = 0; v < nv; ++v) { bools[v] = (bits >> v) & 1; }
            std::vector< uint64_t > full_bools(vars.size(), 0);
            for (size_t i = 0; i < elim.real_vars.size(); ++i) {
                for (size_t j = 0; j < vars.size(); ++j) {
                    if (vars[j] == elim.real_vars[i]) {
                        full_bools[j] = bools[i];
                        break;
                    }
                }
            }
            uint64_t orig = EvalExpr(**folded_ptr, full_bools, 64);
            uint64_t cob  = EvalExpr(*cob_expr, bools, 64);
            if (orig != cob) {
                bool_ok = false;
                break;
            }
        }
        if (bool_ok) { total_cob_bool_ok++; }

        // Full-width check with adversarial inputs
        bool fw_ok                        = true;
        std::vector< uint64_t > fw_probes = {
            2, 3, 7, 0xFF, 0x5555555555555555ULL, 0xAAAAAAAAAAAAAAAAULL, 0x0123456789ABCDEFULL
        };
        for (auto probe : fw_probes) {
            std::vector< uint64_t > inputs(vars.size(), probe);
            uint64_t orig = EvalExpr(**folded_ptr, inputs, 64);
            std::vector< uint64_t > red_inputs(nv);
            for (size_t i = 0; i < elim.real_vars.size(); ++i) {
                for (size_t j = 0; j < vars.size(); ++j) {
                    if (vars[j] == elim.real_vars[i]) {
                        red_inputs[i] = inputs[j];
                        break;
                    }
                }
            }
            uint64_t cob = EvalExpr(*cob_expr, red_inputs, 64);
            if (orig != cob) {
                fw_ok = false;
                break;
            }
        }
        if (!fw_ok) { total_cob_fw_fail++; }

        // Count nonlinear CoB terms
        int linear_terms = 0, nonlinear_terms = 0;
        for (size_t i = 1; i < coeffs.size(); ++i) {
            if (coeffs[i] == 0) { continue; }
            if (std::popcount(static_cast< unsigned >(i)) == 1) {
                linear_terms++;
            } else {
                nonlinear_terms++;
            }
        }
        std::string cob_pattern;
        if (nonlinear_terms == 0) {
            cob_pattern = "pure-linear";
        } else if (nonlinear_terms <= 2) {
            cob_pattern = "few-nonlinear";
        } else {
            cob_pattern = "many-nonlinear";
        }
        cob_term_patterns[cob_pattern]++;

        std::cerr << "  CoB: bool=" << (bool_ok ? "OK" : "FAIL")
                  << " fw=" << (fw_ok ? "OK" : "FAIL") << " linear=" << linear_terms
                  << " nonlinear=" << nonlinear_terms << " (" << cob_pattern << ")\n";

        // Show CoB expression
        auto cob_rendered = Render(*cob_expr, elim.real_vars, 64);
        std::cerr << "  CoB expr: " << cob_rendered.substr(0, 100) << "\n";

        // ── Duplication analysis ──────────────────────
        auto dup        = compute_dup_info(**folded_ptr);
        float dup_ratio = dup.total_subtrees > 0
            ? 100.0F * static_cast< float >(dup.dup_subtrees)
                / static_cast< float >(dup.total_subtrees)
            : 0.0F;

        std::cerr << "  Dup: " << std::fixed << std::setprecision(0) << dup_ratio
                  << "% max_repeat=" << dup.max_repeat << " largest=" << dup.largest_dup
                  << "\n";

        // ── Lifting simulation ────────────────────────
        std::unordered_map< size_t, std::vector< size_t > > by_hash;
        std::vector< LiftEntry > entries;
        collect_repeats(**folded_ptr, vars, 64, by_hash, entries);

        std::vector< LiftEntry * > viable;
        for (auto &entry : entries) {
            if (entry.count >= 2 && entry.size >= 4) { viable.push_back(&entry); }
        }

        std::sort(viable.begin(), viable.end(), [](const auto *a, const auto *b) {
            auto benefit_a = static_cast< uint64_t >(a->count - 1) * a->size;
            auto benefit_b = static_cast< uint64_t >(b->count - 1) * b->size;
            if (benefit_a != benefit_b) { return benefit_a > benefit_b; }
            return a->size > b->size;
        });

        auto orig_vc    = static_cast< uint32_t >(vars.size());
        uint32_t budget = 16 > orig_vc ? 16 - orig_vc : 0;

        std::vector< const LiftEntry * > selected;
        for (const auto *cand : viable) {
            if (selected.size() >= budget) { break; }
            bool overlaps = false;
            for (const auto *sel : selected) {
                if (is_ancestor_of(sel->ptr, cand->ptr) || is_ancestor_of(cand->ptr, sel->ptr))
                {
                    overlaps = true;
                    break;
                }
            }
            if (!overlaps) { selected.push_back(cand); }
        }

        if (!viable.empty()) { total_lift_viable++; }
        if (!selected.empty()) { total_lift_selected++; }

        std::cerr << "  Lift: " << viable.size() << " viable, " << selected.size()
                  << " selected"
                  << " (budget=" << budget << ")\n";

        // Build outer expression if lifting applies
        std::string outer_sem_str = "n/a";
        if (!selected.empty()) {
            std::vector< std::pair< size_t, std::string > > targets;
            std::vector< uint32_t > virt_idx;
            for (size_t i = 0; i < selected.size(); ++i) {
                targets.emplace_back(selected[i]->hash, selected[i]->rendered);
                virt_idx.push_back(orig_vc + static_cast< uint32_t >(i));
            }

            auto outer       = replace_with_virtual(**folded_ptr, targets, virt_idx, vars, 64);
            auto outer_nodes = count_nodes(*outer);
            auto outer_cls   = ClassifyStructural(*outer);
            outer_sem_str    = semantic_str(outer_cls.semantic);
            outer_classes[outer_sem_str]++;

            std::vector< std::string > outer_vars = vars;
            for (size_t i = 0; i < selected.size(); ++i) {
                outer_vars.push_back("r" + std::to_string(i));
            }
            auto outer_rendered = Render(*outer, outer_vars, 64);

            std::cerr << "  Outer: " << outer_nodes << " nodes, " << outer_vars.size()
                      << " vars"
                      << " sem=" << outer_sem_str << " flags={" << flag_str(outer_cls.flags)
                      << "}"
                      << "\n";
            std::cerr << "  Outer expr: " << outer_rendered.substr(0, 120) << "\n";

            // Does the outer still have arith-under-bitwise?
            bool outer_aub = has_arith_under_bitwise(*outer, false);
            std::cerr << "  Outer aub=" << (outer_aub ? "YES" : "no") << "\n";

            // CoB on outer
            auto onv = static_cast< uint32_t >(outer_vars.size());
            if (onv <= 8) {
                auto outer_sig    = EvaluateBooleanSignature(*outer, onv, 64);
                auto outer_elim   = EliminateAuxVars(outer_sig, outer_vars);
                auto ornv         = static_cast< uint32_t >(outer_elim.real_vars.size());
                auto outer_coeffs = InterpolateCoefficients(outer_elim.reduced_sig, ornv, 64);
                int o_lin = 0, o_nl = 0;
                for (size_t i = 1; i < outer_coeffs.size(); ++i) {
                    if (outer_coeffs[i] == 0) { continue; }
                    if (std::popcount(static_cast< unsigned >(i)) == 1) {
                        o_lin++;
                    } else {
                        o_nl++;
                    }
                }
                std::cerr << "  Outer CoB: " << ornv << " real vars"
                          << " linear=" << o_lin << " nonlinear=" << o_nl << "\n";
            } else {
                std::cerr << "  Outer CoB: too many vars (" << onv << ")\n";
            }
        }

        // ── Full pipeline run ─────────────────────────
        Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
        opts.evaluator = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, 64);
        };

        auto result = Simplify(parse.value().sig, parse.value().vars, folded_ptr->get(), opts);
        if (!result.has_value()) { continue; }

        std::string term_reason = "unknown";
        if (result.value().kind == SimplifyOutcome::Kind::kSimplified) {
            term_reason = "SIMPLIFIED!";
            auto sr     = Render(*result.value().expr, result.value().real_vars, 64);
            std::cerr << "  Result: SIMPLIFIED → " << sr.substr(0, 80) << "\n";
        } else {
            if (result.value().diag.reason_code.has_value()) {
                term_reason = category_str(result.value().diag.reason_code->category);
            }
            std::cerr << "  Result: " << term_reason
                      << " exp=" << result.value().telemetry.total_expansions
                      << " verified=" << result.value().telemetry.candidates_verified << "\n";
            std::cerr << "  Reason: " << result.value().diag.reason << "\n";
            if (!result.value().diag.cause_chain.empty()) {
                std::cerr << "  Cause chain (" << result.value().diag.cause_chain.size()
                          << " frames):\n";
                for (size_t i = 0; i < result.value().diag.cause_chain.size() && i < 5; ++i) {
                    std::cerr << "    [" << i << "] "
                              << result.value().diag.cause_chain[i].message.substr(0, 100)
                              << "\n";
                }
            }
        }
        terminal_reasons[term_reason]++;
    }

    // ── Aggregate summary ─────────────────────────────
    std::cerr << "\n";
    std::cerr << "═══════════════════════════════════════════\n";
    std::cerr << "  AGGREGATE SUMMARY (19 Linear exhausted)\n";
    std::cerr << "═══════════════════════════════════════════\n";

    std::cerr << "\nArith-under-bitwise: " << total_arith_under_bw << " / 19\n";
    std::cerr << "CoB boolean-domain OK: " << total_cob_bool_ok << " / 19\n";
    std::cerr << "CoB full-width FAIL: " << total_cob_fw_fail << " / 19\n";
    std::cerr << "Lifting viable: " << total_lift_viable << " / 19\n";
    std::cerr << "Lifting selected: " << total_lift_selected << " / 19\n";

    std::cerr << "\nCoB term patterns:\n";
    for (auto &[k, v] : cob_term_patterns) { std::cerr << "  " << k << ": " << v << "\n"; }

    std::cerr << "\nPost-lift outer classifications:\n";
    for (auto &[k, v] : outer_classes) { std::cerr << "  " << k << ": " << v << "\n"; }

    std::cerr << "\nTerminal reasons:\n";
    for (auto &[k, v] : terminal_reasons) { std::cerr << "  " << k << ": " << v << "\n"; }

    std::cerr << "\n";
}
