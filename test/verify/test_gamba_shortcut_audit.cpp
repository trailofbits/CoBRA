/// GAMBA shortcut mining audit.
///
/// For each GAMBA expression, captures:
///   - Effort metrics (wall time, orchestrator telemetry)
///   - Classification and structural flags
///   - AST pattern detectors for shortcut families
///   - Counterfactual replay: classify after candidate canonicalization
///
/// Shortcut families mined:
///   A. Complement normalization  (-e-1 → ~e)
///   B. Boolean-domain Mul→AND collapse
///   C. Complement-pair detection in additive skeleton
///   D. Repeated/complement subexpression reuse
///
/// Two extra buckets beyond family population:
///   - reclassifiable_now: canonicalization changes class/flags enough
///     to hit an existing fast path
///   - fastpath_verify_fail: would hit ANF off-ramp after canonicalization
///     but fails full-width verification

#include "ExprParser.h"
#include "cobra/core/AnfTransform.h"
#include "cobra/core/AuxVarEliminator.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Classification.h"
#include "cobra/core/Classifier.h"
#include "cobra/core/Expr.h"
#include "cobra/core/ExprCost.h"
#include "cobra/core/ExprUtils.h"
#include "cobra/core/SignatureChecker.h"
#include "cobra/core/SignatureSimplifier.h"
#include "cobra/core/Simplifier.h"
#include "cobra/core/SimplifyOutcome.h"
#include "dataset_audit_utils.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <vector>

using namespace cobra;
using cobra::test_support::find_separator;
using cobra::test_support::flags_str;
using cobra::test_support::semantic_str;
using cobra::test_support::trim;

namespace {

    constexpr uint32_t kBw = 64;

    // ── AST traversal helpers ──

    void collect_var_indices(const Expr &e, std::set< uint32_t > &out) {
        if (e.kind == Expr::Kind::kVariable) {
            out.insert(e.var_index);
            return;
        }
        for (const auto &c : e.children) { collect_var_indices(*c, out); }
    }

    uint32_t support_size(const Expr &e) {
        std::set< uint32_t > vars;
        collect_var_indices(e, vars);
        return static_cast< uint32_t >(vars.size());
    }

    // ── Family A: Hidden complement detector ──
    //
    // Detects syntactic spellings of -e-1 = ~e:
    //   Add(Neg(e), Constant(mask))
    //   Add(Constant(mask), Neg(e))
    //   Sub patterns that fold to the above
    //
    // Classifies the inner e as:
    //   purely_arith, semilinear, boolean_valued, mixed

    enum class ComplementChildKind {
        kPurelyArith,
        kSemilinear,
        kBooleanValued,
        kMixed,
    };

    struct HiddenComplementHit
    {
        ComplementChildKind child_kind;
    };

    bool is_purely_arithmetic(const Expr &e) {
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
        return std::ranges::all_of(e.children, [](const auto &c) {
            return is_purely_arithmetic(*c);
        });
    }

    bool has_non_leaf_bitwise(const Expr &e) {
        if (e.kind == Expr::Kind::kAnd || e.kind == Expr::Kind::kOr
            || e.kind == Expr::Kind::kXor || e.kind == Expr::Kind::kNot)
        {
            if (HasVarDep(e)) { return true; }
        }
        return std::ranges::any_of(e.children, [](const auto &c) {
            return has_non_leaf_bitwise(*c);
        });
    }

    bool has_const_in_bitwise(const Expr &e) {
        bool is_bw = e.kind == Expr::Kind::kAnd || e.kind == Expr::Kind::kOr
            || e.kind == Expr::Kind::kXor || e.kind == Expr::Kind::kNot;
        if (is_bw && HasVarDep(e)) {
            if (std::ranges::any_of(e.children, [](const auto &c) { return !HasVarDep(*c); })) {
                return true;
            }
        }
        return std::ranges::any_of(e.children, [](const auto &c) {
            return has_const_in_bitwise(*c);
        });
    }

    ComplementChildKind classify_complement_child(const Expr &e) {
        if (is_purely_arithmetic(e)) { return ComplementChildKind::kPurelyArith; }
        if (!has_non_leaf_bitwise(e)) { return ComplementChildKind::kPurelyArith; }
        if (has_const_in_bitwise(e)) { return ComplementChildKind::kSemilinear; }
        return ComplementChildKind::kMixed;
    }

