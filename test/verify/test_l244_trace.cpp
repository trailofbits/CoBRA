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
#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <iomanip>
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
        auto start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) { return ""; }
        auto end = s.find_last_not_of(" \t\r\n");
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
            bool found    = false;
            auto hit      = by_hash.find(h);
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

    void report_outcome(const std::string &label, const SimplifyOutcome &out) {
        if (out.kind == SimplifyOutcome::Kind::kSimplified) {
            auto sr = Render(*out.expr, out.real_vars, 64);
            std::cerr << "  " << label << ": SIMPLIFIED → " << sr << "\n";
            std::cerr << "    verified=" << out.verified
                      << " exp=" << out.telemetry.total_expansions
                      << " verified_cands=" << out.telemetry.candidates_verified << "\n";
        } else {
            std::string cat = "unknown";
            if (out.diag.reason_code.has_value()) {
                cat = category_str(out.diag.reason_code->category);
            }
            std::cerr << "  " << label << ": " << cat
                      << " exp=" << out.telemetry.total_expansions
                      << " verified_cands=" << out.telemetry.candidates_verified << "\n";
            std::cerr << "    reason: " << out.diag.reason << "\n";
            if (!out.diag.cause_chain.empty()) {
                for (size_t i = 0; i < out.diag.cause_chain.size() && i < 5; ++i) {
                    std::cerr << "    cause[" << i
                              << "]: " << out.diag.cause_chain[i].message.substr(0, 100)
                              << "\n";
                }
            }
        }
    }

} // namespace

// ═══════════════════════════════════════════════════════════
// Test 1: Standalone simplify on L244's lifted outer
// ═══════════════════════════════════════════════════════════

