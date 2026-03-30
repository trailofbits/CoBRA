#include "ExprParser.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
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
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

    // ── AST metrics ─────────────────────────────────────────────

    struct AstMetrics
    {
        uint32_t total_nodes   = 0;
        uint32_t max_depth     = 0;
        uint32_t add_count     = 0;
        uint32_t mul_count     = 0;
        uint32_t and_count     = 0;
        uint32_t or_count      = 0;
        uint32_t xor_count     = 0;
        uint32_t not_count     = 0;
        uint32_t neg_count     = 0;
        uint32_t shr_count     = 0;
        uint32_t const_count   = 0;
        uint32_t var_count     = 0; // total variable references
        uint32_t distinct_vars = 0;
        uint32_t var_mul_count = 0; // var*var multiplications
    };

    void collect_metrics(const Expr &expr, uint32_t depth, AstMetrics &m) {
        m.total_nodes++;
        if (depth > m.max_depth) { m.max_depth = depth; }

        switch (expr.kind) {
            case Expr::Kind::kConstant:
                m.const_count++;
                break;
            case Expr::Kind::kVariable:
                m.var_count++;
                break;
            case Expr::Kind::kAdd:
                m.add_count++;
                break;
            case Expr::Kind::kMul:
                m.mul_count++;
                if (expr.children.size() == 2 && HasVarDep(*expr.children[0])
                    && HasVarDep(*expr.children[1]))
                {
                    m.var_mul_count++;
                }
                break;
            case Expr::Kind::kAnd:
                m.and_count++;
                break;
            case Expr::Kind::kOr:
                m.or_count++;
                break;
            case Expr::Kind::kXor:
                m.xor_count++;
                break;
            case Expr::Kind::kNot:
                m.not_count++;
                break;
            case Expr::Kind::kNeg:
                m.neg_count++;
                break;
            case Expr::Kind::kShr:
                m.shr_count++;
                break;
        }

        for (const auto &child : expr.children) { collect_metrics(*child, depth + 1, m); }
    }

    // ── Top-level skeleton classification ───────────────────────

    // Classify the "shape" of the top few levels of the AST.
    // We produce a short string like "Add(Xor,And)" or "Xor(Add,Or)"
    // truncated at depth 2.
    std::string kind_tag(Expr::Kind k) {
        switch (k) {
            case Expr::Kind::kConstant:
                return "C";
            case Expr::Kind::kVariable:
                return "V";
            case Expr::Kind::kAdd:
                return "+";
            case Expr::Kind::kMul:
                return "*";
            case Expr::Kind::kAnd:
                return "&";
            case Expr::Kind::kOr:
                return "|";
            case Expr::Kind::kXor:
                return "^";
            case Expr::Kind::kNot:
                return "~";
            case Expr::Kind::kNeg:
                return "-";
            case Expr::Kind::kShr:
                return ">>";
        }
        return "?";
    }

    std::string skeleton(const Expr &expr, int max_depth) {
        std::string tag = kind_tag(expr.kind);
        if (max_depth <= 0 || expr.children.empty()) { return tag; }

        tag += "(";
        for (size_t i = 0; i < expr.children.size(); ++i) {
            if (i > 0) { tag += ","; }
            tag += skeleton(*expr.children[i], max_depth - 1);
        }
        tag += ")";
        return tag;
    }

    // ── Operator interleaving pattern ───────────────────────────

    // Classify the "interleaving style" of the expression:
    // - "bitwise-over-arith": top level is bitwise, children have arithmetic
    // - "arith-over-bitwise": top level is arithmetic, children have bitwise
    // - "alternating": both within top 3 levels
    // - "pure-bitwise": no arithmetic in top levels
    // - "pure-arith": no bitwise in top levels

    bool is_bitwise_kind(Expr::Kind k) {
        return k == Expr::Kind::kAnd || k == Expr::Kind::kOr || k == Expr::Kind::kXor
            || k == Expr::Kind::kNot;
    }

    bool is_arith_kind(Expr::Kind k) {
        return k == Expr::Kind::kAdd || k == Expr::Kind::kMul || k == Expr::Kind::kNeg;
    }

    struct InterleavingInfo
    {
        bool has_bitwise_at[4] = {}; // depth 0-3
        bool has_arith_at[4]   = {}; // depth 0-3
    };

    void collect_interleaving(const Expr &expr, int depth, InterleavingInfo &info) {
        if (depth > 3) { return; }
        if (is_bitwise_kind(expr.kind)) { info.has_bitwise_at[depth] = true; }
        if (is_arith_kind(expr.kind)) { info.has_arith_at[depth] = true; }
        for (const auto &child : expr.children) {
            collect_interleaving(*child, depth + 1, info);
        }
    }

    std::string interleaving_pattern(const InterleavingInfo &info) {
        // Build a 4-char pattern: B=bitwise, A=arith, M=mixed, .=leaf
        std::string pattern;
        for (int d = 0; d < 4; ++d) {
            bool b = info.has_bitwise_at[d];
            bool a = info.has_arith_at[d];
            if (b && a) {
                pattern += 'M';
            } else if (b) {
                pattern += 'B';
            } else if (a) {
                pattern += 'A';
            } else {
                pattern += '.';
            }
        }
        return pattern;
    }

    // ── Shared subexpression analysis ───────────────────────────

    struct SubexprStats
    {
        uint32_t total_subtrees     = 0;
        uint32_t unique_subtrees    = 0;
        uint32_t duplicate_subtrees = 0; // nodes appearing 2+ times
        uint32_t max_duplicates     = 0; // highest repeat count
        float duplication_ratio     = 0; // duplicate / total
        uint32_t largest_dup_size   = 0; // node count of largest dup
        std::string largest_dup_shape;   // skeleton of largest dup
    };

    struct SubtreeInfo
    {
        size_t hash;
        uint32_t node_count;
        std::string shape; // skeleton at depth 2
    };

    void collect_subtree_hashes(
        const Expr &expr, std::unordered_map< size_t, std::vector< SubtreeInfo > > &hash_map
    ) {
        size_t h        = std::hash< Expr >{}(expr);
        uint32_t ncount = 0;

        // Count nodes in this subtree
        std::function< uint32_t(const Expr &) > count_nodes =
            [&count_nodes](const Expr &e) -> uint32_t {
            uint32_t n = 1;
            for (const auto &c : e.children) { n += count_nodes(*c); }
            return n;
        };
        ncount = count_nodes(expr);

        // Only track subtrees with >= 3 nodes (leaf dups are uninteresting)
        if (ncount >= 3) {
            hash_map[h].push_back(
                { .hash = h, .node_count = ncount, .shape = skeleton(expr, 2) }
            );
        }

        for (const auto &child : expr.children) { collect_subtree_hashes(*child, hash_map); }
    }

    SubexprStats compute_subexpr_stats(const Expr &expr) {
        std::unordered_map< size_t, std::vector< SubtreeInfo > > hash_map;
        collect_subtree_hashes(expr, hash_map);

        SubexprStats stats;
        for (const auto &[h, entries] : hash_map) {
            stats.total_subtrees += static_cast< uint32_t >(entries.size());
            stats.unique_subtrees++;
            if (entries.size() >= 2) {
                stats.duplicate_subtrees += static_cast< uint32_t >(entries.size());
                if (entries.size() > stats.max_duplicates) {
                    stats.max_duplicates = static_cast< uint32_t >(entries.size());
                }
                if (entries[0].node_count > stats.largest_dup_size) {
                    stats.largest_dup_size  = entries[0].node_count;
                    stats.largest_dup_shape = entries[0].shape;
                }
            }
        }
        if (stats.total_subtrees > 0) {
            stats.duplication_ratio = static_cast< float >(stats.duplicate_subtrees)
                / static_cast< float >(stats.total_subtrees);
        }
        return stats;
    }

    // ── Variable interaction analysis ───────────────────────────

    struct VarInteraction
    {
        std::set< uint32_t > vars_in_mul;       // vars appearing under var*var
        std::set< uint32_t > vars_in_addsub;    // vars under add/sub only
        std::set< uint32_t > vars_bitwise_only; // vars only in bitwise context
        uint32_t mul_pair_count = 0;            // count of var*var nodes
    };

    void collect_var_interaction(
        const Expr &expr, bool under_mul, bool under_arith, VarInteraction &vi
    ) {
        switch (expr.kind) {
            case Expr::Kind::kVariable:
                if (under_mul) {
                    vi.vars_in_mul.insert(expr.var_index);
                } else if (under_arith) {
                    vi.vars_in_addsub.insert(expr.var_index);
                } else {
                    vi.vars_bitwise_only.insert(expr.var_index);
                }
                return;

            case Expr::Kind::kConstant:
                return;

            case Expr::Kind::kMul:
                if (expr.children.size() == 2 && HasVarDep(*expr.children[0])
                    && HasVarDep(*expr.children[1]))
                {
                    vi.mul_pair_count++;
                    for (const auto &c : expr.children) {
                        collect_var_interaction(*c, true, true, vi);
                    }
                    return;
                }
                for (const auto &c : expr.children) {
                    collect_var_interaction(*c, under_mul, true, vi);
                }
                return;

            case Expr::Kind::kAdd:
            case Expr::Kind::kNeg:
                for (const auto &c : expr.children) {
                    collect_var_interaction(*c, under_mul, true, vi);
                }
                return;

            default: // bitwise ops
                for (const auto &c : expr.children) {
                    collect_var_interaction(*c, false, false, vi);
                }
                return;
        }
    }

    // ── Ground truth complexity ─────────────────────────────────

    // Classify the ground truth expression to understand what the
    // "answer" looks like — is it linear, polynomial, bitwise, etc.?
    struct GroundTruthInfo
    {
        bool has_mul        = false;
        bool has_bitwise    = false;
        bool has_arith      = false;
        uint32_t node_count = 0;
        uint32_t depth      = 0;
    };

    void analyze_gt(const Expr &expr, uint32_t depth, GroundTruthInfo &info) {
        info.node_count++;
        if (depth > info.depth) { info.depth = depth; }
        if (is_arith_kind(expr.kind)) { info.has_arith = true; }
        if (is_bitwise_kind(expr.kind)) { info.has_bitwise = true; }
        if (expr.kind == Expr::Kind::kMul && expr.children.size() == 2
            && HasVarDep(*expr.children[0]) && HasVarDep(*expr.children[1]))
        {
            info.has_mul = true;
        }
        for (const auto &c : expr.children) { analyze_gt(*c, depth + 1, info); }
    }

    std::string gt_class(const GroundTruthInfo &info) {
        if (info.has_mul && info.has_bitwise) { return "mixed-poly"; }
        if (info.has_mul) { return "polynomial"; }
        if (info.has_bitwise && info.has_arith) { return "semilinear"; }
        if (info.has_bitwise) { return "bitwise"; }
        if (info.has_arith) { return "linear"; }
        return "constant";
    }

    // ── Add/Xor ladder detection ────────────────────────────────

    // Count the length of the longest chain of alternating add/xor ops
    // (a pattern common in MBA obfuscation).
    uint32_t add_xor_ladder_length(const Expr &expr) {
        if (expr.kind != Expr::Kind::kAdd && expr.kind != Expr::Kind::kXor) { return 0; }

        uint32_t best = 0;
        for (const auto &child : expr.children) {
            bool alternates = (expr.kind == Expr::Kind::kAdd && child->kind == Expr::Kind::kXor)
                || (expr.kind == Expr::Kind::kXor && child->kind == Expr::Kind::kAdd);
            if (alternates) {
                uint32_t chain = 1 + add_xor_ladder_length(*child);
                if (chain > best) { best = chain; }
            } else {
                uint32_t chain = add_xor_ladder_length(*child);
                if (chain > best) { best = chain; }
            }
        }
        return best;
    }

    // ── Per-expression record ───────────────────────────────────

    struct ExhaustedRecord
    {
        int line_num;
        std::string ground_truth;
        std::string obfuscated_prefix;
        Classification cls;
        uint32_t real_vars;
        AstMetrics metrics;
        std::string skel;       // depth-2 skeleton
        std::string interleave; // BAMBA-style pattern
        SubexprStats subexpr;
        VarInteraction var_int;
        GroundTruthInfo gt_info;
        uint32_t add_xor_ladder;
    };

    void scan_exhausted(
        const std::string &path, std::vector< ExhaustedRecord > &records, int &total_parsed,
        int &total_simplified, int &total_unsupported, int &total_exhausted
    ) {
        std::ifstream file(path);
        ASSERT_TRUE(file.is_open()) << "Cannot open " << path;

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

            total_parsed++;

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
                total_simplified++;
                continue;
            }
            if (result.value().kind != SimplifyOutcome::Kind::kUnchangedUnsupported) {
                continue;
            }

            total_unsupported++;

            // Filter to search-exhausted only
            bool is_exhausted = false;
            if (result.value().diag.reason_code.has_value()) {
                is_exhausted = result.value().diag.reason_code->category
                    == ReasonCategory::kSearchExhausted;
            }
            if (!is_exhausted && result.value().diag.reason.find("search") != std::string::npos)
            {
                is_exhausted = true;
            }
            if (!is_exhausted) { continue; }

            total_exhausted++;

            auto cls  = ClassifyStructural(**folded_ptr);
            auto elim = EliminateAuxVars(parse_result.value().sig, parse_result.value().vars);
            auto rv   = static_cast< uint32_t >(elim.real_vars.size());

            // Collect all structural analyses
            AstMetrics metrics{};
            collect_metrics(**folded_ptr, 0, metrics);

            // Count distinct variables
            std::set< uint32_t > var_set;
            std::function< void(const Expr &) > collect_vars = [&collect_vars,
                                                                &var_set](const Expr &e) {
                if (e.kind == Expr::Kind::kVariable) { var_set.insert(e.var_index); }
                for (const auto &c : e.children) { collect_vars(*c); }
            };
            collect_vars(**folded_ptr);
            metrics.distinct_vars = static_cast< uint32_t >(var_set.size());

            std::string skel = skeleton(**folded_ptr, 2);

            InterleavingInfo il_info{};
            collect_interleaving(**folded_ptr, 0, il_info);
            std::string interleave = interleaving_pattern(il_info);

            auto subexpr = compute_subexpr_stats(**folded_ptr);

            VarInteraction var_int{};
            collect_var_interaction(**folded_ptr, false, false, var_int);

            // Parse and analyze ground truth
            std::string gt_str = trim(line.substr(sep + 1));
            GroundTruthInfo gt_info{};
            auto gt_ast = ParseToAst(gt_str, 64);
            if (gt_ast.has_value()) { analyze_gt(*gt_ast.value().expr, 0, gt_info); }

            uint32_t ladder = add_xor_ladder_length(**folded_ptr);

            records.push_back(
                { .line_num          = line_num,
                  .ground_truth      = gt_str.substr(0, 80),
                  .obfuscated_prefix = obfuscated.substr(0, 100),
                  .cls               = cls,
                  .real_vars         = rv,
                  .metrics           = metrics,
                  .skel              = skel,
                  .interleave        = interleave,
                  .subexpr           = subexpr,
                  .var_int           = var_int,
                  .gt_info           = gt_info,
                  .add_xor_ladder    = ladder }
            );
        }
    }

} // namespace