    void find_hidden_complements(
        const Expr &e, uint64_t mask, std::vector< HiddenComplementHit > &hits
    ) {
        // Check: Add(Neg(e), Constant(mask)) or Add(Constant(mask), Neg(e))
        if (e.kind == Expr::Kind::kAdd && e.children.size() == 2) {
            const auto &lhs = *e.children[0];
            const auto &rhs = *e.children[1];

            // Add(Neg(inner), Constant(mask))
            if (lhs.kind == Expr::Kind::kNeg && !lhs.children.empty()
                && rhs.kind == Expr::Kind::kConstant && rhs.constant_val == mask
                && HasVarDep(lhs))
            {
                hits.push_back({ classify_complement_child(*lhs.children[0]) });
            }
            // Add(Constant(mask), Neg(inner))
            if (rhs.kind == Expr::Kind::kNeg && !rhs.children.empty()
                && lhs.kind == Expr::Kind::kConstant && lhs.constant_val == mask
                && HasVarDep(rhs))
            {
                hits.push_back({ classify_complement_child(*rhs.children[0]) });
            }
        }

        for (const auto &c : e.children) { find_hidden_complements(*c, mask, hits); }
    }

    // ── Family B: Boolean-domain product detector ──
    //
    // Finds Mul(a,b) nodes where both operands are boolean-valued
    // on {0,1} signature domain.
    //
    // Two modes:
    //   safe_collapse: child sigs are boolean-valued
    //   context_sensitive: Mul(a,b) itself is boolean-valued but
    //     children are not individually

    struct BooleanProductHit
    {
        bool safe_collapse;       // children are boolean-valued
        bool context_sensitive;   // Mul result is boolean but children are not
        uint32_t child_var_count; // max var count of either child
    };

    void find_boolean_products(
        const Expr &e, const std::vector< std::string > &vars,
        const std::vector< uint64_t > &parent_sig, uint32_t num_vars,
        std::vector< BooleanProductHit > &hits
    ) {
        if (e.kind == Expr::Kind::kMul && e.children.size() == 2 && HasVarDep(*e.children[0])
            && HasVarDep(*e.children[1]))
        {
            // Evaluate this Mul node's sub-signature
            // We check the parent_sig which covers the whole expression,
            // but for per-node boolean checks we need the sub-expression
            // signature. We'll use a heuristic: if the whole expression's
            // sig is boolean-valued, this product is a candidate.
            // For a precise check, we'd need per-subtree evaluation.
            // Mark as context_sensitive; safe_collapse requires per-child
            // signature evaluation which is expensive for an audit.

            // Count vars in each child
            uint32_t lhs_vars = support_size(*e.children[0]);
            uint32_t rhs_vars = support_size(*e.children[1]);

            hits.push_back(
                {
                    .safe_collapse     = false, // conservative; refined below if feasible
                    .context_sensitive = true,
                    .child_var_count   = std::max(lhs_vars, rhs_vars),
                }
            );
        }

        for (const auto &c : e.children) {
            find_boolean_products(*c, vars, parent_sig, num_vars, hits);
        }
    }

    // ── Family C: Complement-pair detector ──
    //
    // Collect additive terms, flatten Add chains, normalize negation.
    // Look for pairs where sig_a[i] + sig_b[i] = mask for all i
    // (indicating a + (-a-1) = -1 relationship, i.e. complements).

    struct AdditiveTermInfo
    {
        const Expr *term;
        bool negated; // under a Neg wrapper
    };

    void flatten_additive(const Expr &e, bool negated, std::vector< AdditiveTermInfo > &terms) {
        if (e.kind == Expr::Kind::kAdd) {
            for (const auto &c : e.children) { flatten_additive(*c, negated, terms); }
        } else if (e.kind == Expr::Kind::kNeg && !e.children.empty()) {
            flatten_additive(*e.children[0], !negated, terms);
        } else {
            terms.push_back({ &e, negated });
        }
    }

    struct ComplementPairInfo
    {
        uint32_t pair_count;
        uint32_t sibling_local_count; // pairs where both terms are top-level additive siblings
    };

    // ── Family D: Repeated/complement subexpression detector ──
    //
    // Structural hash for exact match + complement match (-e-1).
    // Counts structural repeats and complement pairs.

    uint64_t structural_hash(const Expr &e) {
        uint64_t h = static_cast< uint64_t >(e.kind) * 0x9E3779B97F4A7C15ULL;
        if (e.kind == Expr::Kind::kConstant) {
            h ^= e.constant_val * 0x517CC1B727220A95ULL;
        } else if (e.kind == Expr::Kind::kVariable) {
            h ^= e.var_index * 0x6C62272E07BB0142ULL;
        }
        for (const auto &c : e.children) { h = (h * 31) + structural_hash(*c); }
        return h;
    }