TEST(L244Trace, StandaloneOuter) {
    // Read L244 from dataset
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());
    std::string line;
    int line_num = 0;
    while (std::getline(file, line)) {
        ++line_num;
        if (line_num != 244) { continue; }
        break;
    }

    size_t sep         = find_separator(line);
    std::string obf    = trim(line.substr(0, sep));
    std::string gt_str = trim(line.substr(sep + 1));

    auto parse = ParseAndEvaluate(obf, 64);
    ASSERT_TRUE(parse.has_value());
    auto ast = ParseToAst(obf, 64);
    ASSERT_TRUE(ast.has_value());

    auto folded      = FoldConstantBitwise(std::move(ast.value().expr), 64);
    const auto &vars = parse.value().vars;
    auto orig_vc     = static_cast< uint32_t >(vars.size());

    std::cerr << "\n═══════════════════════════════════════\n";
    std::cerr << "L244 TRACE\n";
    std::cerr << "═══════════════════════════════════════\n";
    std::cerr << "GT: " << gt_str << "\n";
    std::cerr << "Vars (" << orig_vc << "): [";
    for (size_t i = 0; i < vars.size(); ++i) {
        if (i > 0) { std::cerr << ", "; }
        std::cerr << vars[i];
    }
    std::cerr << "]\n";
    std::cerr << "Nodes: " << count_nodes(*folded) << "\n";

    // ── Build lifted outer ────────────────────────────
    std::unordered_map< size_t, std::vector< size_t > > by_hash;
    std::vector< LiftEntry > entries;
    collect_repeats(*folded, vars, 64, by_hash, entries);

    std::vector< LiftEntry * > viable;
    for (auto &e : entries) {
        if (e.count >= 2 && e.size >= 4) { viable.push_back(&e); }
    }
    std::sort(viable.begin(), viable.end(), [](const auto *a, const auto *b) {
        auto benefit_a = static_cast< uint64_t >(a->count - 1) * a->size;
        auto benefit_b = static_cast< uint64_t >(b->count - 1) * b->size;
        if (benefit_a != benefit_b) { return benefit_a > benefit_b; }
        return a->size > b->size;
    });

    uint32_t budget = 16 > orig_vc ? 16 - orig_vc : 0;
    std::vector< const LiftEntry * > selected;
    for (const auto *cand : viable) {
        if (selected.size() >= budget) { break; }
        bool overlaps = false;
        for (const auto *sel : selected) {
            if (is_ancestor_of(sel->ptr, cand->ptr) || is_ancestor_of(cand->ptr, sel->ptr)) {
                overlaps = true;
                break;
            }
        }
        if (!overlaps) { selected.push_back(cand); }
    }

    std::cerr << "Lifting: " << selected.size() << " selected\n";

    std::vector< std::pair< size_t, std::string > > targets;
    std::vector< uint32_t > virt_idx;
    for (size_t i = 0; i < selected.size(); ++i) {
        targets.emplace_back(selected[i]->hash, selected[i]->rendered);
        virt_idx.push_back(orig_vc + static_cast< uint32_t >(i));
        std::cerr << "  r" << i << ": " << selected[i]->count << "x " << selected[i]->size
                  << "nodes → " << selected[i]->rendered.substr(0, 60) << "\n";
    }

    auto outer = replace_with_virtual(*folded, targets, virt_idx, vars, 64);
    std::vector< std::string > outer_vars = vars;
    for (size_t i = 0; i < selected.size(); ++i) {
        outer_vars.push_back("r" + std::to_string(i));
    }

    auto outer_rendered = Render(*outer, outer_vars, 64);
    auto outer_cls      = ClassifyStructural(*outer);
    auto onv            = static_cast< uint32_t >(outer_vars.size());

    std::cerr << "\nOuter: " << count_nodes(*outer) << " nodes, " << onv << " vars\n";
    std::cerr << "Outer expr: " << outer_rendered << "\n";
    std::cerr << "Outer cls: sem=" << semantic_str(outer_cls.semantic) << " flags={"
              << flag_str(outer_cls.flags) << "}\n";
    std::cerr << "Outer vars: [";
    for (size_t i = 0; i < outer_vars.size(); ++i) {
        if (i > 0) { std::cerr << ", "; }
        std::cerr << outer_vars[i];
    }
    std::cerr << "]\n";

    // ── Aux var elimination on outer ──────────────────
    auto outer_sig  = EvaluateBooleanSignature(*outer, onv, 64);
    auto outer_elim = EliminateAuxVars(outer_sig, outer_vars);
    auto ornv       = static_cast< uint32_t >(outer_elim.real_vars.size());

    std::cerr << "Outer real vars (" << ornv << "): [";
    for (size_t i = 0; i < outer_elim.real_vars.size(); ++i) {
        if (i > 0) { std::cerr << ", "; }
        std::cerr << outer_elim.real_vars[i];
    }
    std::cerr << "]\n";

    // ── CoB on outer ─────────────────────────────────
    auto outer_coeffs = InterpolateCoefficients(outer_elim.reduced_sig, ornv, 64);
    auto outer_cob    = BuildCobExpr(outer_coeffs, ornv, 64);
    auto cob_rendered = Render(*outer_cob, outer_elim.real_vars, 64);
    std::cerr << "Outer CoB: " << cob_rendered << "\n";

    // Full-width check of outer CoB vs outer expr
    bool fw_ok                        = true;
    std::vector< uint64_t > fw_probes = {
        2, 3, 7, 42, 0xFF, 0x5555555555555555ULL, 0xAAAAAAAAAAAAAAAAULL, 0x0123456789ABCDEFULL
    };
    for (auto probe : fw_probes) {
        // Build full outer_vars input
        std::vector< uint64_t > full_input(onv, probe);
        uint64_t outer_val = EvalExpr(*outer, full_input, 64);
        // Build reduced input for CoB
        std::vector< uint64_t > red_input(ornv);
        for (size_t i = 0; i < outer_elim.real_vars.size(); ++i) {
            for (size_t j = 0; j < outer_vars.size(); ++j) {
                if (outer_vars[j] == outer_elim.real_vars[i]) {
                    red_input[i] = full_input[j];
                    break;
                }
            }
        }
        uint64_t cob_val = EvalExpr(*outer_cob, red_input, 64);
        if (outer_val != cob_val) {
            std::cerr << "  FW MISMATCH at probe=" << probe << " outer=0x" << std::hex
                      << outer_val << " cob=0x" << cob_val << std::dec << "\n";
            fw_ok = false;
        }
    }
    std::cerr << "Outer CoB fw: " << (fw_ok ? "OK" : "FAIL") << "\n";

    // ── Path A: Standalone Simplify on outer ──────────
    std::cerr << "\n── Path A: Standalone Simplify ──\n";
    auto outer_folded = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*outer));
    Options outer_opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    outer_opts.evaluator = [outer_folded](const std::vector< uint64_t > &v) -> uint64_t {
        return EvalExpr(**outer_folded, v, 64);
    };
    auto standalone = Simplify(outer_sig, outer_vars, outer_folded->get(), outer_opts);
    ASSERT_TRUE(standalone.has_value());
    report_outcome("Standalone", standalone.value());

    // ── Path B: Full pipeline on original ─────────────
    std::cerr << "\n── Path B: Full pipeline on L244 ──\n";
    auto orig_folded = std::make_shared< std::unique_ptr< Expr > >(
        FoldConstantBitwise(std::move(ParseToAst(obf, 64).value().expr), 64)
    );
    Options full_opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    full_opts.evaluator = [orig_folded](const std::vector< uint64_t > &v) -> uint64_t {
        return EvalExpr(**orig_folded, v, 64);
    };
    auto full_result =
        Simplify(parse.value().sig, parse.value().vars, orig_folded->get(), full_opts);
    ASSERT_TRUE(full_result.has_value());
    report_outcome("Full pipeline", full_result.value());

    // ── Path C: Standalone without evaluator ──────────
    std::cerr << "\n── Path C: Standalone outer (no evaluator) ──\n";
    auto outer_folded2 = std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*outer));
    Options no_eval_opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
    auto no_eval = Simplify(outer_sig, outer_vars, outer_folded2->get(), no_eval_opts);
    ASSERT_TRUE(no_eval.has_value());
    report_outcome("No-eval standalone", no_eval.value());

    std::cerr << "\n";
}