TEST(SearchExhaustedAudit, StructuralAnalysis) {
    std::vector< ExhaustedRecord > records;
    int parsed = 0, simplified = 0, unsupported = 0, exhausted = 0;

    std::cerr << "\n=== Scanning QSynth for search-exhausted ===\n";
    scan_exhausted(
        DATASET_DIR "/gamba/qsynth_ea.txt", records, parsed, simplified, unsupported, exhausted
    );
    std::cerr << "  parsed=" << parsed << " simplified=" << simplified
              << " unsupported=" << unsupported << " exhausted=" << exhausted << "\n";

    // ═══════════════════════════════════════════════════════════
    // AXIS A: Top-level algebraic skeleton
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n════════════════════════════════════════════\n";
    std::cerr << "AXIS A: Top-Level Skeleton Families\n";
    std::cerr << "════════════════════════════════════════════\n";

    // Group by depth-2 skeleton
    std::map< std::string, int > skel_counts;
    for (const auto &r : records) { skel_counts[r.skel]++; }

    // Sort by frequency
    std::vector< std::pair< std::string, int > > skel_sorted(
        skel_counts.begin(), skel_counts.end()
    );
    std::sort(skel_sorted.begin(), skel_sorted.end(), [](const auto &a, const auto &b) {
        return a.second > b.second;
    });

    std::cerr << "\nTop skeleton shapes (depth-2):\n";
    for (size_t i = 0; i < skel_sorted.size() && i < 15; ++i) {
        std::cerr << "  " << std::setw(4) << skel_sorted[i].second << "x  "
                  << skel_sorted[i].first << "\n";
    }

    // Interleaving patterns
    std::map< std::string, int > il_counts;
    for (const auto &r : records) { il_counts[r.interleave]++; }

    std::cerr << "\nInterleaving patterns (depth 0-3, B=bitwise A=arith M=mixed):\n";
    std::vector< std::pair< std::string, int > > il_sorted(il_counts.begin(), il_counts.end());
    std::sort(il_sorted.begin(), il_sorted.end(), [](const auto &a, const auto &b) {
        return a.second > b.second;
    });
    for (const auto &[pat, count] : il_sorted) {
        std::cerr << "  " << std::setw(4) << count << "x  " << pat << "\n";
    }

    // Semantic class distribution
    std::map< std::string, int > sem_counts;
    for (const auto &r : records) { sem_counts[semantic_str(r.cls.semantic)]++; }
    std::cerr << "\nSemantic class:\n";
    for (auto &[k, v] : sem_counts) { std::cerr << "  " << k << ": " << v << "\n"; }

    // Structural flags
    std::map< std::string, int > flag_counts;
    for (const auto &r : records) { flag_counts[flag_str(r.cls.flags)]++; }
    std::cerr << "\nStructural flag combos:\n";
    std::vector< std::pair< std::string, int > > flag_sorted(
        flag_counts.begin(), flag_counts.end()
    );
    std::sort(flag_sorted.begin(), flag_sorted.end(), [](const auto &a, const auto &b) {
        return a.second > b.second;
    });
    for (const auto &[f, n] : flag_sorted) {
        std::cerr << "  " << std::setw(4) << n << "x  {" << f << "}\n";
    }

    // Add/xor ladder lengths
    std::map< uint32_t, int > ladder_counts;
    for (const auto &r : records) { ladder_counts[r.add_xor_ladder]++; }
    std::cerr << "\nAdd/Xor ladder lengths:\n";
    for (auto &[len, count] : ladder_counts) {
        std::cerr << "  " << len << ": " << count << "\n";
    }

    // ═══════════════════════════════════════════════════════════
    // AXIS B: Shared subexpression structure
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n════════════════════════════════════════════\n";
    std::cerr << "AXIS B: Shared Subexpression Structure\n";
    std::cerr << "════════════════════════════════════════════\n";

    // Duplication ratio histogram
    std::map< std::string, int > dup_buckets;
    for (const auto &r : records) {
        float ratio = r.subexpr.duplication_ratio;
        std::string bucket;
        if (ratio < 0.1F) {
            bucket = "0-10%";
        } else if (ratio < 0.3F) {
            bucket = "10-30%";
        } else if (ratio < 0.5F) {
            bucket = "30-50%";
        } else if (ratio < 0.7F) {
            bucket = "50-70%";
        } else {
            bucket = "70-100%";
        }
        dup_buckets[bucket]++;
    }

    std::cerr << "\nDuplication ratio (repeated subtrees / total subtrees):\n";
    for (const auto &bucket :
         std::vector< std::string >{ "0-10%", "10-30%", "30-50%", "50-70%", "70-100%" })
    {
        int count = dup_buckets.count(bucket) != 0 ? dup_buckets[bucket] : 0;
        std::cerr << "  " << std::setw(7) << bucket << ": " << count << "\n";
    }

    // Max duplicate count histogram
    std::cerr << "\nMax repeat count for any subtree:\n";
    std::map< uint32_t, int > max_dup_counts;
    for (const auto &r : records) { max_dup_counts[r.subexpr.max_duplicates]++; }
    for (auto &[n, count] : max_dup_counts) {
        std::cerr << "  " << n << "x repeated: " << count << " expressions\n";
    }

    // Largest duplicated subtree shapes
    std::map< std::string, int > dup_shapes;
    for (const auto &r : records) {
        if (r.subexpr.largest_dup_size >= 3) { dup_shapes[r.subexpr.largest_dup_shape]++; }
    }
    std::cerr << "\nMost common shapes of largest duplicated subtree:\n";
    std::vector< std::pair< std::string, int > > shape_sorted(
        dup_shapes.begin(), dup_shapes.end()
    );
    std::sort(shape_sorted.begin(), shape_sorted.end(), [](const auto &a, const auto &b) {
        return a.second > b.second;
    });
    for (size_t i = 0; i < shape_sorted.size() && i < 10; ++i) {
        std::cerr << "  " << std::setw(4) << shape_sorted[i].second << "x  "
                  << shape_sorted[i].first << "\n";
    }

    // ═══════════════════════════════════════════════════════════
    // AXIS C: Variable interaction
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n════════════════════════════════════════════\n";
    std::cerr << "AXIS C: Variable Interaction\n";
    std::cerr << "════════════════════════════════════════════\n";

    std::map< uint32_t, int > real_var_counts;
    std::map< uint32_t, int > mul_pair_counts;
    int has_any_mul      = 0;
    int vars_in_mul_only = 0; // exprs where every var appears under mul

    for (const auto &r : records) {
        real_var_counts[r.real_vars]++;
        mul_pair_counts[r.var_int.mul_pair_count]++;
        if (!r.var_int.vars_in_mul.empty()) { has_any_mul++; }

        // Check if all vars are in mul context
        std::set< uint32_t > all_vars;
        all_vars.insert(r.var_int.vars_in_mul.begin(), r.var_int.vars_in_mul.end());
        all_vars.insert(r.var_int.vars_in_addsub.begin(), r.var_int.vars_in_addsub.end());
        all_vars.insert(r.var_int.vars_bitwise_only.begin(), r.var_int.vars_bitwise_only.end());
        if (!all_vars.empty() && all_vars == r.var_int.vars_in_mul) { vars_in_mul_only++; }
    }

    std::cerr << "\nReal variable count:\n";
    for (auto &[n, count] : real_var_counts) {
        std::cerr << "  " << n << " vars: " << count << "\n";
    }

    std::cerr << "\nVar*var multiplication nodes per expression:\n";
    for (auto &[n, count] : mul_pair_counts) {
        std::cerr << "  " << n << " mul-pairs: " << count << "\n";
    }

    std::cerr << "\nExpressions with any var*var mul: " << has_any_mul << " / "
              << records.size() << "\n";
    std::cerr << "Expressions where ALL vars appear under mul: " << vars_in_mul_only << " / "
              << records.size() << "\n";

    // Hub variable analysis: which vars appear most under mul
    std::map< uint32_t, int > mul_var_freq;
    for (const auto &r : records) {
        for (auto v : r.var_int.vars_in_mul) { mul_var_freq[v]++; }
    }

    // ═══════════════════════════════════════════════════════════
    // AXIS D: Ground truth complexity
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n════════════════════════════════════════════\n";
    std::cerr << "AXIS D: Ground Truth Complexity\n";
    std::cerr << "════════════════════════════════════════════\n";

    std::map< std::string, int > gt_classes;
    for (const auto &r : records) { gt_classes[gt_class(r.gt_info)]++; }

    std::cerr << "\nGround truth expression class:\n";
    for (auto &[k, v] : gt_classes) { std::cerr << "  " << k << ": " << v << "\n"; }

    // GT size distribution
    uint32_t gt_min = 999, gt_max = 0;
    uint64_t gt_sum = 0;
    for (const auto &r : records) {
        if (r.gt_info.node_count < gt_min) { gt_min = r.gt_info.node_count; }
        if (r.gt_info.node_count > gt_max) { gt_max = r.gt_info.node_count; }
        gt_sum += r.gt_info.node_count;
    }
    if (!records.empty()) {
        std::cerr << "\nGround truth size: min=" << gt_min << " max=" << gt_max
                  << " avg=" << (gt_sum / records.size()) << "\n";
    }

    // ═══════════════════════════════════════════════════════════
    // AXIS E: AST size and complexity metrics
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n════════════════════════════════════════════\n";
    std::cerr << "AXIS E: Obfuscated Expression Complexity\n";
    std::cerr << "════════════════════════════════════════════\n";

    if (!records.empty()) {
        uint32_t node_min = 999, node_max = 0;
        uint64_t node_sum  = 0;
        uint32_t depth_min = 999, depth_max = 0;
        uint64_t depth_sum = 0;

        for (const auto &r : records) {
            if (r.metrics.total_nodes < node_min) { node_min = r.metrics.total_nodes; }
            if (r.metrics.total_nodes > node_max) { node_max = r.metrics.total_nodes; }
            node_sum += r.metrics.total_nodes;
            if (r.metrics.max_depth < depth_min) { depth_min = r.metrics.max_depth; }
            if (r.metrics.max_depth > depth_max) { depth_max = r.metrics.max_depth; }
            depth_sum += r.metrics.max_depth;
        }

        auto n = records.size();
        std::cerr << "Node count: min=" << node_min << " max=" << node_max
                  << " avg=" << (node_sum / n) << "\n";
        std::cerr << "Depth:      min=" << depth_min << " max=" << depth_max
                  << " avg=" << (depth_sum / n) << "\n";
    }

    // Node type distribution (aggregated)
    uint64_t total_add = 0, total_mul = 0, total_and = 0;
    uint64_t total_or = 0, total_xor = 0, total_not = 0, total_neg = 0;
    for (const auto &r : records) {
        total_add += r.metrics.add_count;
        total_mul += r.metrics.mul_count;
        total_and += r.metrics.and_count;
        total_or  += r.metrics.or_count;
        total_xor += r.metrics.xor_count;
        total_not += r.metrics.not_count;
        total_neg += r.metrics.neg_count;
    }
    std::cerr << "\nAggregated op counts across all " << records.size()
              << " exhausted expressions:\n";
    std::cerr << "  Add: " << total_add << "  Mul: " << total_mul << "  And: " << total_and
              << "  Or: " << total_or << "  Xor: " << total_xor << "  Not: " << total_not
              << "  Neg: " << total_neg << "\n";

    // ═══════════════════════════════════════════════════════════
    // Detailed per-expression listing (first 15)
    // ═══════════════════════════════════════════════════════════

    std::cerr << "\n════════════════════════════════════════════\n";
    std::cerr << "Detailed Listing (first 15)\n";
    std::cerr << "════════════════════════════════════════════\n";

    for (size_t i = 0; i < records.size() && i < 15; ++i) {
        const auto &r = records[i];
        std::cerr << "\n[" << (i + 1) << "] L" << r.line_num << " real_vars=" << r.real_vars
                  << " semantic=" << semantic_str(r.cls.semantic) << " flags={"
                  << flag_str(r.cls.flags) << "}\n";
        std::cerr << "  GT: " << r.ground_truth << "  (" << gt_class(r.gt_info) << ", "
                  << r.gt_info.node_count << " nodes)\n";
        std::cerr << "  Skeleton: " << r.skel << "\n";
        std::cerr << "  Interleave: " << r.interleave << "\n";
        std::cerr << "  Nodes=" << r.metrics.total_nodes << " Depth=" << r.metrics.max_depth
                  << " Add=" << r.metrics.add_count << " Mul=" << r.metrics.mul_count
                  << " And=" << r.metrics.and_count << " Or=" << r.metrics.or_count
                  << " Xor=" << r.metrics.xor_count << "\n";
        std::cerr << "  Dup: ratio=" << std::fixed << std::setprecision(0)
                  << (r.subexpr.duplication_ratio * 100)
                  << "% max_repeat=" << r.subexpr.max_duplicates
                  << " largest_dup=" << r.subexpr.largest_dup_size
                  << "nodes shape=" << r.subexpr.largest_dup_shape << "\n";
        std::cerr << "  VarMul={";
        for (auto v : r.var_int.vars_in_mul) { std::cerr << v << ","; }
        std::cerr << "} MulPairs=" << r.var_int.mul_pair_count
                  << " AddXorLadder=" << r.add_xor_ladder << "\n";
    }

    std::cerr << "\n";
}