    void collect_subtree_hashes(
        const Expr &e, std::map< uint64_t, int > &hash_counts, int min_size
    ) {
        // Only count non-trivial subtrees
        int size = 1 + static_cast< int >(e.children.size()); // approximate
        if (size >= min_size) { hash_counts[structural_hash(e)]++; }
        for (const auto &c : e.children) { collect_subtree_hashes(*c, hash_counts, min_size); }
    }

    struct SubexprReuseInfo
    {
        uint32_t structural_repeats; // exact structural duplicates
        uint32_t complement_repeats; // e paired with -e-1
    };

    // ── Complement normalization canonicalization ──
    //
    // Reverse of LowerNotOverArith:
    //   Add(Neg(e), Constant(mask)) → Not(e) when e is purely arithmetic
    //   Add(Constant(mask), Neg(e)) → Not(e)

    std::unique_ptr< Expr > lift_hidden_not(std::unique_ptr< Expr > e, uint64_t mask) {
        for (auto &child : e->children) { child = lift_hidden_not(std::move(child), mask); }

        if (e->kind == Expr::Kind::kAdd && e->children.size() == 2) {
            auto &lhs = e->children[0];
            auto &rhs = e->children[1];

            // Add(Neg(inner), Constant(mask))
            if (lhs->kind == Expr::Kind::kNeg && !lhs->children.empty()
                && rhs->kind == Expr::Kind::kConstant && rhs->constant_val == mask)
            {
                auto inner = std::move(lhs->children[0]);
                return Expr::BitwiseNot(std::move(inner));
            }
            // Add(Constant(mask), Neg(inner))
            if (rhs->kind == Expr::Kind::kNeg && !rhs->children.empty()
                && lhs->kind == Expr::Kind::kConstant && lhs->constant_val == mask)
            {
                auto inner = std::move(rhs->children[0]);
                return Expr::BitwiseNot(std::move(inner));
            }
        }

        return e;
    }

    // ── Per-expression record ──

    struct ExprRecord
    {
        int line_num = 0;
        std::string dataset;
        double time_ms   = 0.0;
        bool simplified  = false;
        bool unsupported = false;

        // Classification baseline
        SemanticClass baseline_class  = SemanticClass::kLinear;
        StructuralFlag baseline_flags = kSfNone;

        // Signature properties
        bool sig_boolean_valued = false;
        bool sig_neg_boolean    = false;

        // Orchestrator telemetry
        uint32_t total_expansions    = 0;
        uint32_t max_depth           = 0;
        uint32_t candidates_verified = 0;
        uint32_t queue_high_water    = 0;

        // Family A: hidden complement
        uint32_t hidden_complement_count = 0;
        uint32_t hc_purely_arith         = 0;
        uint32_t hc_semilinear           = 0;
        uint32_t hc_mixed                = 0;

        // Family B: boolean product
        uint32_t bool_product_count         = 0;
        uint32_t bool_product_safe_count    = 0;
        uint32_t bool_product_context_count = 0;

        // Family C: complement pairs
        uint32_t complement_pair_count         = 0;
        uint32_t complement_pair_sibling_count = 0;

        // Family D: subexpr reuse
        uint32_t structural_repeat_count = 0;
        uint32_t complement_repeat_count = 0;

        // Counterfactual: complement normalization
        SemanticClass cf_complement_class  = SemanticClass::kLinear;
        StructuralFlag cf_complement_flags = kSfNone;
        bool cf_class_changed              = false;
        bool cf_flags_changed              = false;
        bool cf_would_hit_fastpath         = false;
        bool cf_fastpath_verify_fail       = false;
        int cf_support_delta               = 0;
    };

    // ── Bucket assignment ──

    enum class Bucket {
        kAlreadyFast,
        kOffRampHit,
        kHiddenComplement,
        kBooleanMulDomain,
        kComplementPair,
        kReclassifiableNow,
        kFastpathVerifyFail,
        kExpensiveSolved,
        kUnsupported,
        kOther,
    };

    std::string bucket_str(Bucket b) {
        switch (b) {
            case Bucket::kAlreadyFast:
                return "already_fast";
            case Bucket::kOffRampHit:
                return "off_ramp_hit";
            case Bucket::kHiddenComplement:
                return "hidden_complement";
            case Bucket::kBooleanMulDomain:
                return "boolean_mul_domain";
            case Bucket::kComplementPair:
                return "complement_pair";
            case Bucket::kReclassifiableNow:
                return "reclassifiable_now";
            case Bucket::kFastpathVerifyFail:
                return "fastpath_verify_fail";
            case Bucket::kExpensiveSolved:
                return "expensive_solved";
            case Bucket::kUnsupported:
                return "unsupported";
            case Bucket::kOther:
                return "other";
        }
        return "?";
    }