// ═══════════════════════════════════════════════════════════
// Test 2: Back-substitution FW check
// Replicate exactly what ResolveLiftedSubstitute does
// ═══════════════════════════════════════════════════════════

TEST(L244Trace, BackSubstitutionFwCheck) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());
    std::string line;
    int line_num = 0;
    while (std::getline(file, line)) {
        ++line_num;
        if (line_num != 244) { continue; }
        break;
    }

    size_t sep      = find_separator(line);
    std::string obf = trim(line.substr(0, sep));

    auto parse = ParseAndEvaluate(obf, 64);
    ASSERT_TRUE(parse.has_value());
    auto ast = ParseToAst(obf, 64);
    ASSERT_TRUE(ast.has_value());

    auto folded      = FoldConstantBitwise(std::move(ast.value().expr), 64);
    const auto &vars = parse.value().vars;
    auto orig_vc     = static_cast< uint32_t >(vars.size());

    std::cerr << "\n═══════════════════════════════════════\n";
    std::cerr << "L244 BACK-SUBSTITUTION FW CHECK\n";
    std::cerr << "═══════════════════════════════════════\n";

    // Build lifted outer (same as before)
    std::unordered_map< size_t, std::vector< size_t > > by_hash;
    std::vector< LiftEntry > entries;
    collect_repeats(*folded, vars, 64, by_hash, entries);

    std::vector< LiftEntry * > viable;
    for (auto &e : entries) {
        if (e.count >= 2 && e.size >= 4) { viable.push_back(&e); }
    }
    std::sort(viable.begin(), viable.end(), [](const auto *a, const auto *b) {
        auto benefit_a = static_cast< uint64_t >(a->count - 1) * a->size;
        auto benefit_b = static_cast< uint64_t >(b->count - 1) * b->size;
        if (benefit_a != benefit_b) { return benefit_a > benefit_b; }
        return a->size > b->size;
    });

    uint32_t budget = 16 > orig_vc ? 16 - orig_vc : 0;
    std::vector< const LiftEntry * > selected;
    for (const auto *cand : viable) {
        if (selected.size() >= budget) { break; }
        bool overlaps = false;
        for (const auto *sel : selected) {
            if (is_ancestor_of(sel->ptr, cand->ptr) || is_ancestor_of(cand->ptr, sel->ptr)) {
                overlaps = true;
                break;
            }
        }
        if (!overlaps) { selected.push_back(cand); }
    }

    // Build targets and virtual indices
    std::vector< std::pair< size_t, std::string > > targets;
    std::vector< uint32_t > virt_idx;
    for (size_t i = 0; i < selected.size(); ++i) {
        targets.emplace_back(selected[i]->hash, selected[i]->rendered);
        virt_idx.push_back(orig_vc + static_cast< uint32_t >(i));
    }

    auto outer = replace_with_virtual(*folded, targets, virt_idx, vars, 64);
    std::vector< std::string > outer_vars = vars;
    for (size_t i = 0; i < selected.size(); ++i) {
        outer_vars.push_back("r" + std::to_string(i));
    }

    // Step 1: CoB on outer
    auto onv          = static_cast< uint32_t >(outer_vars.size());
    auto outer_sig    = EvaluateBooleanSignature(*outer, onv, 64);
    auto outer_elim   = EliminateAuxVars(outer_sig, outer_vars);
    auto ornv         = static_cast< uint32_t >(outer_elim.real_vars.size());
    auto outer_coeffs = InterpolateCoefficients(outer_elim.reduced_sig, ornv, 64);
    auto cob_expr     = BuildCobExpr(outer_coeffs, ornv, 64);
    auto cob_rendered = Render(*cob_expr, outer_elim.real_vars, 64);

    std::cerr << "Outer CoB: " << cob_rendered << " (" << ornv << " real vars)\n";
    std::cerr << "Outer real vars: [";
    for (size_t i = 0; i < outer_elim.real_vars.size(); ++i) {
        if (i > 0) { std::cerr << ", "; }
        std::cerr << outer_elim.real_vars[i];
    }
    std::cerr << "]\n";

    // Step 2: Remap from reduced to full outer space
    auto remapped = CloneExpr(*cob_expr);
    if (outer_elim.real_vars.size() < outer_vars.size()) {
        auto remap = BuildVarSupport(outer_vars, outer_elim.real_vars);
        std::cerr << "Remap indices: [";
        for (size_t i = 0; i < remap.size(); ++i) {
            if (i > 0) { std::cerr << ", "; }
            std::cerr << remap[i];
        }
        std::cerr << "]\n";
        RemapVarIndices(*remapped, remap);
    }
    auto remapped_r = Render(*remapped, outer_vars, 64);
    std::cerr << "Remapped: " << remapped_r << "\n";

    // Step 3: Substitute bindings (virtual vars → subtrees)
    // Manual substitution: replace Variable(virt_idx[i]) with
    // the original subtree
    std::function< std::unique_ptr< Expr >(const Expr &) > substitute =
        [&](const Expr &node) -> std::unique_ptr< Expr > {
        if (node.kind == Expr::Kind::kVariable) {
            for (size_t i = 0; i < selected.size(); ++i) {
                if (node.var_index == virt_idx[i]) { return CloneExpr(*selected[i]->ptr); }
            }
        }
        auto r          = std::make_unique< Expr >();
        r->kind         = node.kind;
        r->constant_val = node.constant_val;
        r->var_index    = node.var_index;
        for (const auto &c : node.children) { r->children.push_back(substitute(*c)); }
        return r;
    };
    auto substituted   = substitute(*remapped);
    auto substituted_r = Render(*substituted, vars, 64);
    std::cerr << "Substituted: " << substituted_r.substr(0, 120) << "\n";
    std::cerr << "Substituted nodes: " << count_nodes(*substituted) << "\n";

    // Step 4: Full-width check
    std::cerr << "\n── Full-width verification ──\n";
    std::vector< uint64_t > probes = {
        0,
        1,
        2,
        3,
        7,
        42,
        0xFF,
        0x5555555555555555ULL,
        0xAAAAAAAAAAAAAAAAULL,
        0x0123456789ABCDEFULL,
        0xFEDCBA9876543210ULL,
        0x8000000000000000ULL,
    };
    int mismatches = 0;
    for (auto probe : probes) {
        std::vector< uint64_t > input(orig_vc, probe);
        uint64_t orig_val = EvalExpr(*folded, input, 64);
        uint64_t sub_val  = EvalExpr(*substituted, input, 64);
        if (orig_val != sub_val) {
            std::cerr << "  MISMATCH at " << probe << ": orig=0x" << std::hex << orig_val
                      << " sub=0x" << sub_val << std::dec << "\n";
            mismatches++;
        }
    }
    // Also test with different values per variable
    for (int trial = 0; trial < 20; ++trial) {
        std::vector< uint64_t > input(orig_vc);
        for (uint32_t v = 0; v < orig_vc; ++v) {
            input[v] = probes[trial % probes.size()]
                ^ (static_cast< uint64_t >(v + 1) * 0xDEADBEEFULL);
        }
        uint64_t orig_val = EvalExpr(*folded, input, 64);
        uint64_t sub_val  = EvalExpr(*substituted, input, 64);
        if (orig_val != sub_val) {
            std::cerr << "  MISMATCH at trial " << trial << ": orig=0x" << std::hex << orig_val
                      << " sub=0x" << sub_val << std::dec << "\n";
            mismatches++;
        }
    }
    std::cerr << "FW check: " << mismatches << " mismatches out of 32 probes\n";

    std::cerr << "\n";
}
