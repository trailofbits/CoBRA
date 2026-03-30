#include "ExprParser.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/Simplifier.h"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <iomanip>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

using namespace cobra;

namespace {

    // ── Dataset helpers ─────────────────────────────────────────

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

    // ── Replicated lifting analysis (no orchestrator dependency) ──

    // These mirror the logic in LiftingPasses.cpp to analyze what
    // *would* happen without needing to construct WorkItem/OrchestratorContext.

    bool IsBitwiseKind(Expr::Kind k) {
        return k == Expr::Kind::kAnd || k == Expr::Kind::kOr || k == Expr::Kind::kXor
            || k == Expr::Kind::kNot;
    }

    bool IsPureArithmetic(const Expr &e) {
        switch (e.kind) {
            case Expr::Kind::kConstant:
            case Expr::Kind::kVariable:
                return true;
            case Expr::Kind::kAdd:
            case Expr::Kind::kMul:
            case Expr::Kind::kNeg:
                break;
            default:
                return false;
        }
        for (const auto &c : e.children) {
            if (!IsPureArithmetic(*c)) { return false; }
        }
        return true;
    }

    uint32_t CountNodes(const Expr &e) {
        uint32_t count = 1;
        for (const auto &c : e.children) { count += CountNodes(*c); }
        return count;
    }

    // ── Analysis of RunLiftArithmeticAtoms behavior ─────────────

    struct ArithAtomInfo
    {
        uint32_t total_candidates = 0;
        uint32_t unique_atoms     = 0;
        uint32_t would_add_vars   = 0;
    };

    void CollectArithAtoms(
        const Expr &node, bool parent_is_bitwise, const std::vector< std::string > &vars,
        uint32_t bitwidth, std::vector< std::pair< size_t, std::string > > &out
    ) {
        if (parent_is_bitwise && IsPureArithmetic(node) && HasVarDep(node)
            && node.kind != Expr::Kind::kVariable)
        {
            size_t h      = std::hash< Expr >{}(node);
            auto rendered = Render(node, vars, bitwidth);
            out.emplace_back(h, std::move(rendered));
            return;
        }

        bool current_is_bitwise = IsBitwiseKind(node.kind);
        for (const auto &child : node.children) {
            CollectArithAtoms(*child, current_is_bitwise, vars, bitwidth, out);
        }
    }

    ArithAtomInfo AnalyzeArithLifting(
        const Expr &expr, const std::vector< std::string > &vars, uint32_t bitwidth
    ) {
        std::vector< std::pair< size_t, std::string > > candidates;
        bool root_is_bitwise = IsBitwiseKind(expr.kind);
        for (const auto &child : expr.children) {
            CollectArithAtoms(*child, root_is_bitwise, vars, bitwidth, candidates);
        }

        // Deduplicate by hash + rendered string
        std::unordered_map< size_t, std::vector< std::string > > by_hash;
        uint32_t unique_count = 0;
        for (const auto &[h, rendered] : candidates) {
            auto it    = by_hash.find(h);
            bool found = false;
            if (it != by_hash.end()) {
                for (const auto &existing : it->second) {
                    if (existing == rendered) {
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                by_hash[h].push_back(rendered);
                unique_count++;
            }
        }

        return {
            .total_candidates = static_cast< uint32_t >(candidates.size()),
            .unique_atoms     = unique_count,
            .would_add_vars   = unique_count,
        };
    }

    // ── Analysis of RunLiftRepeatedSubexpressions behavior ──────

    struct RepeatInfo
    {
        size_t hash;
        std::string rendered;
        uint32_t count;
        uint32_t size;
    };

    struct RepeatedSubexprAnalysis
    {
        uint32_t total_nonleaf_subtrees = 0;
        uint32_t distinct_subtrees      = 0;
        uint32_t viable_before_filter   = 0; // count>=2 && size>=4
        uint32_t selected_count         = 0; // after greedy non-overlap
        uint32_t would_add_vars         = 0;
        uint32_t projected_total_vars   = 0;
        bool blocked_by_max_vars        = false;
        uint32_t max_repeat_count       = 0;
        uint32_t largest_viable_size    = 0;
        uint64_t total_savings          = 0;  // sum of (count-1)*size for selected
        std::vector< RepeatInfo > top_viable; // top 5 viable candidates
    };

    void CollectSubtrees(
        const Expr &node, const std::vector< std::string > &vars, uint32_t bitwidth,
        std::unordered_map< size_t, std::vector< size_t > > &by_hash,
        std::vector< RepeatInfo > &entries
    ) {
        bool is_leaf =
            (node.kind == Expr::Kind::kConstant || node.kind == Expr::Kind::kVariable);
        if (!is_leaf) {
            size_t h       = std::hash< Expr >{}(node);
            auto rendered  = Render(node, vars, bitwidth);
            auto node_size = CountNodes(node);

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
                    RepeatInfo{
                        .hash     = h,
                        .rendered = std::move(rendered),
                        .count    = 1,
                        .size     = node_size,
                    }
                );
            }
        }
        for (const auto &child : node.children) {
            CollectSubtrees(*child, vars, bitwidth, by_hash, entries);
        }
    }