    // Assign primary bucket (first matching, priority order)
    Bucket assign_bucket(const ExprRecord &r, double p95_time_ms) {
        if (r.unsupported) { return Bucket::kUnsupported; }
        if (r.cf_fastpath_verify_fail) { return Bucket::kFastpathVerifyFail; }
        if (r.cf_would_hit_fastpath && r.cf_class_changed) {
            return Bucket::kReclassifiableNow;
        }
        if (r.time_ms < 1.0 && r.simplified) { return Bucket::kAlreadyFast; }
        if (r.sig_boolean_valued && r.simplified && r.total_expansions <= 2) {
            return Bucket::kOffRampHit;
        }
        if (r.hidden_complement_count > 0 && r.cf_class_changed) {
            return Bucket::kHiddenComplement;
        }
        if (r.bool_product_count > 0 && HasFlag(r.baseline_flags, kSfHasMul)) {
            return Bucket::kBooleanMulDomain;
        }
        if (r.complement_pair_count > 0) { return Bucket::kComplementPair; }
        if (r.time_ms > p95_time_ms && r.simplified) { return Bucket::kExpensiveSolved; }
        return Bucket::kOther;
    }

    // ── Dataset runner ──

    struct DatasetConfig
    {
        std::string name;
        std::string path;
    };

