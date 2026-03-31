// Re-baseline audit of all QSynth unsupported expressions after the
// competition-group handle-leak fix (766964a).  Captures:
//   - category (search-exhausted / verify-failed / guard-failed / rep-gap)
//   - semantic class (Linear / Semilinear / Polynomial / NonPoly)
//   - real-var count after aux-var elimination
//   - node count of folded AST
//   - telemetry (expansions, candidates_verified, queue_high_water)
//   - whether lifting appears in the cause chain
//   - terminal domain + message from cause chain
//   - arith-under-bitwise structural flag
//
// Outputs:
//   1. Category × SemanticClass cross-tab
//   2. Per-expression detail for verify-failed
//   3. Per-expression detail for guard-failed
//   4. Per-expression detail for search-exhausted
//   5. Top-20 target list (ranked by tractability heuristic)

#include "ExprParser.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureEval.h"
#include "cobra/core/Simplifier.h"
#include "cobra/core/SimplifyOutcome.h"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <iomanip>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace cobra;

namespace {

    // ── helpers ──────────────────────────────────────────────

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

    const char *semantic_str(SemanticClass s) {
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

    const char *category_str(ReasonCategory cat) {
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

    const char *domain_str(ReasonDomain d) {
        switch (d) {
            case ReasonDomain::kOrchestrator:
                return "Orchestrator";
            case ReasonDomain::kSemilinear:
                return "Semilinear";
            case ReasonDomain::kSignature:
                return "Signature";
            case ReasonDomain::kStructuralTransform:
                return "StructXform";
            case ReasonDomain::kDecomposition:
                return "Decomposition";
            case ReasonDomain::kTemplateDecomposer:
                return "TemplateDec";
            case ReasonDomain::kWeightedPolyFit:
                return "WeightedPoly";
            case ReasonDomain::kMultivarPoly:
                return "MultivarPoly";
            case ReasonDomain::kPolynomialRecovery:
                return "PolyRecovery";
            case ReasonDomain::kBitwiseDecomposer:
                return "BitwiseDec";
            case ReasonDomain::kHybridDecomposer:
                return "HybridDec";
            case ReasonDomain::kGhostResidual:
                return "GhostResidual";
            case ReasonDomain::kOperandSimplifier:
                return "OperandSimp";
            case ReasonDomain::kLifting:
                return "Lifting";
            case ReasonDomain::kVerifier:
                return "Verifier";
        }
        return "?";
    }

    uint32_t count_nodes(const Expr &e) {
        uint32_t n = 1;
        for (const auto &c : e.children) { n += count_nodes(*c); }
        return n;
    }

    // Scan cause chain for any mention of lifting
    bool cause_chain_mentions_lifting(const std::vector< ReasonFrame > &chain) {
        for (const auto &frame : chain) {
            if (frame.code.domain == ReasonDomain::kLifting) { return true; }
            if (frame.message.find("Lifted") != std::string::npos) { return true; }
            if (frame.message.find("lifted") != std::string::npos) { return true; }
            if (frame.message.find("lift") != std::string::npos) { return true; }
        }
        return false;
    }

    // Extract terminal (deepest) cause frame
    struct TerminalInfo
    {
        std::string domain;
        std::string category;
        std::string message;
        uint16_t subcode = 0;
    };

    TerminalInfo extract_terminal(const Diagnostic &diag) {
        TerminalInfo info;
        if (!diag.cause_chain.empty()) {
            const auto &last = diag.cause_chain.back();
            info.domain      = domain_str(last.code.domain);
            info.category    = category_str(last.code.category);
            info.subcode     = last.code.subcode;
            info.message     = last.message;
        } else if (diag.reason_code.has_value()) {
            info.domain   = domain_str(diag.reason_code->domain);
            info.category = category_str(diag.reason_code->category);
            info.subcode  = diag.reason_code->subcode;
            info.message  = diag.reason;
        }
        return info;
    }

    // ── per-expression record ───────────────────────────────

    struct UnsupportedRecord
    {
        int line_num                 = 0;
        SemanticClass semantic       = SemanticClass::kLinear;
        StructuralFlag flags         = kSfNone;
        ReasonCategory category      = ReasonCategory::kNone;
        ReasonDomain domain          = ReasonDomain::kOrchestrator;
        uint32_t real_vars           = 0;
        uint32_t nodes               = 0;
        uint32_t total_expansions    = 0;
        uint32_t candidates_verified = 0;
        uint32_t queue_high_water    = 0;
        bool lifting_in_chain        = false;
        bool arith_over_bitwise      = false;
        bool candidate_failed_verify = false;
        TerminalInfo terminal;
        std::string gt_str;
        std::string top_reason;
        size_t cause_chain_depth = 0;
    };

    // Tractability score: lower = more likely tractable
    // Heuristic: verify-failed with few vars and lifting involved
    // are the ripest targets
    int tractability_score(const UnsupportedRecord &r) {
        int score = 0;
        // Category weight
        switch (r.category) {
            case ReasonCategory::kVerifyFailed:
                score += 0;
                break; // best target
            case ReasonCategory::kGuardFailed:
                score += 100;
                break;
            case ReasonCategory::kRepresentationGap:
                score += 200;
                break;
            case ReasonCategory::kSearchExhausted:
                score += 300;
                break;
            default:
                score += 400;
                break;
        }
        // Fewer vars = easier
        score += static_cast< int >(r.real_vars) * 10;
        // Semantic class
        if (r.semantic == SemanticClass::kLinear) {
            score -= 50; // linear should be solvable
        } else if (r.semantic == SemanticClass::kSemilinear) {
            score -= 30;
        }
        // Lifting involved means pipeline was close
        if (r.lifting_in_chain) { score -= 20; }
        // Low expansion count means we gave up early (gate issue)
        if (r.total_expansions < 100) { score -= 10; }
        return score;
    }

} // namespace

TEST(RebaselineAudit, FullFrontierAnalysis) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    std::vector< UnsupportedRecord > records;
    int total_parsed     = 0;
    int total_simplified = 0;
    int total_unsup      = 0;

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

        auto parse = ParseAndEvaluate(obfuscated, 64);
        if (!parse.has_value()) { continue; }

        auto ast = ParseToAst(obfuscated, 64);
        if (!ast.has_value()) { continue; }

        auto folded_ptr = std::make_shared< std::unique_ptr< Expr > >(
            FoldConstantBitwise(std::move(ast.value().expr), 64)
        );

        ++total_parsed;

        const auto &sig  = parse.value().sig;
        const auto &vars = parse.value().vars;

        Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
        opts.evaluator = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, 64);
        };

        auto result = Simplify(sig, vars, folded_ptr->get(), opts);
        if (!result.has_value()) { continue; }

        if (result.value().kind == SimplifyOutcome::Kind::kSimplified) {
            ++total_simplified;
            continue;
        }

        ++total_unsup;
        const auto &out = result.value();

        auto cls  = ClassifyStructural(**folded_ptr);
        auto elim = EliminateAuxVars(sig, vars);

        UnsupportedRecord rec;
        rec.line_num  = line_num;
        rec.semantic  = cls.semantic;
        rec.flags     = cls.flags;
        rec.real_vars = static_cast< uint32_t >(elim.real_vars.size());
        rec.nodes     = count_nodes(**folded_ptr);
        rec.gt_str    = gt_str;

        if (out.diag.reason_code.has_value()) {
            rec.category = out.diag.reason_code->category;
            rec.domain   = out.diag.reason_code->domain;
        }

        rec.total_expansions    = out.telemetry.total_expansions;
        rec.candidates_verified = out.telemetry.candidates_verified;
        rec.queue_high_water    = out.telemetry.queue_high_water;

        rec.lifting_in_chain        = cause_chain_mentions_lifting(out.diag.cause_chain);
        rec.arith_over_bitwise      = HasFlag(cls.flags, kSfHasArithOverBitwise);
        rec.candidate_failed_verify = out.diag.candidate_failed_verification;
        rec.terminal                = extract_terminal(out.diag);
        rec.top_reason              = out.diag.reason;
        rec.cause_chain_depth       = out.diag.cause_chain.size();

        records.push_back(std::move(rec));
    }

    // ═══════════════════════════════════════════════════════════
    //  SECTION 1: Summary
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n";
    std::cerr << "═══════════════════════════════════════════════════════════\n";
    std::cerr << "  QSYNTH RE-BASELINE AUDIT (post handle-leak fix)\n";
    std::cerr << "  Parsed: " << total_parsed << "  Simplified: " << total_simplified
              << "  Unsupported: " << total_unsup << "\n";
    std::cerr << "═══════════════════════════════════════════════════════════\n";

    // ═══════════════════════════════════════════════════════════
    //  SECTION 2: Category × SemanticClass cross-tab
    // ═══════════════════════════════════════════════════════════

    std::map< std::string, std::map< std::string, int > > crosstab;
    std::map< std::string, int > cat_totals;
    std::map< std::string, int > sem_totals;

    for (const auto &r : records) {
        std::string cat = category_str(r.category);
        std::string sem = semantic_str(r.semantic);
        crosstab[cat][sem]++;
        cat_totals[cat]++;
        sem_totals[sem]++;
    }

    std::vector< std::string > sem_cols = { "Linear", "Semilinear", "Polynomial", "NonPoly" };

    std::cerr << "\n── Category × SemanticClass ──────────────────────────────\n";
    std::cerr << std::left << std::setw(22) << "Category";
    for (const auto &s : sem_cols) { std::cerr << std::setw(12) << s; }
    std::cerr << std::setw(8) << "TOTAL" << "\n";
    std::cerr << std::string(22 + 12 * 4 + 8, '-') << "\n";

    for (const auto &[cat, inner] : crosstab) {
        std::cerr << std::left << std::setw(22) << cat;
        for (const auto &s : sem_cols) {
            auto it = inner.find(s);
            int v   = (it != inner.end()) ? it->second : 0;
            std::cerr << std::setw(12) << v;
        }
        std::cerr << std::setw(8) << cat_totals[cat] << "\n";
    }
    std::cerr << std::string(22 + 12 * 4 + 8, '-') << "\n";
    std::cerr << std::left << std::setw(22) << "TOTAL";
    for (const auto &s : sem_cols) {
        auto it = sem_totals.find(s);
        int v   = (it != sem_totals.end()) ? it->second : 0;
        std::cerr << std::setw(12) << v;
    }
    std::cerr << std::setw(8) << total_unsup << "\n";

    // ═══════════════════════════════════════════════════════════
    //  SECTION 3: Lifting involvement by category
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n── Lifting involvement ──────────────────────────────────\n";
    std::map< std::string, int > lift_yes;
    std::map< std::string, int > lift_no;
    for (const auto &r : records) {
        std::string cat = category_str(r.category);
        if (r.lifting_in_chain) {
            lift_yes[cat]++;
        } else {
            lift_no[cat]++;
        }
    }
    std::cerr << std::left << std::setw(22) << "Category" << std::setw(12) << "LiftYes"
              << std::setw(12) << "LiftNo" << "\n";
    for (const auto &[cat, _] : cat_totals) {
        std::cerr << std::left << std::setw(22) << cat << std::setw(12) << lift_yes[cat]
                  << std::setw(12) << lift_no[cat] << "\n";
    }

    // ═══════════════════════════════════════════════════════════
    //  SECTION 4: Terminal domain distribution
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n── Terminal domain × category ───────────────────────────\n";
    std::map< std::string, int > terminal_dist;
    for (const auto &r : records) {
        std::string key = std::string(category_str(r.category)) + " → " + r.terminal.domain;
        if (!r.terminal.message.empty()) {
            auto msg  = r.terminal.message.substr(0, 50);
            key      += " [" + msg + "]";
        }
        terminal_dist[key]++;
    }
    for (const auto &[k, v] : terminal_dist) {
        std::cerr << "  " << std::setw(4) << v << "× " << k << "\n";
    }

    // ═══════════════════════════════════════════════════════════
    //  SECTION 5: Detailed per-expression: verify-failed
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n── VERIFY-FAILED detail ─────────────────────────────────\n";
    for (const auto &r : records) {
        if (r.category != ReasonCategory::kVerifyFailed) { continue; }
        std::cerr << "  L" << r.line_num << " sem=" << semantic_str(r.semantic)
                  << " vars=" << r.real_vars << " nodes=" << r.nodes
                  << " exp=" << r.total_expansions << " cands=" << r.candidates_verified
                  << " lift=" << (r.lifting_in_chain ? "YES" : "no")
                  << " AoB=" << (r.arith_over_bitwise ? "Y" : "n") << " flags={"
                  << flag_str(r.flags) << "}"
                  << "\n";
        std::cerr << "    terminal: " << r.terminal.domain << "/" << r.terminal.category
                  << " sub=" << r.terminal.subcode << "\n";
        std::cerr << "    reason: " << r.top_reason.substr(0, 100) << "\n";
        std::cerr << "    GT: " << r.gt_str.substr(0, 80) << "\n";
    }

    // ═══════════════════════════════════════════════════════════
    //  SECTION 6: Detailed per-expression: guard-failed
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n── GUARD-FAILED detail ──────────────────────────────────\n";
    for (const auto &r : records) {
        if (r.category != ReasonCategory::kGuardFailed) { continue; }
        std::cerr << "  L" << r.line_num << " sem=" << semantic_str(r.semantic)
                  << " vars=" << r.real_vars << " nodes=" << r.nodes
                  << " exp=" << r.total_expansions << " cands=" << r.candidates_verified
                  << " lift=" << (r.lifting_in_chain ? "YES" : "no")
                  << " AoB=" << (r.arith_over_bitwise ? "Y" : "n") << " flags={"
                  << flag_str(r.flags) << "}"
                  << "\n";
        std::cerr << "    terminal: " << r.terminal.domain << "/" << r.terminal.category
                  << " sub=" << r.terminal.subcode << "\n";
        std::cerr << "    reason: " << r.top_reason.substr(0, 100) << "\n";
        std::cerr << "    GT: " << r.gt_str.substr(0, 80) << "\n";
    }

    // ═══════════════════════════════════════════════════════════
    //  SECTION 7: Search-exhausted by semantic class + vars
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n── SEARCH-EXHAUSTED detail ──────────────────────────────\n";
    for (const auto &r : records) {
        if (r.category != ReasonCategory::kSearchExhausted) { continue; }
        std::cerr << "  L" << r.line_num << " sem=" << semantic_str(r.semantic)
                  << " vars=" << r.real_vars << " nodes=" << r.nodes
                  << " exp=" << r.total_expansions << " cands=" << r.candidates_verified
                  << " qhw=" << r.queue_high_water
                  << " lift=" << (r.lifting_in_chain ? "YES" : "no")
                  << " AoB=" << (r.arith_over_bitwise ? "Y" : "n") << " flags={"
                  << flag_str(r.flags) << "}"
                  << "\n";
        std::cerr << "    terminal: " << r.terminal.domain << "/" << r.terminal.category
                  << " sub=" << r.terminal.subcode << "\n";
        std::cerr << "    GT: " << r.gt_str.substr(0, 80) << "\n";
    }

    // ═══════════════════════════════════════════════════════════
    //  SECTION 8: Representation-gap detail
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n── REPRESENTATION-GAP detail ────────────────────────────\n";
    for (const auto &r : records) {
        if (r.category != ReasonCategory::kRepresentationGap) { continue; }
        std::cerr << "  L" << r.line_num << " sem=" << semantic_str(r.semantic)
                  << " vars=" << r.real_vars << " nodes=" << r.nodes
                  << " exp=" << r.total_expansions
                  << " lift=" << (r.lifting_in_chain ? "YES" : "no") << " flags={"
                  << flag_str(r.flags) << "}"
                  << "\n";
        std::cerr << "    terminal: " << r.terminal.domain << "/" << r.terminal.category
                  << " sub=" << r.terminal.subcode << "\n";
        std::cerr << "    reason: " << r.top_reason.substr(0, 100) << "\n";
        std::cerr << "    GT: " << r.gt_str.substr(0, 80) << "\n";
    }

    // ═══════════════════════════════════════════════════════════
    //  SECTION 9: Expansion budget analysis
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n── Expansion budget analysis ────────────────────────────\n";
    std::map< std::string, std::vector< uint32_t > > exp_by_cat;
    for (const auto &r : records) {
        exp_by_cat[category_str(r.category)].push_back(r.total_expansions);
    }
    for (auto &[cat, vals] : exp_by_cat) {
        std::sort(vals.begin(), vals.end());
        uint32_t sum = 0;
        for (auto v : vals) { sum += v; }
        std::cerr << "  " << cat << ": n=" << vals.size() << " min=" << vals.front()
                  << " med=" << vals[vals.size() / 2] << " max=" << vals.back()
                  << " avg=" << (sum / vals.size()) << " >1024="
                  << std::count_if(
                         vals.begin(), vals.end(), [](uint32_t v) { return v > 1024; }
                     )
                  << "\n";
    }

    // ═══════════════════════════════════════════════════════════
    //  SECTION 10: Top-20 targets (ranked by tractability)
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n── TOP-20 TARGETS (lower score = more tractable) ───────\n";
    std::vector< std::pair< int, const UnsupportedRecord * > > ranked;
    for (const auto &r : records) { ranked.emplace_back(tractability_score(r), &r); }
    std::sort(ranked.begin(), ranked.end());

    int shown = 0;
    for (const auto &[score, rp] : ranked) {
        if (shown >= 20) { break; }
        const auto &r = *rp;
        std::cerr << "  #" << std::setw(2) << (shown + 1) << " score=" << std::setw(4) << score
                  << " L" << std::setw(3) << r.line_num << " " << std::setw(16)
                  << category_str(r.category) << " " << std::setw(10)
                  << semantic_str(r.semantic) << " vars=" << r.real_vars
                  << " exp=" << std::setw(4) << r.total_expansions
                  << " lift=" << (r.lifting_in_chain ? "Y" : "n")
                  << " AoB=" << (r.arith_over_bitwise ? "Y" : "n") << "\n";
        std::cerr << "       GT: " << r.gt_str.substr(0, 70) << "\n";
        ++shown;
    }

    // ═══════════════════════════════════════════════════════════
    //  SECTION 11: Line number inventory
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n── Line number inventory by category ───────────────────\n";
    std::map< std::string, std::vector< int > > lines_by_cat;
    for (const auto &r : records) {
        lines_by_cat[category_str(r.category)].push_back(r.line_num);
    }
    for (const auto &[cat, lines_vec] : lines_by_cat) {
        std::cerr << "  " << cat << ": {";
        for (size_t i = 0; i < lines_vec.size(); ++i) {
            if (i > 0) { std::cerr << ", "; }
            std::cerr << lines_vec[i];
        }
        std::cerr << "}\n";
    }

    std::cerr << "\n";

    // Sanity check: totals match what benchmarks expect.
    // All remaining unsupported are mixed-domain representation gaps
    // (AoB + BoA flags, CoB boolean-correct but FW-incorrect).
    EXPECT_EQ(total_simplified, 447);
    EXPECT_EQ(total_unsup, 53);
}