    static constexpr uint32_t kMinRepeatSize = 4;

    RepeatedSubexprAnalysis AnalyzeRepeatedLifting(
        const Expr &expr, const std::vector< std::string > &vars, uint32_t bitwidth,
        uint32_t max_vars
    ) {
        uint32_t total_nodes = CountNodes(expr);

        // Guard: same as LiftingPasses.cpp
        if (total_nodes > 50'000) { return { .blocked_by_max_vars = false }; }

        std::unordered_map< size_t, std::vector< size_t > > by_hash;
        std::vector< RepeatInfo > entries;
        CollectSubtrees(expr, vars, bitwidth, by_hash, entries);

        RepeatedSubexprAnalysis result;
        result.total_nonleaf_subtrees = static_cast< uint32_t >(entries.size());
        result.distinct_subtrees      = static_cast< uint32_t >(entries.size());

        // Filter: count >= 2 && size >= 4
        std::vector< RepeatInfo * > viable;
        for (auto &entry : entries) {
            if (entry.count >= 2 && entry.size >= kMinRepeatSize) { viable.push_back(&entry); }
        }
        result.viable_before_filter = static_cast< uint32_t >(viable.size());

        if (viable.empty()) { return result; }

        // Sort: higher count first, larger size second
        std::sort(viable.begin(), viable.end(), [](const auto *a, const auto *b) {
            if (a->count != b->count) { return a->count > b->count; }
            return a->size > b->size;
        });

        // Track top 5
        for (size_t i = 0; i < viable.size() && i < 5; ++i) {
            result.top_viable.push_back(*viable[i]);
        }

        // Greedy maximal non-overlapping — simplified version.
        // We can't do the exact IsAncestorOf check without the tree structure,
        // but we can compute a useful upper bound on selected count.
        // Use the full count as upper bound since overlap removal only reduces.
        result.selected_count = static_cast< uint32_t >(viable.size());

        // Calculate stats
        result.would_add_vars = result.selected_count;
        result.projected_total_vars =
            static_cast< uint32_t >(vars.size()) + result.would_add_vars;
        result.blocked_by_max_vars = result.projected_total_vars > max_vars;

        for (const auto *v : viable) {
            if (v->count > result.max_repeat_count) { result.max_repeat_count = v->count; }
            if (v->size > result.largest_viable_size) { result.largest_viable_size = v->size; }
            result.total_savings += static_cast< uint64_t >(v->count - 1) * v->size;
        }

        return result;
    }

    // ── Combined per-expression record ──────────────────────────

    struct LiftingRecord
    {
        int line_num;
        std::string ground_truth;
        Classification cls;
        uint32_t real_vars;
        uint32_t tree_size;

        // Orchestrator telemetry
        uint32_t total_expansions;
        uint32_t max_depth;
        uint32_t candidates_verified;
        uint32_t queue_high_water;
        std::string terminal_reason;

        // Lifting analysis
        ArithAtomInfo arith_lift;
        RepeatedSubexprAnalysis repeat_lift;
    };

} // namespace

TEST(LiftingDiagnostic, InstrumentSearchExhausted) {
    std::ifstream file(DATASET_DIR "/gamba/qsynth_ea.txt");
    ASSERT_TRUE(file.is_open());

    std::vector< LiftingRecord > records;
    int parsed = 0, simplified = 0, unsupported = 0, exhausted = 0;

    std::string line;
    int line_num = 0;
    while (std::getline(file, line)) {
        ++line_num;
        if (line.empty() || line[0] == '#') { continue; }

        size_t sep = find_separator(line);
        if (sep == std::string::npos) { continue; }

        std::string obfuscated = trim(line.substr(0, sep));
        if (obfuscated.empty()) { continue; }

        auto parse_result = ParseAndEvaluate(obfuscated, 64);
        if (!parse_result.has_value()) { continue; }

        auto ast_result = ParseToAst(obfuscated, 64);
        if (!ast_result.has_value()) { continue; }

        parsed++;

        auto folded_ptr = std::make_shared< std::unique_ptr< Expr > >(
            FoldConstantBitwise(std::move(ast_result.value().expr), 64)
        );

        Options opts{ .bitwidth = 64, .max_vars = 16, .spot_check = true };
        opts.evaluator = [folded_ptr](const std::vector< uint64_t > &v) -> uint64_t {
            return EvalExpr(**folded_ptr, v, 64);
        };

        auto result = Simplify(
            parse_result.value().sig, parse_result.value().vars, folded_ptr->get(), opts
        );
        if (!result.has_value()) { continue; }

        if (result.value().kind == SimplifyOutcome::Kind::kSimplified) {
            simplified++;
            continue;
        }
        if (result.value().kind != SimplifyOutcome::Kind::kUnchangedUnsupported) { continue; }

        unsupported++;

        // Filter to search-exhausted
        bool is_exhausted = false;
        if (result.value().diag.reason_code.has_value()) {
            is_exhausted =
                result.value().diag.reason_code->category == ReasonCategory::kSearchExhausted;
        }
        if (!is_exhausted) { continue; }

        exhausted++;

        auto cls       = ClassifyStructural(**folded_ptr);
        auto elim      = EliminateAuxVars(parse_result.value().sig, parse_result.value().vars);
        auto rv        = static_cast< uint32_t >(elim.real_vars.size());
        auto tree_size = CountNodes(**folded_ptr);

        // Analyze lifting potential
        auto arith_info = AnalyzeArithLifting(**folded_ptr, parse_result.value().vars, 64);
        auto repeat_info =
            AnalyzeRepeatedLifting(**folded_ptr, parse_result.value().vars, 64, 16);

        std::string gt_str = trim(line.substr(sep + 1));

        std::string reason;
        if (!result.value().diag.cause_chain.empty()) {
            const auto &top = result.value().diag.cause_chain.back();
            reason          = top.message.substr(0, 80);
        } else {
            reason = result.value().diag.reason.substr(0, 80);
        }

        records.push_back(
            LiftingRecord{
                .line_num            = line_num,
                .ground_truth        = gt_str.substr(0, 70),
                .cls                 = cls,
                .real_vars           = rv,
                .tree_size           = tree_size,
                .total_expansions    = result.value().telemetry.total_expansions,
                .max_depth           = result.value().telemetry.max_depth_reached,
                .candidates_verified = result.value().telemetry.candidates_verified,
                .queue_high_water    = result.value().telemetry.queue_high_water,
                .terminal_reason     = reason,
                .arith_lift          = arith_info,
                .repeat_lift         = repeat_info,
            }
        );
    }

    std::cerr << "\n=== QSynth Search-Exhausted Lifting Diagnostic ===\n";
    std::cerr << "parsed=" << parsed << " simplified=" << simplified
              << " unsupported=" << unsupported << " exhausted=" << exhausted << "\n";

    // ═══════════════════════════════════════════════════════════
    // 1. Would lifting be blocked by max_vars?
    // ═══════════════════════════════════════════════════════════

    int blocked_count = 0;
    int not_blocked   = 0;
    int no_viable     = 0;
    for (const auto &r : records) {
        if (r.repeat_lift.viable_before_filter == 0) {
            no_viable++;
        } else if (r.repeat_lift.blocked_by_max_vars) {
            blocked_count++;
        } else {
            not_blocked++;
        }
    }

    std::cerr << "\n════════════════════════════════════════════\n";
    std::cerr << "1. Repeated-Subexpr Lifting Feasibility\n";
    std::cerr << "════════════════════════════════════════════\n";
    std::cerr << "  Would be blocked by max_vars(16): " << blocked_count << " / "
              << records.size() << "\n";
    std::cerr << "  Would NOT be blocked:             " << not_blocked << " / "
              << records.size() << "\n";
    std::cerr << "  No viable candidates (all unique): " << no_viable << " / " << records.size()
              << "\n";

    // Projected var counts
    std::cerr << "\n  Projected total_vars distribution:\n";
    std::map< std::string, int > var_buckets;
    for (const auto &r : records) {
        uint32_t pv = r.repeat_lift.projected_total_vars;
        std::string bucket;
        if (pv <= 8) {
            bucket = "1-8";
        } else if (pv <= 16) {
            bucket = "9-16";
        } else if (pv <= 32) {
            bucket = "17-32";
        } else if (pv <= 64) {
            bucket = "33-64";
        } else if (pv <= 128) {
            bucket = "65-128";
        } else {
            bucket = "129+";
        }
        var_buckets[bucket]++;
    }
    for (const auto &bucket :
         std::vector< std::string >{ "1-8", "9-16", "17-32", "33-64", "65-128", "129+" })
    {
        int c = var_buckets.count(bucket) != 0 ? var_buckets[bucket] : 0;
        std::cerr << "    " << std::setw(6) << bucket << ": " << c << "\n";
    }

    // ═══════════════════════════════════════════════════════════
    // 2. Arith-atom lifting feasibility
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n════════════════════════════════════════════\n";
    std::cerr << "2. Arithmetic-Atom Lifting Feasibility\n";
    std::cerr << "════════════════════════════════════════════\n";

    int arith_no_candidates = 0;
    int arith_blocked       = 0;
    int arith_feasible      = 0;
    for (const auto &r : records) {
        if (r.arith_lift.total_candidates == 0) {
            arith_no_candidates++;
        } else if (r.real_vars + r.arith_lift.would_add_vars > 16) {
            arith_blocked++;
        } else {
            arith_feasible++;
        }
    }
    std::cerr << "  No arith atoms to lift:           " << arith_no_candidates << "\n";
    std::cerr << "  Would be blocked by max_vars:     " << arith_blocked << "\n";
    std::cerr << "  Would succeed:                    " << arith_feasible << "\n";

    // ═══════════════════════════════════════════════════════════
    // 3. Budget analysis
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n════════════════════════════════════════════\n";
    std::cerr << "3. Orchestrator Budget Usage\n";
    std::cerr << "════════════════════════════════════════════\n";

    uint64_t total_exp_sum = 0;
    uint32_t exp_min = 9999, exp_max = 0;
    for (const auto &r : records) {
        total_exp_sum += r.total_expansions;
        if (r.total_expansions < exp_min) { exp_min = r.total_expansions; }
        if (r.total_expansions > exp_max) { exp_max = r.total_expansions; }
    }
    if (!records.empty()) {
        std::cerr << "  Expansions: min=" << exp_min << " max=" << exp_max
                  << " avg=" << (total_exp_sum / records.size()) << "\n";
    }

    std::map< std::string, int > exp_buckets;
    for (const auto &r : records) {
        std::string bucket;
        if (r.total_expansions <= 10) {
            bucket = "1-10";
        } else if (r.total_expansions <= 50) {
            bucket = "11-50";
        } else if (r.total_expansions <= 100) {
            bucket = "51-100";
        } else if (r.total_expansions <= 500) {
            bucket = "101-500";
        } else if (r.total_expansions <= 1024) {
            bucket = "501-1024";
        } else {
            bucket = "1025+";
        }
        exp_buckets[bucket]++;
    }
    std::cerr << "  Expansion distribution:\n";
    for (const auto &bucket : std::vector< std::string >{ "1-10", "11-50", "51-100", "101-500",
                                                          "501-1024", "1025+" })
    {
        int c = exp_buckets.count(bucket) != 0 ? exp_buckets[bucket] : 0;
        std::cerr << "    " << std::setw(8) << bucket << ": " << c << "\n";
    }

    // ═══════════════════════════════════════════════════════════
    // 4. Savings potential from top-K lifting
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n════════════════════════════════════════════\n";
    std::cerr << "4. Top-K Lifting Potential (capped at max_vars)\n";
    std::cerr << "════════════════════════════════════════════\n";

    // For each expression, compute how much we could save if we
    // lifted only the top-K subtrees that fit within max_vars.
    struct TopKStats
    {
        uint32_t original_nodes;
        uint32_t effective_nodes_after_topk; // nodes saved by top-K lifting
        uint32_t k_used;
        uint64_t savings;
    };

    std::vector< TopKStats > topk_results;
    for (const auto &r : records) {
        uint32_t budget = 16 - static_cast< uint32_t >(std::min< uint32_t >(r.real_vars, 16));
        // Sort viable by benefit = (count-1) * size
        auto viable     = r.repeat_lift.top_viable; // already sorted by count desc
        // Re-sort by benefit
        std::sort(viable.begin(), viable.end(), [](const auto &a, const auto &b) {
            return (uint64_t(a.count - 1) * a.size) > (uint64_t(b.count - 1) * b.size);
        });

        uint64_t savings = 0;
        uint32_t k_used  = 0;
        for (const auto &v : viable) {
            if (k_used >= budget) { break; }
            savings += static_cast< uint64_t >(v.count - 1) * v.size;
            k_used++;
        }

        topk_results.push_back(
            TopKStats{
                .original_nodes             = r.tree_size,
                .effective_nodes_after_topk = r.tree_size
                    - static_cast< uint32_t >(std::min< uint64_t >(savings, r.tree_size)),
                .k_used  = k_used,
                .savings = savings,
            }
        );
    }

    // Reduction ratio histogram
    std::cerr << "  Node reduction from top-K (K limited to var budget):\n";
    std::map< std::string, int > reduction_buckets;
    for (size_t i = 0; i < records.size(); ++i) {
        float ratio = 0;
        if (topk_results[i].original_nodes > 0) {
            ratio = 1.0F
                - static_cast< float >(topk_results[i].effective_nodes_after_topk)
                    / static_cast< float >(topk_results[i].original_nodes);
        }
        std::string bucket;
        if (ratio < 0.1F) {
            bucket = "<10%";
        } else if (ratio < 0.3F) {
            bucket = "10-30%";
        } else if (ratio < 0.5F) {
            bucket = "30-50%";
        } else if (ratio < 0.7F) {
            bucket = "50-70%";
        } else {
            bucket = "70%+";
        }
        reduction_buckets[bucket]++;
    }
    for (const auto &bucket :
         std::vector< std::string >{ "<10%", "10-30%", "30-50%", "50-70%", "70%+" })
    {
        int c = reduction_buckets.count(bucket) != 0 ? reduction_buckets[bucket] : 0;
        std::cerr << "    " << std::setw(6) << bucket << ": " << c << "\n";
    }

    // ═══════════════════════════════════════════════════════════
    // 5. Detailed per-expression listing
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n════════════════════════════════════════════\n";
    std::cerr << "5. Detailed Listing (all " << records.size() << ")\n";
    std::cerr << "════════════════════════════════════════════\n";

    for (size_t i = 0; i < records.size(); ++i) {
        const auto &r = records[i];
        const auto &t = topk_results[i];

        std::cerr << "\n[" << (i + 1) << "] L" << r.line_num << " vars=" << r.real_vars
                  << " semantic=" << semantic_str(r.cls.semantic) << " nodes=" << r.tree_size
                  << "\n";
        std::cerr << "  GT: " << r.ground_truth << "\n";
        std::cerr << "  Budget: expansions=" << r.total_expansions << " depth=" << r.max_depth
                  << " verified=" << r.candidates_verified << " hwm=" << r.queue_high_water
                  << "\n";
        std::cerr << "  ArithLift: candidates=" << r.arith_lift.total_candidates
                  << " unique=" << r.arith_lift.unique_atoms
                  << " +vars=" << r.arith_lift.would_add_vars << " blocked="
                  << (r.real_vars + r.arith_lift.would_add_vars > 16 ? "YES" : "no") << "\n";
        std::cerr << "  RepeatLift: viable=" << r.repeat_lift.viable_before_filter
                  << " selected(ub)=" << r.repeat_lift.selected_count
                  << " +vars=" << r.repeat_lift.would_add_vars
                  << " projected=" << r.repeat_lift.projected_total_vars
                  << " blocked=" << (r.repeat_lift.blocked_by_max_vars ? "YES" : "no")
                  << " max_repeat=" << r.repeat_lift.max_repeat_count
                  << " savings=" << r.repeat_lift.total_savings << "\n";
        std::cerr << "  TopK: k=" << t.k_used << " savings=" << t.savings << " "
                  << t.original_nodes << "->" << t.effective_nodes_after_topk << " nodes\n";

        // Show top viable subtrees
        if (!r.repeat_lift.top_viable.empty()) {
            std::cerr << "  Top repeated subtrees:\n";
            for (size_t j = 0; j < r.repeat_lift.top_viable.size(); ++j) {
                const auto &v = r.repeat_lift.top_viable[j];
                std::cerr << "    #" << (j + 1) << ": " << v.count << "x " << v.size
                          << "nodes benefit=" << ((v.count - 1) * v.size) << "\n";
            }
        }
    }

    std::cerr << "\n";
}