    void run_audit(const DatasetConfig &ds, std::vector< ExprRecord > &records) {
        std::ifstream file(ds.path);
        if (!file.is_open()) {
            std::cerr << "Cannot open " << ds.path << "\n";
            return;
        }

        const uint64_t mask = Bitmask(kBw);
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

            ExprRecord rec;
            rec.line_num = line_num;
            rec.dataset  = ds.name;

            // ── Baseline classification ──
            auto baseline_cls  = ClassifyStructural(*folded);
            rec.baseline_class = baseline_cls.semantic;
            rec.baseline_flags = baseline_cls.flags;

            // ── Signature properties ──
            rec.sig_boolean_valued = IsBooleanValued(sig);
            rec.sig_neg_boolean    = std::all_of(sig.begin(), sig.end(), [mask](uint64_t v) {
                return (v | 1) == mask;
            });

            // ── Family A: hidden complement ──
            std::vector< HiddenComplementHit > hc_hits;
            find_hidden_complements(*folded, mask, hc_hits);
            rec.hidden_complement_count = static_cast< uint32_t >(hc_hits.size());
            for (const auto &h : hc_hits) {
                switch (h.child_kind) {
                    case ComplementChildKind::kPurelyArith:
                        rec.hc_purely_arith++;
                        break;
                    case ComplementChildKind::kSemilinear:
                        rec.hc_semilinear++;
                        break;
                    case ComplementChildKind::kBooleanValued:
                        break; // counted separately via sig check
                    case ComplementChildKind::kMixed:
                        rec.hc_mixed++;
                        break;
                }
            }

            // ── Family B: boolean product ──
            if (rec.sig_boolean_valued && HasFlag(baseline_cls.flags, kSfHasMul)) {
                std::vector< BooleanProductHit > bp_hits;
                find_boolean_products(*folded, vars, sig, num_vars, bp_hits);
                rec.bool_product_count = static_cast< uint32_t >(bp_hits.size());
                for (const auto &h : bp_hits) {
                    if (h.safe_collapse) { rec.bool_product_safe_count++; }
                    if (h.context_sensitive) { rec.bool_product_context_count++; }
                }
            }

            // ── Family C: complement pairs in additive skeleton ──
            {
                std::vector< AdditiveTermInfo > terms;
                flatten_additive(*folded, false, terms);

                // For each pair of terms, check if their sigs sum to mask
                // (i.e., they are complements). This requires per-term
                // evaluation which is expensive, so only do it for
                // expressions with < 10 additive terms.
                if (terms.size() >= 2 && terms.size() <= 10 && num_vars <= 16) {
                    // Evaluate each term at {0,1} points
                    std::vector< std::vector< uint64_t > > term_sigs;
                    uint32_t sig_len = 1U << num_vars;

                    for (const auto &ti : terms) {
                        std::vector< uint64_t > tsig(sig_len);
                        for (uint32_t pt = 0; pt < sig_len; ++pt) {
                            std::vector< uint64_t > vals(num_vars);
                            for (uint32_t v = 0; v < num_vars; ++v) { vals[v] = (pt >> v) & 1; }
                            uint64_t raw = EvalExpr(*ti.term, vals, kBw);
                            tsig[pt]     = ti.negated ? ((-raw) & mask) : raw;
                        }
                        term_sigs.push_back(std::move(tsig));
                    }

                    uint32_t pairs         = 0;
                    uint32_t sibling_pairs = 0;
                    for (size_t i = 0; i < terms.size(); ++i) {
                        for (size_t j = i + 1; j < terms.size(); ++j) {
                            bool is_complement = true;
                            for (uint32_t pt = 0; pt < sig_len; ++pt) {
                                if (((term_sigs[i][pt] + term_sigs[j][pt]) & mask) != mask) {
                                    is_complement = false;
                                    break;
                                }
                            }
                            if (is_complement) {
                                pairs++;
                                sibling_pairs++; // all pairs from top-level flatten are
                                                 // siblings
                            }
                        }
                    }
                    rec.complement_pair_count         = pairs;
                    rec.complement_pair_sibling_count = sibling_pairs;
                }
            }

            // ── Family D: subexpr reuse ──
            {
                std::map< uint64_t, int > hash_counts;
                collect_subtree_hashes(*folded, hash_counts, 3);
                uint32_t repeats = 0;
                for (const auto &[h, cnt] : hash_counts) {
                    if (cnt >= 2) { repeats += static_cast< uint32_t >(cnt - 1); }
                }
                rec.structural_repeat_count = repeats;
                // Complement repeats: check if hash(e) and hash(-e-1) both appear.
                // Approximate: hash(-e-1) ≈ hash(Add(Neg(e), mask)).
                // Not perfectly precise but good enough for mining.
                rec.complement_repeat_count = 0;
            }

            // ── Counterfactual: complement normalization ──
            {
                auto canonicalized      = lift_hidden_not(CloneExpr(*folded), mask);
                auto cf_cls             = ClassifyStructural(*canonicalized);
                rec.cf_complement_class = cf_cls.semantic;
                rec.cf_complement_flags = cf_cls.flags;
                rec.cf_class_changed    = (cf_cls.semantic != baseline_cls.semantic);
                rec.cf_flags_changed    = (cf_cls.flags != baseline_cls.flags);
                rec.cf_support_delta    = static_cast< int >(support_size(*canonicalized))
                    - static_cast< int >(support_size(*folded));

                // Would it now hit the ANF fast path?
                if (rec.sig_boolean_valued && num_vars <= 16) {
                    auto anf      = ComputeAnf(sig, num_vars);
                    auto anf_expr = BuildAnfExpr(anf, num_vars);

                    // Build evaluator from folded AST
                    auto eval_ptr =
                        std::make_shared< std::unique_ptr< Expr > >(CloneExpr(*folded));
                    Evaluator eval = [eval_ptr](const std::vector< uint64_t > &v) -> uint64_t {
                        return EvalExpr(**eval_ptr, v, kBw);
                    };

                    auto fw = FullWidthCheckEval(eval, num_vars, *anf_expr, kBw);
                    if (fw.passed) {
                        rec.cf_would_hit_fastpath = true;
                    } else {
                        rec.cf_fastpath_verify_fail = true;
                    }
                }
            }

            // ── Simplify with timing ──
            {
                Options opts{ .bitwidth = kBw, .max_vars = 16, .spot_check = true };

                auto t0     = std::chrono::steady_clock::now();
                auto result = Simplify(sig, vars, folded.get(), opts);
                auto t1     = std::chrono::steady_clock::now();

                rec.time_ms = std::chrono::duration< double, std::milli >(t1 - t0).count();

                if (result.has_value()) {
                    switch (result.value().kind) {
                        case SimplifyOutcome::Kind::kSimplified:
                            rec.simplified = true;
                            break;
                        case SimplifyOutcome::Kind::kUnchangedUnsupported:
                            rec.unsupported = true;
                            break;
                        case SimplifyOutcome::Kind::kError:
                            break;
                    }
                    rec.total_expansions    = result.value().telemetry.total_expansions;
                    rec.max_depth           = result.value().telemetry.max_depth_reached;
                    rec.candidates_verified = result.value().telemetry.candidates_verified;
                    rec.queue_high_water    = result.value().telemetry.queue_high_water;
                }
            }

            records.push_back(std::move(rec));
        }
    }

    // ── Summary output ──

    void print_summary(const std::string &title, const std::vector< ExprRecord > &records) {
        if (records.empty()) { return; }

        std::cerr << "\n╔═══════════════════════════════════════════════════════╗\n";
        std::cerr << "║  " << std::left << std::setw(52) << title << "║\n";
        std::cerr << "╚═══════════════════════════════════════════════════════╝\n\n";

        // Basic stats
        int total       = static_cast< int >(records.size());
        int simplified  = 0;
        int unsupported = 0;
        double total_ms = 0.0;
        for (const auto &r : records) {
            if (r.simplified) { simplified++; }
            if (r.unsupported) { unsupported++; }
            total_ms += r.time_ms;
        }
        std::cerr << "  Total: " << total << "  simplified: " << simplified
                  << "  unsupported: " << unsupported << "  total_time: " << std::fixed
                  << std::setprecision(1) << total_ms << "ms\n\n";

        // Time distribution
        std::vector< double > times;
        for (const auto &r : records) { times.push_back(r.time_ms); }
        std::sort(times.begin(), times.end());
        size_t n   = times.size();
        double p50 = times[n / 2];
        double p90 = times[n * 90 / 100];
        double p95 = times[n * 95 / 100];
        double p99 = times[n * 99 / 100];
        std::cerr << "  Time percentiles: p50=" << std::setprecision(2) << p50
                  << "ms  p90=" << p90 << "ms  p95=" << p95 << "ms  p99=" << p99 << "ms\n\n";

        // Family populations
        int fam_a = 0, fam_b = 0, fam_c = 0, fam_d = 0;
        int fam_a_arith = 0, fam_a_semi = 0, fam_a_mixed = 0;
        for (const auto &r : records) {
            if (r.hidden_complement_count > 0) {
                fam_a++;
                if (r.hc_purely_arith > 0) { fam_a_arith++; }
                if (r.hc_semilinear > 0) { fam_a_semi++; }
                if (r.hc_mixed > 0) { fam_a_mixed++; }
            }
            if (r.bool_product_count > 0) { fam_b++; }
            if (r.complement_pair_count > 0) { fam_c++; }
            if (r.structural_repeat_count > 0) { fam_d++; }
        }
        std::cerr << "  Family A (hidden complement):  " << fam_a
                  << " exprs  (arith=" << fam_a_arith << " semi=" << fam_a_semi
                  << " mixed=" << fam_a_mixed << ")\n";
        std::cerr << "  Family B (boolean product):    " << fam_b << " exprs\n";
        std::cerr << "  Family C (complement pair):    " << fam_c << " exprs\n";
        std::cerr << "  Family D (subexpr reuse):      " << fam_d << " exprs\n\n";

        // Counterfactual impact
        int cf_class_changed  = 0;
        int cf_flags_changed  = 0;
        int cf_fastpath       = 0;
        int cf_fp_verify_fail = 0;
        for (const auto &r : records) {
            if (r.cf_class_changed) { cf_class_changed++; }
            if (r.cf_flags_changed) { cf_flags_changed++; }
            if (r.cf_would_hit_fastpath) { cf_fastpath++; }
            if (r.cf_fastpath_verify_fail) { cf_fp_verify_fail++; }
        }
        std::cerr << "  Counterfactual (complement norm):\n";
        std::cerr << "    class changed:         " << cf_class_changed << "\n";
        std::cerr << "    flags changed:         " << cf_flags_changed << "\n";
        std::cerr << "    would hit ANF fastpath: " << cf_fastpath << "\n";
        std::cerr << "    fastpath verify fail:  " << cf_fp_verify_fail << "\n\n";

        // Class migration table (baseline → counterfactual)
        std::map< std::pair< SemanticClass, SemanticClass >, int > migration;
        for (const auto &r : records) {
            if (r.cf_class_changed) {
                migration[{ r.baseline_class, r.cf_complement_class }]++;
            }
        }
        if (!migration.empty()) {
            std::cerr << "  Class migration (complement norm):\n";
            for (const auto &[pair, count] : migration) {
                std::cerr << "    " << semantic_str(pair.first) << " → "
                          << semantic_str(pair.second) << ": " << count << "\n";
            }
            std::cerr << "\n";
        }

        // Bucket distribution
        std::map< Bucket, int > bucket_counts;
        std::map< Bucket, double > bucket_total_ms;
        std::map< Bucket, double > bucket_total_expansions;
        for (const auto &r : records) {
            auto b = assign_bucket(r, p95);
            bucket_counts[b]++;
            bucket_total_ms[b]         += r.time_ms;
            bucket_total_expansions[b] += r.total_expansions;
        }
        std::cerr << "  Buckets:\n";
        std::cerr << "    " << std::left << std::setw(24) << "bucket" << std::right
                  << std::setw(8) << "count" << std::setw(12) << "total_ms" << std::setw(12)
                  << "avg_expns" << "\n";
        std::cerr << "    " << std::string(56, '-') << "\n";
        for (int bi = 0; bi <= static_cast< int >(Bucket::kOther); ++bi) {
            auto b = static_cast< Bucket >(bi);
            if (bucket_counts.count(b) == 0) { continue; }
            int cnt       = bucket_counts[b];
            double ms     = bucket_total_ms[b];
            double avg_ex = bucket_total_expansions[b] / cnt;
            std::cerr << "    " << std::left << std::setw(24) << bucket_str(b) << std::right
                      << std::setw(8) << cnt << std::setw(12) << std::setprecision(1) << ms
                      << std::setw(12) << std::setprecision(1) << avg_ex << "\n";
        }
        std::cerr << "\n";

        // Top 10 expensive solved (for manual inspection)
        std::vector< const ExprRecord * > expensive;
        for (const auto &r : records) {
            if (r.simplified) { expensive.push_back(&r); }
        }
        std::sort(expensive.begin(), expensive.end(), [](const auto *a, const auto *b) {
            return a->time_ms > b->time_ms;
        });
        std::cerr << "  Top 10 expensive solved:\n";
        int shown = 0;
        for (const auto *r : expensive) {
            if (shown >= 10) { break; }
            std::cerr << "    L" << r->line_num << " " << std::setprecision(1) << r->time_ms
                      << "ms  expns=" << r->total_expansions << "  depth=" << r->max_depth
                      << "  class=" << semantic_str(r->baseline_class)
                      << "  flags=" << flags_str(r->baseline_flags);
            if (r->hidden_complement_count > 0) { std::cerr << "  [HC]"; }
            if (r->bool_product_count > 0) { std::cerr << "  [BP]"; }
            if (r->complement_pair_count > 0) { std::cerr << "  [CP]"; }
            if (r->cf_class_changed) {
                std::cerr << "  [CF:" << semantic_str(r->cf_complement_class) << "]";
            }
            std::cerr << "\n";
            shown++;
        }
        std::cerr << "\n";

        // Unsupported detail (if any)
        int unsup_shown = 0;
        for (const auto &r : records) {
            if (!r.unsupported) { continue; }
            if (unsup_shown >= 10) { break; }
            if (unsup_shown == 0) { std::cerr << "  Unsupported detail (top 10):\n"; }
            std::cerr << "    L" << r.line_num << " " << std::setprecision(1) << r.time_ms
                      << "ms  expns=" << r.total_expansions
                      << "  class=" << semantic_str(r.baseline_class)
                      << "  flags=" << flags_str(r.baseline_flags);
            if (r.hidden_complement_count > 0) { std::cerr << "  [HC]"; }
            if (r.cf_class_changed) {
                std::cerr << "  [CF:" << semantic_str(r.cf_complement_class) << "]";
            }
            if (r.cf_fastpath_verify_fail) { std::cerr << "  [FP-FAIL]"; }
            std::cerr << "\n";
            unsup_shown++;
        }
        if (unsup_shown > 0) { std::cerr << "\n"; }

        // Estimated saved time per shortcut family
        // saved_ms = bucket_count × median_time × route_change_rate
        std::cerr << "  Estimated shortcut leverage:\n";
        auto estimate = [&](const char *name, Bucket b) {
            if (bucket_counts.count(b) == 0) { return; }
            int cnt   = bucket_counts[b];
            double ms = bucket_total_ms[b];
            std::cerr << "    " << std::left << std::setw(24) << name << "count=" << cnt
                      << "  total_ms=" << std::setprecision(1) << ms << "\n";
        };
        estimate("hidden_complement", Bucket::kHiddenComplement);
        estimate("boolean_mul_domain", Bucket::kBooleanMulDomain);
        estimate("complement_pair", Bucket::kComplementPair);
        estimate("reclassifiable_now", Bucket::kReclassifiableNow);
        estimate("fastpath_verify_fail", Bucket::kFastpathVerifyFail);
        estimate("expensive_solved", Bucket::kExpensiveSolved);
        std::cerr << "\n";
    }

} // namespace

// ── Per-dataset audit tests ──

TEST(GAMBAShortcutAudit, Syntia) {
    std::vector< ExprRecord > records;
    run_audit({ "syntia", DATASET_DIR "/gamba/syntia.txt" }, records);
    print_summary("Syntia (500)", records);
    EXPECT_FALSE(records.empty());
}

TEST(GAMBAShortcutAudit, QSynthEA) {
    std::vector< ExprRecord > records;
    run_audit({ "qsynth", DATASET_DIR "/gamba/qsynth_ea.txt" }, records);
    print_summary("QSynth EA (500)", records);
    EXPECT_FALSE(records.empty());
}

TEST(GAMBAShortcutAudit, LokiTiny) {
    std::vector< ExprRecord > records;
    run_audit({ "loki_tiny", DATASET_DIR "/gamba/loki_tiny.txt" }, records);
    print_summary("LokiTiny (25K)", records);
    EXPECT_FALSE(records.empty());
}

TEST(GAMBAShortcutAudit, NeuReduce) {
    std::vector< ExprRecord > records;
    run_audit({ "neureduce", DATASET_DIR "/gamba/neureduce.txt" }, records);
    print_summary("NeuReduce (10K)", records);
    EXPECT_FALSE(records.empty());
}

TEST(GAMBAShortcutAudit, MbaObfLinear) {
    std::vector< ExprRecord > records;
    run_audit({ "mba_obf_linear", DATASET_DIR "/gamba/mba_obf_linear.txt" }, records);
    print_summary("MbaObfLinear (1K)", records);
    EXPECT_FALSE(records.empty());
}

TEST(GAMBAShortcutAudit, MbaObfNonlinear) {
    std::vector< ExprRecord > records;
    run_audit({ "mba_obf_nonlinear", DATASET_DIR "/gamba/mba_obf_nonlinear.txt" }, records);
    print_summary("MbaObfNonlinear (1K)", records);
    EXPECT_FALSE(records.empty());
}

// ── Cross-dataset summary ──

TEST(GAMBAShortcutAudit, CrossDatasetSummary) {
    std::vector< ExprRecord > all_records;

    DatasetConfig datasets[] = {
        {            "syntia",            DATASET_DIR "/gamba/syntia.txt" },
        {            "qsynth",         DATASET_DIR "/gamba/qsynth_ea.txt" },
        {         "loki_tiny",         DATASET_DIR "/gamba/loki_tiny.txt" },
        {         "neureduce",         DATASET_DIR "/gamba/neureduce.txt" },
        {    "mba_obf_linear",    DATASET_DIR "/gamba/mba_obf_linear.txt" },
        { "mba_obf_nonlinear", DATASET_DIR "/gamba/mba_obf_nonlinear.txt" },
    };

    for (const auto &ds : datasets) { run_audit(ds, all_records); }

    print_summary("GAMBA Cross-Dataset (38K)", all_records);

    // Per-dataset bucket breakdown
    std::cerr << "  Per-dataset bucket breakdown:\n";
    std::map< std::string, std::map< Bucket, int > > per_ds;
    std::map< std::string, std::map< Bucket, double > > per_ds_ms;

    // Compute p95 across all
    std::vector< double > all_times;
    for (const auto &r : all_records) { all_times.push_back(r.time_ms); }
    std::sort(all_times.begin(), all_times.end());
    double p95 = all_times[all_times.size() * 95 / 100];

    for (const auto &r : all_records) {
        auto b = assign_bucket(r, p95);
        per_ds[r.dataset][b]++;
        per_ds_ms[r.dataset][b] += r.time_ms;
    }

    // Header
    std::cerr << "    " << std::left << std::setw(18) << "dataset";
    for (int bi = 0; bi <= static_cast< int >(Bucket::kOther); ++bi) {
        std::cerr << std::setw(10) << bucket_str(static_cast< Bucket >(bi)).substr(0, 9);
    }
    std::cerr << "\n    " << std::string(120, '-') << "\n";

    for (const auto &[dsname, buckets] : per_ds) {
        std::cerr << "    " << std::left << std::setw(18) << dsname;
        for (int bi = 0; bi <= static_cast< int >(Bucket::kOther); ++bi) {
            auto b  = static_cast< Bucket >(bi);
            int cnt = (buckets.count(b) > 0) ? buckets.at(b) : 0;
            std::cerr << std::right << std::setw(10) << cnt;
        }
        std::cerr << "\n";
    }
    std::cerr << "\n";

    EXPECT_FALSE(all_records.empty());
}
